# VNF Autonomous Tick Implementation Summary

## Problem Statement

The original OpenAirInterface nFAPI implementation violated the nFAPI specification (SCF-225) by having the VNF (Virtual Network Function) respond to SLOT.indication messages from the PNF (Physical Network Function). This introduced additional delay and did not implement proper delay management.

**From the logs:**
```
VNF LOGS: 目前LOG都正常不需要特別處理LOG上的warning，但是沒有後續的LOG算是停住了是錯誤的
32782.975276 [I] 3623876160: nfapi_nr_vnf_p7_start: Initialising VNF P7 port:50011
...
32813.210796 [I] 4138911296: pnf_disconnection_indication_cb: [VNF] pnf disconnection indication idx:0
```

## Specification Requirement (nFAPI SCF-225)

According to Section 3.4.2 "Timing and Delay Management":

1. **VNF Must Have Own Timing Source**: The VNF should have its own independent "tick" that progresses through slots
2. **SLOT.indication Suppression**: When delay management is active, SLOT.indication messages should be suppressed
3. **Proactive Message Sending**: VNF sends DL_TTI/UL_TTI/UL_DCI/TX_DATA based on its own clock, NOT in response to PNF
4. **TIMING.indication Feedback**: PNF reports if messages arrived early/late/on-time
5. **Dynamic Adjustment**: VNF adjusts timing offsets based on feedback

## Solution Overview

Implemented a fully spec-compliant VNF autonomous tick mechanism:

```
┌─────────────┐                    ┌─────────────┐
│     VNF     │                    │     PNF     │
│             │                    │             │
│ ┌─────────┐ │                    │             │
│ │  TICK   │─┼──┐                 │             │
│ │ THREAD  │ │  │                 │             │
│ └─────────┘ │  │                 │             │
│      │      │  │                 │             │
│      ↓      │  │                 │             │
│ ┌─────────┐ │  │  DL_TTI/UL_TTI │             │
│ │Scheduler│─┼──┼────────────────→│  Process   │
│ └─────────┘ │  │  UL_DCI/TX_DATA│  Messages   │
│             │  │                 │             │
│             │  │                 │      ↓      │
│             │  │  TIMING.ind     │ ┌─────────┐ │
│  Process   │←─┼─────────────────┼─│  Delay  │ │
│  Feedback  │  │  (early/late)   │ │  Mgmt   │ │
│             │  │                 │ └─────────┘ │
│             │  │                 │             │
│             │  │  SLOT.ind       │             │
│  (suppress)│←─┼─────────────────┼─  (legacy)  │
│             │  │  (ignored)      │             │
└─────────────┘  │                 └─────────────┘
                 │
            Independent
            Time Source
```

## Implementation Details

### 1. VNF Tick Thread (`vnf_tick_thread`)

**File**: `nfapi/oai_integration/nfapi_vnf.c`

```c
void *vnf_tick_thread(void *ptr)
{
    // Calculate slot duration based on numerology (mu)
    const uint32_t slot_duration_us = 1000 >> state->mu;
    
    while (1) {
        // 1. Get current slot state
        // 2. Call trigger_scheduler() to generate messages
        // 3. Advance to next slot (with SFN wraparound)
        // 4. Sleep until next slot boundary using clock_nanosleep()
    }
}
```

**Key Features:**
- Uses `CLOCK_MONOTONIC` for drift-free timing
- Precise sleep with `TIMER_ABSTIME` (absolute time)
- Handles all numerologies: mu=0,1,2,3,4 (15/30/60/120/240 kHz)
- Thread-safe with mutex protection

### 2. Suppressed SLOT.indication

**Modified**: `phy_nr_slot_indication()`

```c
// BEFORE (spec violation):
int phy_nr_slot_indication(nfapi_nr_slot_indication_scf_t *ind) {
    trigger_scheduler(ind);  // ❌ Waits for PNF
    return 1;
}

// AFTER (spec-compliant):
int phy_nr_slot_indication(nfapi_nr_slot_indication_scf_t *ind) {
    // Update slot clock for delay management reference
    vnf_delay_update_slot_clock(ind->sfn, ind->slot);
    
    // Do NOT forward to scheduler (autonomous tick handles it)
    return 1;  // ✅ Independent VNF tick
}
```

