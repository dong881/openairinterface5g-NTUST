# FAPI/nFAPI Latency Measurement Implementation Summary

## Overview

This implementation provides a comprehensive latency measurement system for OpenAirInterface's FAPI and nFAPI interfaces. The system captures high-precision timestamps at critical points in the message flow and outputs detailed timing data in JSON format for analysis.

## Implementation Approach

### Design Principles

1. **Minimal Code Changes**: Uses conditional compilation to minimize impact when disabled
2. **Zero Runtime Cost When Disabled**: Complete compile-time elimination via `#ifdef`
3. **Low Overhead When Enabled**: Circular buffer, batched I/O, inline functions
4. **Modular Design**: Separate initialization, core logic, and instrumentation
5. **Standard Compliance**: Uses POSIX `CLOCK_MONOTONIC` for reliable timestamps

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     Application (Softmodem)                     │
│  ┌────────────────────────────────────────────────────────┐    │
│  │        timing_measurement_global_init()                │    │
│  │        timing_measurement_global_cleanup()             │    │
│  └────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│              Timing Measurement Infrastructure                  │
│  ┌─────────────────┐     ┌──────────────────┐                 │
│  │ timing_         │────▶│ Circular Buffer  │                 │
│  │ measurement.h   │     │ (measurements)   │                 │
│  └─────────────────┘     └──────────────────┘                 │
│  ┌─────────────────┐     ┌──────────────────┐                 │
│  │ timing_         │────▶│ JSON Writer      │───▶ output.json │
│  │ measurement.c   │     │ (buffered I/O)   │                 │
│  └─────────────────┘     └──────────────────┘                 │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Instrumentation Points                       │
│  ┌───────────────┐              ┌──────────────────┐          │
│  │  PNF Side     │              │   VNF Side       │          │
│  │  - Slot Ind   │              │   - Scheduler    │          │
│  │  - UCI Ind    │              │   - DL/UL/TX Req │          │
│  └───────────────┘              └──────────────────┘          │
└─────────────────────────────────────────────────────────────────┘
```

## Key Components

### 1. Core Data Structures

**File**: `nfapi/open-nFAPI/common/public_inc/timing_measurement.h`

- `timing_measurement_t`: Single measurement record with timestamps for all points
- `timing_measurement_context_t`: Global context managing buffer, statistics, JSON output
- `harq_tracking_entry_t`: HARQ process tracking (for future use)
- `packet_stats_t`: Per-message-type packet statistics

### 2. Implementation Logic

**File**: `nfapi/open-nFAPI/common/src/timing_measurement.c`

- Initialization and cleanup routines
- Circular buffer management
- JSON output with calculated latencies
- Packet statistics tracking
- Auto-flush mechanism

### 3. Initialization Wrapper

**Files**: 
- `nfapi/oai_integration/timing_measurement_init.h`
- `nfapi/oai_integration/timing_measurement_init.c`

Provides simple initialization interface for applications with stub implementations when disabled.

### 4. Instrumentation Points

**Files Modified**:
- `nfapi/oai_integration/nfapi_pnf.c`: PNF-side measurement points
- `nfapi/oai_integration/nfapi_vnf.c`: VNF-side measurement points

## Measurement Flow

### Typical Message Flow with Timing Points

```
VNF (MAC Scheduler)                                    PNF (PHY)
─────────────────                                      ─────────
                                                            │
                                                            │ (1) PNF_SLOT_INDICATION_SEND
                                                            ▼
                        ◀─────── Slot Indication ──────────
                        │
(2) SOCKET_RECEIVE     │
(3) SCHEDULER_START    │
                        │
    [Scheduler runs]    │
                        │
(4) SCHEDULER_END      │
(5) SCHEDULED_DATA_    │
    PACK_START          │
                        │
    [Pack DL_TTI.req]   │
                        │
(6) SCHEDULED_DATA_    │
    PACK_END            │
(7) VNF_TO_PNF_SOCKET  │
                        ├────── DL_TTI.request ──────────▶
                        │
(8) MESSAGE_PACK       │
                        ├────── TX_DATA.request ─────────▶
                        │
                        │
                        ◀──────── UCI.indication ──────────
                        │                                    │
                        │                        (9) HARQ_FEEDBACK_RECEIVED
                        │
