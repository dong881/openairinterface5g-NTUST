# Timing Measurement Integration Example

## Overview

This document provides a step-by-step example of how to integrate the timing measurement system into an OpenAirInterface softmodem application.

## Step 1: Build Configuration

Build OAI normally (timing measurement is now always enabled):

```bash
cd cmake_targets
./build_oai --gNB --nrUE
```

## Step 2: Add Headers to Softmodem

In your main softmodem file (e.g., `executables/nr-softmodem.c`), add the timing measurement header:

```c
#include "nfapi/oai_integration/timing_measurement_init.h"
```

## Step 3: Add Command-Line Options (Optional)

If you want to add command-line options for timing measurement:

```c
// Add to paramdef structure
static paramdef_t timing_params[] = {
  {"timing_enabled", "T", PARAMFLAG_BOOL, .iptr = &timing_enabled, .defintval = 0, TYPE_INT, 0},
  {"timing_json_file", "J", 0, .strptr = &timing_json_file, .defstrval = "/tmp/timing_measurements.json", TYPE_STRING, 0},
  {"timing_buffer_size", "B", 0, .uptr = &timing_buffer_size, .defintval = 10000, TYPE_UINT, 0},
};

// Parse options
config_get(timing_params, sizeof(timing_params)/sizeof(paramdef_t), CONFIG_STRING_TIMING);
```

## Step 4: Initialize at Application Startup

Add initialization code after argument parsing but before main loop:

```c
int main(int argc, char **argv) {
  // ... existing initialization code ...
  
  // Parse command-line arguments
  // ... 
  
  // Initialize timing measurement system
  const char *mode = "nfapi";  // or "fapi" for monolithic mode
  const char *deployment = "same_machine";  // or "different_machine"
  bool ptp_sync = false;  // Set to true if using PTP synchronization
  const char *json_file = "/tmp/timing_measurements.json";
  uint32_t buffer_size = 10000;
  
  timing_measurement_global_init(mode, deployment, ptp_sync, json_file, buffer_size);
  
  printf("[TIMING] Measurement system initialized - output: %s\n", json_file);
  
  // ... continue with main application ...
}
```

## Step 5: Cleanup Before Exit

Add cleanup code in the shutdown path:

```c
void cleanup_and_exit(void) {
  // ... existing cleanup code ...
  
  printf("[TIMING] Flushing measurement data and cleaning up...\n");
  timing_measurement_global_cleanup();
  
  exit(0);
}
```

## Step 6: Signal Handler Integration

For proper cleanup on signals (SIGINT, SIGTERM):

```c
void signal_handler(int sig) {
  printf("Received signal %d, cleaning up...\n", sig);
  
  timing_measurement_global_cleanup();
  
  exit(sig);
}

// In main()
signal(SIGINT, signal_handler);
signal(SIGTERM, signal_handler);
```

## Complete Example: nr-softmodem.c Integration

Here's a complete minimal example:

```c
// At the top of the file
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "nfapi/oai_integration/timing_measurement_init.h"

// Global variables for timing configuration
static char timing_json_file[256] = "/tmp/oai_timing_measurements.json";
static uint32_t timing_buffer_size = 10000;
static bool timing_enabled = false;

// Signal handler
static void signal_handler(int sig) {
  printf("Received signal %d, cleaning up...\n", sig);
  
  if (timing_enabled) {
    printf("[TIMING] Flushing measurement data...\n");
    timing_measurement_global_cleanup();
  }
  
  exit(sig);
}

int main(int argc, char **argv) {
  // Install signal handlers
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  
  // ... parse command-line arguments ...
  
#ifdef ENABLE_TIMING_MEASUREMENT
  // Check if timing measurement is requested
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--timing-measurement") == 0) {
      timing_enabled = true;
    } else if (strcmp(argv[i], "--timing-json-file") == 0 && i + 1 < argc) {
      strncpy(timing_json_file, argv[++i], sizeof(timing_json_file) - 1);
    } else if (strcmp(argv[i], "--timing-buffer-size") == 0 && i + 1 < argc) {
      timing_buffer_size = atoi(argv[++i]);
    }
  }
  
  if (timing_enabled) {
    // Determine mode based on configuration
    const char *mode = "nfapi";  // TODO: detect from config
    const char *deployment = "same_machine";  // TODO: detect from config
    bool ptp_sync = false;  // TODO: detect from config
    
    timing_measurement_global_init(mode, deployment, ptp_sync, 
                                  timing_json_file, timing_buffer_size);
    
    printf("[TIMING] Measurement system initialized\n");
    printf("[TIMING]   Mode: %s\n", mode);
    printf("[TIMING]   Deployment: %s\n", deployment);
    printf("[TIMING]   PTP sync: %s\n", ptp_sync ? "yes" : "no");
    printf("[TIMING]   JSON output: %s\n", timing_json_file);
    printf("[TIMING]   Buffer size: %u\n", timing_buffer_size);
  }
  
  // ... run main application loop ...
  
  // Cleanup before exit
  if (timing_enabled) {
    timing_measurement_global_cleanup();
  }
  
  return 0;
}
```