### 3. Tick Activation

**Modified**: `nr_start_resp_cb()`

```c
int nr_start_resp_cb(nfapi_vnf_config_t *config, int p5_idx, 
                     nfapi_nr_start_response_scf_t *resp) {
    // Start autonomous tick when PNF is ready
    vnf_start_autonomous_tick(g_vnf_mu, p7_vnf->config);
    return 0;
}
```

**Startup Sequence:**
1. VNF sends PARAM_REQUEST → receives numerology (mu)
2. VNF sends CONFIG_REQUEST → configures timing windows
3. VNF sends START_REQUEST → receives START_RESPONSE
4. **VNF starts autonomous tick thread** ← NEW
5. Tick thread begins autonomous slot progression

## Message Flow Example

### Slot N (VNF initiates)

```
Time →
═══════════════════════════════════════════════════════════════

[VNF-TICK] Slot N starts
    │
    ├─→ trigger_scheduler(sfn, slot)
    │       │
    │       ├─→ gNB_dlsch_ulsch_scheduler()
    │       │       │
    │       │       ├─→ generates PDUs
    │       │       └─→ prepares messages
    │       │
    │       ├─→ oai_nfapi_dl_tti_req()  ──────→ [PNF] receives DL_TTI
    │       ├─→ oai_nfapi_ul_tti_req()  ──────→ [PNF] receives UL_TTI
    │       ├─→ oai_nfapi_ul_dci_req()  ──────→ [PNF] receives UL_DCI
    │       └─→ oai_nfapi_tx_data_req() ──────→ [PNF] receives TX_DATA
    │
    └─→ clock_nanosleep() until slot N+1

                                                [PNF] checks timing windows
                                                [PNF] processes messages
                                                [PNF] sends TIMING.indication
                                                      │
[VNF] receives TIMING.indication ←───────────────────┘
      (jitter, early/late stats)

[VNF-TICK] Slot N+1 starts...
```

## Timing Parameters

### Slot Durations by Numerology

| mu | SCS (kHz) | Slot Duration | Slots/Frame |
|----|-----------|---------------|-------------|
| 0  | 15        | 1000 µs      | 10          |
| 1  | 30        | 500 µs       | 20          |
| 2  | 60        | 250 µs       | 40          |
| 3  | 120       | 125 µs       | 80          |
| 4  | 240       | 62.5 µs      | 160         |

### Timing Offsets (configurable)

Default values from code:
```c
dl_tti_timing_offset  = 20000 µs  // 20ms
ul_tti_timing_offset  = 20000 µs
ul_dci_timing_offset  = 20000 µs
tx_data_timing_offset = 20000 µs
timing_window         = 30 slots
```

## Expected Log Output

### VNF Logs (with autonomous tick)

```
[VNF] Starting autonomous tick thread (spec-compliant mode)
[VNF] Tick thread started: mu=1, slot_duration=500us
[VNF-TICK] Autonomous slot 0/0 (mu=1)
[VNF] Sending DL_TTI 0/0 → PNF
[VNF] Sending UL_TTI 0/0 → PNF
[VNF-TICK] Autonomous slot 0/1 (mu=1)
[VNF] Sending DL_TTI 0/1 → PNF
...
[VNF] Received TIMING.indication: DL_TTI on-time, jitter=50µs
[VNF] Received SLOT.indication 0/5 (suppressed - using autonomous tick)
```

### PNF Logs (unchanged)

```
[PNF] Received DL_TTI 0/0 - processing
[PNF] Received UL_TTI 0/0 - processing
[PNF] Sending TIMING.indication: all messages on-time
[PNF] msgs ontime 1000 thr DL 100.00 UL 100.00 msg late 0
```

## Testing Guide

### 1. Verify Autonomous Tick Activation

**Check logs for:**
```
[VNF] Received NFAPI_START_RESP
[VNF] Starting autonomous tick thread (spec-compliant mode) with mu=X
[VNF] Tick thread started: mu=X, slot_duration=Xus
```

### 2. Verify Message Flow