```

## Implemented Measurement Points

### Currently Instrumented

| Point | Location | Description |
|-------|----------|-------------|
| `PNF_SLOT_INDICATION_SEND` | `nfapi_pnf.c:oai_nfapi_nr_slot_indication()` | Slot indication sent from PNF |
| `HARQ_FEEDBACK_RECEIVED` | `nfapi_pnf.c:oai_nfapi_nr_uci_indication()` | UCI indication received at PNF |
| `SOCKET_RECEIVE` | `nfapi_vnf.c:trigger_scheduler()` | Slot indication received at VNF |
| `SCHEDULER_START` | `nfapi_vnf.c:trigger_scheduler()` | Before scheduler call |
| `SCHEDULER_END` | `nfapi_vnf.c:trigger_scheduler()` | After scheduler returns |
| `SCHEDULED_DATA_PACK_START` | `nfapi_vnf.c:oai_nfapi_dl_tti_req()` | Before DL_TTI packing |
| `SCHEDULED_DATA_PACK_END` | `nfapi_vnf.c:oai_nfapi_dl_tti_req()` | After DL_TTI packing |
| `VNF_TO_PNF_SOCKET` | `nfapi_vnf.c:oai_nfapi_*_req()` | Message sent to PNF |
| `MESSAGE_PACK` | `nfapi_vnf.c:oai_nfapi_tx_data_req()` | TX data packing |

### Planned for Future Implementation

- `MESSAGE_UNPACK_START/END`: nFAPI message unpacking at PNF
- `BUFFER_ENQUEUE/DEQUEUE`: Buffer operations in nFAPI stack
- `TX_FUNC_START`: PHY layer TX function entry
- `FRONTHAUL_TX`: Transmission to radio hardware

## Statistics Tracked

The system maintains per-message-type statistics:

- **Total**: Total number of messages processed
- **Dropped**: Messages marked as dropped
- **Late**: Messages that arrived late

Currently tracked message types:
- `DL_TTI_REQUEST`
- `UL_TTI_REQUEST`
- `TX_DATA_REQUEST`
- `SLOT_INDICATION`
- `UCI_INDICATION`
- And others (see `fapi_message_type_t` enum)

## Performance Characteristics

### Memory Usage

- **Per measurement**: ~280 bytes (16 timestamps + metadata)
- **Default buffer**: 10,000 measurements = ~2.7 MB
- **HARQ table**: 1,000 entries × ~200 bytes = ~200 KB
- **Total**: ~3 MB (configurable)

### CPU Overhead

- **Per measurement point**: ~50-100 ns (clock_gettime + array write)
- **Per message**: ~6-8 measurement points = ~600 ns
- **Typical slot**: Multiple messages = ~2-5 μs
- **Percentage**: < 0.1% of slot time (assuming 1ms slot)

### I/O Impact

- **Buffered writes**: Reduces syscall overhead
- **Auto-flush**: Every 1000 measurements (configurable)
- **Async I/O**: JSON writing doesn't block measurement recording

## JSON Output Specification

### Session Information
- Mode: "fapi" or "nfapi"
- Start time: Unix timestamp
- Deployment: "same_machine" or "different_machine"
- PTP sync: Boolean flag

### Measurement Records
Each record contains:
- Frame/slot identification (SFN, slot)
- Message type
- RNTI, HARQ process ID
- Raw timestamps (nanoseconds)
- Calculated latencies (microseconds)
- Status flags (dropped, retransmission, NACK)

### Packet Statistics
Per-message-type counters for:
- Total messages
- Dropped messages
- Late messages

## Build Integration

### CMake Option

Added to `CMakeLists.txt`:
```cmake
add_boolean_option(ENABLE_TIMING_MEASUREMENT OFF "Enable detailed timing measurement for FAPI/nFAPI with JSON output" ON)
```

### Library Integration

Modified `nfapi/open-nFAPI/common/CMakeLists.txt`:
- Added `timing_measurement.c` to sources
- Added conditional compile definition

### Build Commands

```bash
# Enable timing measurement
cmake -DENABLE_TIMING_MEASUREMENT=ON ...

# Or via build_oai script
./build_oai --gNB --cmake-opt -DENABLE_TIMING_MEASUREMENT=ON
```

## Testing Strategy

### Unit Testing
1. Compile timing_measurement.c standalone
2. Verify data structure sizes
3. Test circular buffer behavior
4. Validate JSON output format

### Integration Testing
1. Build with timing measurement enabled
2. Run basic scenarios
3. Verify measurement points are hit
4. Check JSON output validity
5. Measure performance impact

### Performance Testing
1. Compare throughput with/without timing measurement
2. Measure latency overhead per measurement point
3. Verify memory usage stays within bounds
4. Test auto-flush mechanism

## Known Limitations

1. **Single Measurement Per Slot**: Current implementation tracks one measurement per frame/slot/message combination
2. **HARQ Tracking Incomplete**: Data structures defined but tracking logic not fully implemented
3. **Missing Points**: Some measurement points (buffer operations, PHY layer) not yet instrumented
4. **No Sampling Mode**: Always records every message (no selective sampling)
5. **Fixed Buffer Size**: Must be configured at startup, not dynamically adjustable

## Future Enhancements

### Short Term
1. Complete HARQ retransmission tracking
2. Add remaining measurement points (unpack, buffer, PHY)
3. Implement packet loss/drop detection logic
4. Add command-line option parsing to softmodems

### Medium Term
1. Implement sampling modes (every Nth frame)
2. Add real-time statistics query API
3. Support multiple concurrent measurements per slot
4. Integration with OAI logging system (LOG_D, LOG_I, etc.)

### Long Term
1. Distributed tracing support (span IDs across VNF/PNF)
2. Binary output format for reduced overhead
3. Real-time visualization dashboard
4. Automated anomaly detection
5. Integration with performance monitoring tools (Prometheus, Grafana)

## Documentation

### User Documentation
- **TIMING_MEASUREMENT_README.md**: Comprehensive user guide
- **INTEGRATION_EXAMPLE.md**: Step-by-step integration examples
- **IMPLEMENTATION_SUMMARY.md**: This document

### Code Documentation
- **timing_measurement.h**: API documentation in header comments
- **timing_measurement.c**: Implementation notes and design decisions
- **Inline comments**: Throughout instrumented code

## References

1. SCF-225: 5G nFAPI Specification
2. SCF-222: 5G FAPI PHY API Specification  
3. NFAPI P7 Delay Management Development Manual
4. OpenAirInterface Documentation
5. POSIX clock_gettime() specification

## Conclusion

This implementation provides a comprehensive, low-overhead timing measurement system for OpenAirInterface's FAPI/nFAPI interfaces. The modular design allows easy extension and customization while maintaining minimal impact on performance when disabled. The JSON output format enables flexible analysis and visualization of timing data for debugging, optimization, and validation purposes.

## Contributors

Implementation based on requirements specification provided by the OpenAirInterface community and developed following OAI coding standards and practices.
