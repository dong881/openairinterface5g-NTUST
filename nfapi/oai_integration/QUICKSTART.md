# Timing Measurement Quick Start Guide

## 5-Minute Setup

### 1. Build

```bash
cd cmake_targets
./build_oai --gNB
```

**Note**: Timing measurement is now always enabled by default. No special build flags needed.

### 2. Add to Your Application

In your main softmodem file (e.g., `nr-softmodem.c`):

```c
// At the top
#include "nfapi/oai_integration/timing_measurement_init.h"

// In main(), after argument parsing
timing_measurement_global_init("nfapi", "same_machine", false, 
                              "/tmp/timing.json", 10000);
printf("[TIMING] Enabled - output: /tmp/timing.json\n");

// Before exit
timing_measurement_global_cleanup();
```

### 3. Run Your Application

```bash
./nr-softmodem [your normal options]
```

### 4. Analyze Results

```bash
# View session info
jq '.session_info' /tmp/timing.json

# View first measurement
jq '.measurements[0]' /tmp/timing.json

# Get average scheduler latency
jq '[.measurements[].latencies_us.scheduler_processing] | add/length' /tmp/timing.json

# View packet statistics
jq '.packet_statistics' /tmp/timing.json
```

## What Gets Measured?

- **Scheduler processing time**: Time spent in MAC scheduler
- **Message packing time**: Time to pack FAPI messages
- **Network transport**: Time for message to travel VNF → PNF
- **End-to-end latency**: Total time from slot indication to data transmission

## Output Format

Each measurement includes:
- Frame/slot numbers (SFN, slot)
- Message type (DL_TTI_REQUEST, TX_DATA_REQUEST, etc.)
- Raw timestamps in nanoseconds
- Calculated latencies in microseconds
- Status flags (dropped, retransmission, NACK)

## Performance Impact

- **CPU overhead**: < 0.1% per slot
- **Memory usage**: ~3 MB (configurable)
- **Runtime control**: Can be enabled/disabled via global_timing_ctx initialization

## More Information

- **Full API Documentation**: See `TIMING_MEASUREMENT_README.md`
- **Integration Examples**: See `INTEGRATION_EXAMPLE.md`
- **Architecture Details**: See `IMPLEMENTATION_SUMMARY.md`

## Common Issues

**Q: No JSON file created?**  
A: Check that `/tmp` is writable and initialization was successful

**Q: Empty or incomplete JSON?**  
A: Ensure `timing_measurement_global_cleanup()` is called before exit

**Q: High memory usage?**  
A: Reduce buffer size in initialization: `timing_measurement_global_init(..., 5000)`

**Q: Need more detail?**  
A: See the comprehensive documentation files in this directory

## Quick Analysis with Python

```python
import json
import pandas as pd

# Load data
with open('/tmp/timing.json') as f:
    data = json.load(f)

# Create DataFrame
df = pd.DataFrame(data['measurements'])
latencies = pd.json_normalize(df['latencies_us'])

# Print statistics
print(latencies.describe())

# Find slowest measurements
slowest = df.loc[latencies['total_latency'].nlargest(10).index]
print(slowest[['sfn', 'slot', 'message_type']])
```

## Next Steps

1. Review measurement points in your specific scenario
2. Adjust buffer size and flush interval if needed
3. Add custom analysis scripts for your use case
4. Consider adding more measurement points (see `timing_measurement.h`)

For production deployments, remember to rebuild without `-DENABLE_TIMING_MEASUREMENT=ON` to eliminate all overhead.
