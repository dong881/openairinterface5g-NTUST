# Dynamic Timing Adjustment Algorithm for VNF Scheduler

## Overview

This document describes the dynamic timing adjustment algorithm implemented to ensure stable message delivery from the VNF to PNF, even when scheduler and message packing times vary.

## Problem Statement

In the NFAPI VNF-PNF split architecture, the VNF must send downlink messages (DL_TTI, UL_TTI, TX_DATA, UL_DCI) to the PNF at precise times. The timing synchronization mechanism uses `us_adjustment` and `slot_adjustment` to align the VNF's internal clock with the PNF's expected timing.

However, the original implementation only considered the network timing offset, without accounting for:
1. Variable scheduler execution time (`gNB_dlsch_ulsch_scheduler`)
2. Variable message packing and transmission time
3. The fact that these processing times can change dynamically based on system load and message complexity

This led to instability in message arrival times at the PNF, especially when processing times varied significantly.

## Solution: Dynamic Timing Adjustment Algorithm

### Key Insight

The adjustment variables (`us_adjustment` and `slot_adjustment`) determine when messages are sent. To ensure messages arrive at the expected time, we must account for the total processing time from when `trigger_scheduler` starts until messages are sent.

### Algorithm Components

#### 1. Processing Time Tracking

We track three statistics for the total processing time:

- **Maximum (`proc_time_max_us`)**: Highest processing time observed
- **Simple Moving Average (`proc_time_avg_us`)**: Average over all samples
- **Exponentially Weighted Moving Average (`proc_time_ewma_us`)**: Weighted average that gives more weight to recent samples

**EWMA Formula:**
```
EWMA_new = (new_value + 7 * EWMA_old) / 8
```
This uses α = 1/8 = 0.125, providing smooth but responsive tracking.

#### 2. Dynamic Margin Adjustment

The effective timing margin is calculated as:

```
effective_margin = base_margin + processing_time_buffer
```

Where:
- `base_margin = 500 * (2 << mu)` microseconds (varies by numerology)
  - mu=0: 1000 μs
  - mu=1: 2000 μs  
  - mu=2: 4000 μs
  - mu=3: 8000 μs
- `processing_time_buffer = proc_time_ewma_us * 1.2` (EWMA with 20% safety factor)

The 1.2x safety factor accounts for occasional spikes above the EWMA.

#### 3. Adjustment Calculation

The slot and microsecond adjustments are calculated using the effective margin:

```c
int32_t offsetslot = (offset + effective_margin) / slot_us;
int32_t offsetus = (offset + effective_margin) % slot_us;
```

These adjustments shift the timing so that messages are sent early enough to complete processing and arrive at the PNF at the expected time.

### Implementation Details

#### Modified Files

1. **nfapi/open-nFAPI/vnf/inc/vnf_p7.h**
   - Added processing time tracking fields to `nfapi_vnf_p7_connection_info_t`

2. **nfapi/oai_integration/nfapi_vnf.c**
   - Added `update_processing_time_stats()` function
   - Modified `trigger_scheduler()` to track and report processing time
   - Modified `vnf_timing_thread()` to initialize tracking fields

3. **nfapi/open-nFAPI/vnf/src/vnf_p7.c**
   - Modified `vnf_handle_ul_node_sync()` to use dynamic effective margin

#### Key Functions

**`update_processing_time_stats(p7_info, proc_time_us)`**

Updates all three statistics (max, average, EWMA) based on the current processing time sample.

**`vnf_handle_ul_node_sync()`**

Calculates timing adjustments using:
1. Network timing offset (from t1, t2, t3, t4 timestamps)
2. Processing time buffer (from EWMA)
3. Combines these into effective margin for adjustment calculation

## How It Works: Step by Step

### 1. Timing Measurement

Every slot, `trigger_scheduler()`:
1. Records start time
2. Calls scheduler and message packing functions
3. Records end time
4. Calculates total processing time
5. Updates statistics via `update_processing_time_stats()`

### 2. Periodic Sync Messages

Every N slots (default: 2), the VNF sends a DL_NODE_SYNC message to the PNF with timestamp t1.

### 3. PNF Response

The PNF receives the sync message at time t2, processes it, and sends back an UL_NODE_SYNC at time t3.

### 4. VNF Processing

When the VNF receives UL_NODE_SYNC at time t4, it:
1. Calculates timing offset: `offset = ((t2 - t1) - (t4 - t3)) / 2`
2. Gets current processing time EWMA
3. Calculates effective margin: `base_margin + (EWMA * 1.2)`
4. Computes adjustments using effective margin
5. Sets `us_adjustment` and `slot_adjustment`

### 5. Adjustment Application