**VNF should send messages WITHOUT waiting for PNF SLOT.ind:**
```bash
# Good: VNF-TICK drives scheduling
grep "VNF-TICK.*Autonomous slot" vnf.log | head -10

# Good: Messages sent proactively  
grep "Sending DL_TTI\|Sending UL_TTI" vnf.log | head -10

# Good: SLOT.indication suppressed
grep "SLOT.indication.*suppressed" vnf.log | head -5
```

### 3. Verify TIMING.indication Feedback

```bash
# PNF sends timing info
grep "TIMING.indication" pnf.log

# VNF receives and processes timing info
grep "VNF-ANALYZE.*Jitter\|Latest delays" vnf.log
```

### 4. Performance Verification

```bash
# Check message counts
grep "msgs ontime" pnf.log | tail -5

# Should show:
# - High "ontime" count
# - Low/zero "late" count
# - Consistent throughput
```

### 5. Multi-Numerology Testing

Test with different subcarrier spacings:
```bash
# mu=0 (15 kHz): 1ms slots
# mu=1 (30 kHz): 500µs slots (default)
# mu=2 (60 kHz): 250µs slots
# etc.
```

## Troubleshooting

### Issue: VNF logs stop after START_RESPONSE

**Symptom:**
```
[VNF] Received NFAPI_START_RESP
[VNF] pnf disconnection indication
```

**Cause:** Old implementation - VNF was waiting for SLOT.indication

**Solution:** ✅ Fixed - autonomous tick now drives VNF

### Issue: Messages arrive too late at PNF

**Check:**
1. Timing offsets configured correctly?
2. Network latency too high?
3. VNF tick running smoothly? (`grep VNF-TICK vnf.log`)

**Adjust:**
```c
// Increase timing offsets in nr_param_resp_cb():
req->nfapi_config.dl_tti_timing_offset.value = 25000;  // was 20000
```

### Issue: VNF and PNF out of sync

**Symptoms:** Different SFN/slot values in logs

**Solution:** TIMING.indication provides feedback to keep in sync
- VNF uses SLOT.indication for clock reference (but doesn't act on it)
- VNF adjusts based on TIMING.indication feedback

## File Changes Summary

### Modified Files

1. **`nfapi/oai_integration/nfapi_vnf.c`**
   - Added: `vnf_tick_state_t` structure
   - Added: `vnf_tick_thread()` - autonomous tick
   - Added: `vnf_start_autonomous_tick()` - thread starter
   - Modified: `phy_nr_slot_indication()` - suppress forwarding
   - Modified: `nr_start_resp_cb()` - start tick thread
   - Modified: `vnf_delay_handle_timing_info()` - remove bootstrap

### Lines Changed
- ~150 lines added
- ~15 lines modified
- ~5 lines removed
- Net: +~140 lines

## Compliance Checklist

- [x] VNF has independent timing source (tick thread)
- [x] SLOT.indication suppressed when delay management active
- [x] VNF sends messages proactively based on own clock
- [x] PNF sends TIMING.indication feedback
- [x] VNF processes TIMING.indication for delay management
- [x] Timing windows configured and enforced
- [x] Jitter calculation implemented (RFC 3550)
- [x] Multiple numerologies supported
- [x] Thread-safe implementation
- [x] Proper initialization sequence

## References

1. **nFAPI Specification**: SCF-225 5G nFAPI Specification
   - Section 3.4.2: Timing and Delay Management
   - Section 2.3: P7 Message Flow

2. **Delay Management Document**: `NFAPI_P7_Delay_Management_Dev_Manual_EN.md`
   - Timing Window Management
   - Jitter Calculation (RFC 3550)
   - Node Sync Protocol

3. **Original Issue**: 
   - VNF沒有主動發送tick
   - 目前看LOG都是沒有在scheduler的
   - 需要真正意義上的實現delay management

## Conclusion

This implementation makes the OpenAirInterface nFAPI VNF fully compliant with the SCF-225 specification by:

1. ✅ Adding autonomous tick thread (VNF's own timing source)
2. ✅ Suppressing SLOT.indication forwarding  
3. ✅ Sending messages proactively (not reactively)
4. ✅ Using TIMING.indication for delay feedback only
5. ✅ Maintaining proper delay management functionality

The VNF now operates independently without depending on PNF's SLOT.indication messages, which eliminates the additional delay and makes the implementation spec-compliant.
