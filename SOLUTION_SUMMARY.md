# Solution Summary: TX Data Latency and HARQ PDU Issues

## Problem Statement

### Issue 1: Persistent High TX_Data Latency
- VNF logs showed continuous high TX_Data delays (~217-236µs)
- VNF detected the issue but made NO adjustments
- Only logged warnings without taking corrective action
- Example logs:
  ```
  109201.517738 [W] vnf_delay_handle_timing_info: [WARN] VNF: High TxData delay=229µs - fronthaul latency issue
  ```

### Issue 2: Missing HARQ PDU and PDSCH Generation Failures
- PHY continuously reported missing HARQ PDUs:
  ```
  [PHY] 475.16 PDSCH generation skipped: missing HARQ PDU for dlsch_id 0
  [PHY] 475.16 No valid PDSCHs to generate (all missing PDUs)
  ```
- Root cause: TX_Data messages arriving outside timing window at PNF
- Result: Slot loss and failed downlink transmissions

## Root Causes Identified

1. **Hardcoded Timing Offsets**: PNF_CONFIG used fixed 20000µs (20ms) offsets for all message types
2. **No Adaptive Adjustment**: VNF had no logic to adjust timing parameters based on feedback
3. **No Immediate Relief**: Even if offsets were adjusted, they wouldn't take effect until restart
4. **Missing SCF-222 Compliance**: Delay management spec (Section 3.4.7) requires VNF to respond to timing info feedback

## Solution Implemented

### Architecture Changes

1. **Enhanced VNF Delay Context Structure**
   ```c
   typedef struct {
     // ... existing fields ...
     uint32_t dl_tti_timing_offset_us;   // Per-message timing offsets
     uint32_t ul_tti_timing_offset_us;
     uint32_t ul_dci_timing_offset_us;
     uint32_t tx_data_timing_offset_us;
     uint32_t consecutive_high_delay_count[4]; // Track persistent delays
     uint32_t last_adjustment_time_ms;         // Rate limiting
   } oai_vnf_delay_ctx_t;
   ```

2. **Adaptive Timing Adjustment Function**
   - Monitors per-message latency and jitter
   - Tracks consecutive high delays (>50µs)
   - Triggers adjustments based on severity:
     - **Critical (>150µs)**: Immediate adjustment (200µs step)
     - **Persistent**: Gradual adjustment after 3 occurrences (100µs step)
   - Rate limited to one adjustment per 5 seconds
   - Maximum offset: 30ms (per SCF-222 spec)

3. **Two-Level Adjustment Strategy**

   **Level 1: Immediate Effect (Slot Offset)**
   - When critical delays detected, increases `target_slot_offset`
   - Makes VNF run 1 additional slot ahead of PNF
   - Takes effect immediately via `sync_pending` flag
   - Provides instant relief without waiting for restart

   **Level 2: Long-Term Fix (Timing Offset)**
   - Increases timing offset values in context
   - Applied on next PNF_CONFIG (restart/reconnect)
   - Widens receive window on PNF side
   - Provides permanent accommodation for latency

### Code Changes

1. **nfapi/oai_integration/nfapi_vnf.c**
   - Added `vnf_adaptive_timing_adjust()` function
   - Modified `vnf_delay_handle_timing_info()` to call adjustment logic
   - Updated `nr_param_resp_cb()` to use dynamic offsets from context
   - Changed default offsets from 20000µs to 500µs (more reasonable)
   - Enhanced logging for timing adjustments

### Key Improvements

1. **Initial Configuration**: Changed from 20ms to 500µs default offsets
2. **Dynamic Adjustment**: Automatically responds to timing info feedback
3. **Immediate Relief**: Slot offset adjustment provides instant improvement
4. **Long-term Stability**: Timing offset adjustments persist across restarts
5. **Rate Limiting**: Prevents oscillation with 5-second minimum interval
6. **Comprehensive Logging**: Tracks all adjustments with before/after values

## Expected Behavior After Fix

### Normal Operation (Low Latency)
```
[INFO] Timing offsets: DL_TTI=500µs UL_TTI=500µs UL_DCI=500µs TX_Data=500µs
[INFO] VNF-ANALYZE: Jitter: DL=0µs UL=0µs ULDCI=0µs TxData=10µs | Delays: DL=5µs UL=5µs
```

### When High Delay Detected (Gradual Adjustment)
```
[INFO] VNF-ANALYZE: Jitter: TxData=100µs | Delays: TxData=80µs
[INFO] VNF-ANALYZE: Jitter: TxData=100µs | Delays: TxData=85µs
[INFO] VNF-ANALYZE: Jitter: TxData=100µs | Delays: TxData=75µs
[INFO] [ADAPT] VNF: Persistent TX_Data high delay (3 occurrences) → Increased timing offset: 500µs → 600µs
```

### When Critical Delay Detected (Immediate Adjustment)
```
[WARN] VNF-ANALYZE: Jitter: TxData=200µs | Delays: TxData=230µs
[WARN] [ADAPT] VNF: CRITICAL TX_Data delay=230µs → IMMEDIATE: target_slot_offset 6→7 slots (+ timing offset 500µs→700µs)
```

### After Adjustment (Recovery)
```
[INFO] VNF-ANALYZE: Jitter: TxData=50µs | Delays: TxData=20µs
[PHY] PDSCH generation successful for all PDUs
```

## Validation Points

1. **Timing Offset Initialization**: Verify PNF_CONFIG uses 500µs instead of 20000µs
2. **Adjustment Triggers**: Confirm adjustments occur after 3 consecutive high delays
3. **Immediate Effect**: Check that critical delays trigger slot offset increase
4. **Rate Limiting**: Verify no more than one adjustment per 5 seconds
5. **HARQ PDU Recovery**: Confirm no more missing HARQ PDU errors
6. **Slot Loss Prevention**: Verify DL/UL slot loss rate decreases

## Performance Impact

- **Minimal**: Adjustment logic runs only when timing info received (~5ms intervals)
- **Thread-safe**: Uses existing mutex protection
- **No additional latency**: Adjustments optimize existing timing
- **Logging overhead**: Only logs when adjustments made (rare events)

## Compliance

- **SCF-222 Section 3.4.7**: Implements event-driven delay management
- **SCF-225**: Compatible with nFAPI P7 timing window specifications
- **RFC 3550**: Jitter calculation matches RTP jitter algorithm
- **TLV Ranges**: Respects 0-65535µs range for timing offsets (TLVs 0x0106-0x0109)

## Future Enhancements

1. **Configuration File Persistence**: Store adjusted offsets to config file
2. **Per-PNF Adjustment**: Track and adjust offsets independently per PNF
3. **Statistical Analysis**: Add long-term trend analysis for proactive adjustment
4. **Dynamic Window Size**: Also adjust timing window based on jitter patterns
5. **Dashboard Integration**: Expose timing metrics via REST API or web UI