## Running with Timing Measurement

### Basic Usage

```bash
./nr-softmodem --timing-measurement --timing-json-file /tmp/my_timing.json
```

### With Custom Buffer Size

```bash
./nr-softmodem --timing-measurement --timing-json-file /tmp/my_timing.json --timing-buffer-size 20000
```

### Analyzing Results

After running, analyze the JSON output:

```bash
# View first 10 measurements
jq '.measurements[:10]' /tmp/my_timing.json

# Get average scheduler latency
jq '[.measurements[].latencies_us.scheduler_processing] | add/length' /tmp/my_timing.json

# Get packet statistics
jq '.packet_statistics' /tmp/my_timing.json

# Filter measurements for a specific slot
jq '.measurements[] | select(.slot == 5)' /tmp/my_timing.json
```

## Python Analysis Example

```python
import json
import pandas as pd
import matplotlib.pyplot as plt

# Load timing data
with open('/tmp/my_timing.json', 'r') as f:
    data = json.load(f)

# Convert measurements to DataFrame
measurements_df = pd.DataFrame(data['measurements'])

# Extract latency information
latencies_df = pd.json_normalize(measurements_df['latencies_us'])

# Plot scheduler latency distribution
plt.figure(figsize=(10, 6))
plt.hist(latencies_df['scheduler_processing'], bins=50)
plt.xlabel('Scheduler Processing Time (μs)')
plt.ylabel('Frequency')
plt.title('Scheduler Latency Distribution')
plt.savefig('scheduler_latency.png')

# Print statistics
print("Scheduler Latency Statistics (μs):")
print(latencies_df['scheduler_processing'].describe())

# Print packet statistics
print("\nPacket Statistics:")
for msg_type, stats in data['packet_statistics'].items():
    print(f"{msg_type}:")
    print(f"  Total: {stats['total']}")
    print(f"  Dropped: {stats['dropped']}")
    print(f"  Late: {stats['late']}")
```

## Notes

1. The timing measurement system uses `CLOCK_MONOTONIC` for timestamps, which is not affected by system time adjustments.

2. For cross-machine deployment with PTP sync, ensure PTP is properly configured before enabling timing measurement.

3. The circular buffer prevents memory growth. When the buffer is full, oldest measurements are overwritten. Adjust `buffer_size` based on your needs.

4. The system auto-flushes to the JSON file periodically (default: every 1000 measurements). You can adjust this in `timing_measurement.c`.

5. To minimize performance impact, only enable timing measurement during testing/debugging phases, not in production deployments.

## Troubleshooting

### No JSON File Created

- Check that the output directory exists and is writable
- Verify that `global_timing_ctx` is properly initialized
- Check for error messages during initialization

### Empty JSON File

- Ensure measurements are actually being recorded (check instrumentation points)
- Verify that cleanup is called to flush remaining data
- Check that auto-flush is working (add debug prints)

### Performance Impact

- Reduce buffer size if memory is constrained
- Increase flush interval to reduce I/O overhead
- Consider sampling mode (e.g., record every 10th slot)

## Additional Resources

- See `TIMING_MEASUREMENT_README.md` for API documentation
- Check `timing_measurement.h` for available timing points
- Review `timing_measurement.c` for implementation details
