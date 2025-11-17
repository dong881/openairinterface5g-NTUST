# FAPI/nFAPI Latency Measurement System

## Overview

This implementation provides detailed latency measurement for OpenAirInterface's FAPI and nFAPI modes with JSON output for analysis. The system captures timestamps at key points in the message flow from VNF to PNF and outputs comprehensive timing data.

## Building with Timing Measurement

### Enable Timing Measurement

Timing measurement is now always enabled in the build. Simply build as normal:

```bash
cd cmake_targets
./build_oai --gNB --nrUE
```

Or manually in CMake:

```bash
mkdir -p build && cd build
cmake ../..
make
```

**Note**: The `ENABLE_TIMING_MEASUREMENT` CMake option is kept for backwards compatibility but has no effect.

## Initialization

The timing measurement system must be initialized before use. Add the following to your application startup code:

```c
#include "nfapi/oai_integration/timing_measurement_init.h"

// Initialize at application startup
timing_measurement_global_init(
    "nfapi",                          // mode: "fapi" or "nfapi"
    "same_machine",                   // deployment: "same_machine" or "different_machine"
    false,                            // ptp_sync: true if using PTP synchronization
    "/tmp/timing_measurements.json",  // JSON output file path
    10000                             // buffer size (0 for default)
);

// ... run application ...

// Cleanup before exit
timing_measurement_global_cleanup();
```

## Measurement Points

The system captures timestamps at the following points:

### PNF (Physical Network Function) Side
- **TIMING_POINT_PNF_SLOT_INDICATION_SEND**: Slot indication sent from PNF
- **TIMING_POINT_HARQ_FEEDBACK_RECEIVED**: UCI indication (HARQ feedback) received

### VNF (Virtual Network Function) Side
- **TIMING_POINT_SOCKET_RECEIVE**: Message received from PNF
- **TIMING_POINT_SCHEDULER_START**: Scheduler processing started
- **TIMING_POINT_SCHEDULER_END**: Scheduler processing completed
- **TIMING_POINT_SCHEDULED_DATA_PACK_START**: DL/UL message packing started
- **TIMING_POINT_SCHEDULED_DATA_PACK_END**: DL/UL message packing completed
- **TIMING_POINT_VNF_TO_PNF_SOCKET**: Message sent to PNF
- **TIMING_POINT_MESSAGE_PACK**: TX data message packing

### Additional Points (for future implementation)
- **TIMING_POINT_MESSAGE_UNPACK_START**: Message unpacking started
- **TIMING_POINT_MESSAGE_UNPACK_END**: Message unpacking completed
- **TIMING_POINT_BUFFER_ENQUEUE**: Message enqueued to buffer
- **TIMING_POINT_BUFFER_DEQUEUE**: Message dequeued from buffer
- **TIMING_POINT_TX_FUNC_START**: PHY TX function started
- **TIMING_POINT_FRONTHAUL_TX**: Data transmitted to fronthaul/radio

## JSON Output Format

The system outputs timing data in JSON format with the following structure:

```json
{
  "session_info": {
    "mode": "nfapi",
    "start_time": "1700000000",
    "deployment": "same_machine",
    "ptp_sync": false
  },
  "measurements": [
    {
      "sfn": 100,
      "slot": 5,
      "message_type": "DL_TTI_REQUEST",
      "rnti": 0,
      "harq_process_id": 0,
      "timestamps": {
        "socket_receive": 1234567890125000,
        "scheduler_start": 1234567890126000,
        "scheduler_end": 1234567890127000,
        "scheduled_data_pack_start": 1234567890127100,
        "scheduled_data_pack_end": 1234567890127800,
        "vnf_to_pnf_socket": 1234567890128000
      },
      "latencies_us": {
        "pack_latency": 0.0,
        "network_transport": 0.0,
        "unpack_latency": 0.0,
        "scheduler_processing": 1.000,
        "data_pack_latency": 0.700,
        "vnf_to_pnf_transport": 0.200,
        "buffer_wait_time": 0.0,
        "phy_processing": 0.0,
        "total_latency": 1.900
      },
      "flags": {
        "dropped": false,
        "retransmission": false,
        "nack_received": false
      }
    }
  ],
  "packet_statistics": {
    "DL_TTI_REQUEST": {
      "total": 1000,
      "dropped": 0,
      "late": 0
    },
    "UL_TTI_REQUEST": {
      "total": 1000,
      "dropped": 0,
      "late": 0
    },
    "TX_DATA_REQUEST": {
      "total": 800,
      "dropped": 0,
      "late": 0
    }
  }
}
```

## Message Types Tracked

- **DL_TTI_REQUEST**: Downlink TTI configuration request
- **UL_TTI_REQUEST**: Uplink TTI configuration request
- **UL_DCI_REQUEST**: Uplink DCI request
- **TX_DATA_REQUEST**: Transmit data request
- **SLOT_INDICATION**: Slot timing indication
- **UCI_INDICATION**: Uplink control information (includes HARQ feedback)
- **CRC_INDICATION**: CRC check result
- **RX_DATA_INDICATION**: Received data indication
- **RACH_INDICATION**: Random access channel indication
- **SRS_INDICATION**: Sounding reference signal indication

## Performance Impact

The timing measurement system is designed to have minimal performance impact:

- Always available but can be controlled at runtime via initialization
- Employs circular buffer to limit memory usage
- Implements buffered JSON writing to reduce I/O overhead
- Only captures timestamps (< 100ns per measurement point)
- Auto-flush mechanism prevents buffer overflow

## API Usage

### Recording a Timestamp

```c
timing_measurement_t *measurement = TIMING_START(global_timing_ctx, sfn, slot, 
                                                 FAPI_MSG_DL_TTI_REQUEST, rnti, harq_id);
TIMING_RECORD(global_timing_ctx, measurement, TIMING_POINT_SCHEDULER_START);
// ... do work ...
TIMING_RECORD(global_timing_ctx, measurement, TIMING_POINT_SCHEDULER_END);
```

### Updating Statistics

```c
TIMING_UPDATE_STATS(global_timing_ctx, FAPI_MSG_DL_TTI_REQUEST, dropped, late);
```

## Analyzing Results

The JSON output can be analyzed using various tools:

1. **Python/pandas**: Load and analyze timing data
2. **jq**: Query specific measurements from command line
3. **Visualization tools**: Plot latency distributions, identify bottlenecks

Example jq query to get average scheduler latency:

```bash
jq '[.measurements[].latencies_us.scheduler_processing] | add/length' timing_measurements.json
```

## Extending the System

To add new measurement points:

1. Add a new enum value to `timing_point_t` in `timing_measurement.h`
2. Add the point name to `timing_point_names[]` in `timing_measurement.c`
3. Add `TIMING_RECORD()` calls at the desired locations
4. Update latency calculation in `write_measurement_json()` if needed

## Limitations

- Currently tracks one measurement per frame/slot/message type combination
- HARQ tracking implementation is basic (structures defined but not fully integrated)
- Some measurement points (e.g., buffer enqueue/dequeue) not yet instrumented
- Packet loss/drop detection logic not fully implemented

## Future Enhancements

- Complete HARQ retransmission tracking
- Add packet loss/drop detection
- Implement sampling modes (e.g., every N frames)
- Add real-time statistics query API
- Support for multiple parallel measurements per slot
- Integration with existing OAI logging system