In the next slot timing cycle:
1. `us_adjustment` shifts the slot timing phase
2. `slot_adjustment` shifts which slot number is being processed
3. These ensure messages are sent early enough to account for processing time

## Benefits

### 1. Stability Under Variable Load
The algorithm adapts to changing processing times, maintaining stable message delivery even when:
- System load varies
- Message complexity changes (different numbers of PDUs)
- Scheduler decisions take more or less time

### 2. Smooth Response
The EWMA provides smooth tracking that:
- Responds to sustained changes in processing time
- Filters out occasional spikes
- Avoids over-correction

### 3. Safety Margin
The 1.2x safety factor ensures that even when processing time occasionally exceeds the EWMA, messages still arrive on time.

### 4. Backward Compatibility
The algorithm can be disabled by setting `dynamic_adj_enabled = 0`, reverting to the original fixed-margin behavior.

## Monitoring and Debugging

### Log Messages

The algorithm generates several types of log messages:

**Processing Time Statistics (periodic):**
```
[P7_SYNC][STATS] sfn:slot 100:5 proc_time: max=450 avg=320 ewma=325 samples=200
```

**Dynamic Adjustment Calculation:**
```
[P7_SYNC][DYNAMIC] phy_id:1 proc_ewma:325 proc_max:450 proc_avg:320 buffer:390 eff_margin:1390
```

**Sync Message Details:**
```
[P7_SYNC] ul_node_sync phy_id:1 offset:-50 owd:25 slot_adj:0 us_adj:50 eff_mgn:1390 locked:0
```

### Memory-Mapped Log Files

Processing times are also logged to memory-mapped files for detailed analysis:
- `[SCHED]<time>`: Scheduler execution time
- `[DL_TTI]<time>,<pdus>`: DL_TTI packing time and PDU count
- `[UL_TTI]<time>,<pdus>`: UL_TTI packing time and PDU count
- `[TX_DATA]<time>,<pdus>`: TX_DATA packing time and PDU count
- `[UL_DCI]<time>,<pdus>`: UL_DCI packing time and PDU count
- `[TOTAL]<time>`: Total processing time

## Configuration

### Enable/Disable Dynamic Adjustment

In `vnf_timing_thread()`:
```c
p7_info->dynamic_adj_enabled = 1;  // 1 = enabled, 0 = disabled
```

### Adjust Safety Factor

In `vnf_handle_ul_node_sync()`:
```c
int32_t proc_time_buffer = (p7_info->proc_time_ewma_us * 12) / 10;  // 1.2x factor
```

Change the multiplier (12/10) to adjust the safety margin.

### Adjust EWMA Alpha

In `update_processing_time_stats()`:
```c
p7_info->proc_time_ewma_us = (proc_time_us + 7 * p7_info->proc_time_ewma_us) / 8;
```

Change the ratio (7:1) to adjust responsiveness:
- Higher ratio (e.g., 15:1) → smoother, slower response
- Lower ratio (e.g., 3:1) → faster, more reactive response

## Performance Considerations

### Memory Overhead
- Adds 5 int32_t fields (20 bytes) per P7 connection
- Negligible impact on overall memory usage

### CPU Overhead
- Statistics update: ~5 arithmetic operations per slot
- EWMA calculation: 2 multiplications, 1 division per slot
- Negligible impact on overall CPU usage

### Convergence Time
- Initial convergence: 8-16 slots (depends on EWMA alpha)
- Adaptation to changes: 4-8 slots for 90% convergence

## Testing and Validation

### Recommended Test Scenarios

1. **Steady State Load**: Verify stable operation under constant load
2. **Varying Load**: Test adaptation to increasing/decreasing message complexity
3. **Burst Traffic**: Verify behavior during sudden traffic spikes
4. **Long Duration**: Confirm no accumulation of timing drift over extended periods

### Success Criteria

- Messages arrive at PNF within timing window
- No DL_TTI.request too late errors
- Stable offset values (±10 μs) after convergence
- Processing time statistics reflect actual measurements

## Future Enhancements

### Possible Improvements

1. **Adaptive Safety Factor**: Adjust safety factor based on processing time variance
2. **Per-Message-Type Tracking**: Track processing times separately for different message types
3. **Predictive Adjustment**: Use recent trend to predict future processing times
4. **Auto-tuning**: Automatically adjust EWMA alpha based on processing time stability

## References

- NFAPI Specification (SCF225)
- OpenAirInterface NFAPI Implementation
- Exponentially Weighted Moving Average (EWMA) in Time Series Analysis

## Conclusion

The dynamic timing adjustment algorithm provides a robust solution for maintaining stable VNF-PNF message timing in the face of variable processing times. By continuously tracking processing time statistics and dynamically adjusting the effective timing margin, the algorithm ensures reliable message delivery while adapting to changing system conditions.
