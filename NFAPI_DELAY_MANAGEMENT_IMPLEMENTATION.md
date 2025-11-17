# nFAPI Delay Management Implementation

## Overview

This document describes the implementation of nFAPI delay management per SCF-225 specification Section 2.1.3.4, replacing the non-compliant SLOT.indication mechanism with an autonomous VNF tick.

## Problem Statement

The original OAI implementation violated the nFAPI specification:
- **Spec Violation:** PNF sent `SLOT.indication` messages to VNF
- **Issue:** VNF waited for PNF slot.indication → processed → responded → introduced extra latency
- **Spec Requirement:** "the role of SLOT.indication message is replaced by the Delay Management procedure" (SCF-225 Section 2.1.3.4)

## Solution Architecture

### nFAPI Spec Requirements (SCF-225 Section 2.1.3.4)

1. **VNF Autonomous Timing:** VNF maintains its own slot counter ("tick"), not driven by PNF messages
2. **No SLOT.indication:** PNF should NOT send SLOT.indication messages to VNF
3. **Delay Management:** PNF provides timing feedback via:
   - DL Node Sync messages (latency probing)
   - UL Node Sync responses (round-trip time calculation)
   - Timing Info messages (message arrival statistics)

### Implementation Changes

#### VNF Side (nfapi/oai_integration/nfapi_vnf.c)

**1. Autonomous Tick Thread**
```c
void *vnf_nr_autonomous_tick_thread(void *ptr)
```
- **Purpose:** Independent VNF slot counter per nFAPI spec
- **Timing Source:** System clock (`CLOCK_MONOTONIC`)
- **Slot Duration:** Calculated from numerology: `1000000 >> mu` microseconds
- **Precision:** Uses `clock_nanosleep(TIMER_ABSTIME)` for absolute time scheduling
- **Slot Advance:** Increments VNF slot counter, wraps at slots_per_frame
- **Scheduler Trigger:** Calls `trigger_scheduler()` directly with VNF's own timing

**2. VNF Timing State (added to vnf_p7_info)**
```c
uint16_t vnf_sfn;           // VNF autonomous SFN counter
uint16_t vnf_slot;          // VNF autonomous slot counter  
uint8_t vnf_mu;             // Numerology (0=15kHz, 1=30kHz, etc.)
pthread_mutex_t vnf_slot_mutex;  // Protects VNF timing state
uint8_t vnf_terminate;      // Thread termination flag
uint8_t tick_thread_started;    // Thread start tracking
```

**3. Removed slot.indication Callback**
```c
// OLD: p7_vnf->config->nr_slot_indication = &phy_nr_slot_indication;
// NEW: p7_vnf->config->nr_slot_indication = NULL;  // Per spec 2.1.3.4
```

**4. Deprecated Functions**
- `phy_nr_slot_indication()` - wrapped in `#if 0` (no longer called)

#### PNF Side (nfapi/oai_integration/nfapi_pnf.c)

**1. Removed slot.indication Transmission**
```c
// REMOVED (lines 2304-2311):
// int slot_ahead = 2 << mu;
// uint16_t sfn_tx = sfn;
// uint16_t slot_tx = slot;
// sfnslot_add_slot(mu, &sfn_tx, &slot_tx, slot_ahead);
// nfapi_nr_slot_indication_scf_t ind = {.sfn = sfn_tx, .slot = slot_tx};
// oai_nfapi_nr_slot_indication(&ind);
```

**2. Deprecated Functions**
- `oai_nfapi_nr_slot_indication()` - wrapped in `#if 0` (no longer called)

**3. Preserved P7 Processing**
- `nfapi_pnf_p7_slot_ind()` still called - processes P7 slot buffers (DL_TTI, TX_Data, etc.)
- PNF continues normal PHY operations, just doesn't drive VNF timing

## Timing Diagram

### Before (Non-Compliant)
```
PNF PHY Slot N → PNF sends SLOT.indication(N+4) → VNF receives → VNF runs scheduler → 
VNF sends DL_TTI/TX_Data → PNF receives → PNF processes slot N
│←────────────── Round-trip delay ──────────────→│
```

### After (Spec-Compliant)
```
VNF Autonomous Tick: Slot M → VNF runs scheduler → VNF sends DL_TTI/TX_Data
PNF PHY Slot N ← receives DL_TTI/TX_Data for slot N+k
│
│ (Optional: PNF sends Timing Info feedback about message arrival)
```

## Existing Infrastructure (Already in Codebase)

The following delay management components already exist and can be enabled:

### DL Node Sync (nfapi/open-nFAPI/vnf/src/vnf_p7.c)
```c
int vnf_nr_build_send_dl_node_sync(vnf_p7_t* vnf_p7, nfapi_vnf_p7_connection_info_t* p7_info)
```
- VNF can send DL Node Sync to measure round-trip latency
- Enabled via `periodic_timing_enabled` configuration

### UL Node Sync (nfapi/open-nFAPI/pnf/src/pnf_p7.c)
```c
void pnf_nr_handle_dl_node_sync(void *pRecvMsg, int recvMsgLen, pnf_p7_t* pnf_p7, uint32_t rx_hr_time)
```
- PNF responds to DL Node Sync with t1, t2, t3 timestamps
- Already implemented and functional

### Timing Info (nfapi/open-nFAPI/pnf/src/pnf_p7.c)
```c
void pnf_nr_pack_and_send_timing_info(pnf_p7_t* pnf_p7)
```
- PNF can report jitter and delay statistics
- Structure includes: `dl_tti_jitter`, `tx_data_jitter`, `ul_tti_jitter`, `ul_dci_jitter`
- Enabled via `timing_info_mode` and `timing_info_period` configuration

### Configuration TLVs (already defined)
- `0x0106-0x0109`: DL_TTI/TX_Data/UL_TTI/UL_DCI Timing Offsets
- `0x011E`: Timing Window
- `0x011F`: Timing Info Mode (periodic/aperiodic)
- `0x0120`: Timing Info Period

## Configuration

### VNF Configuration (vnf_p7_info)
```c
timing_window = 30;  // microseconds (example)
periodic_timing_enabled = 1;  // Enable DL Node Sync probing
aperiodic_timing_enabled = 0;  // Event-driven Timing Info
periodic_timing_period = 10;  // slots
```

### VNF Autonomous Tick
- **Auto-configured:** Reads numerology from gNB configuration
- **Numerology Source:** `RC.nrmac[0]->common_channels->ServingCellConfigCommon->ssbSubcarrierSpacing`
- **Slot Timing:** `1000000 >> mu` microseconds per slot
- **Slots per Frame:** `10 * (1 << mu)`

## Testing Recommendations

### 1. Functional Test
```bash
# Start gNB with nFAPI VNF
cd cmake_targets
./ran_build/build/nr-softmodem -O <config> --nfapi VNF

# Verify in logs:
# "[VNF] Starting autonomous tick thread (per nFAPI spec 2.1.3.4)"
# "[VNF] Autonomous tick: mu=X, slot_duration=Y us, slots_per_frame=Z"
```

### 2. Timing Validation
- Monitor VNF tick thread logs for slot progression
- Verify scheduler called at correct intervals
- Check no SLOT.indication messages in P7 traffic (use wireshark)

### 3. Latency Measurement
- Compare VNF→PNF message timing vs. old implementation
- Expect reduced latency without round-trip slot.indication delay

### 4. Integration Test
- Run with actual PNF/PHY
- Verify DL_TTI, TX_Data, UL_TTI, UL_DCI messages arrive on time
- Enable Timing Info to monitor message arrival statistics

## Spec Compliance Checklist

✅ **Section 2.1.3.4 - VNF Autonomous Timing**
- VNF maintains independent slot counter
- Not driven by PNF slot.indication

✅ **Section 2.1.3.5 - API Message Order**
- "role of SLOT.indication message is replaced by Delay Management procedure"
- SLOT.indication removed

✅ **Section 2.1.3.4 - Infrastructure Present**
- DL Node Sync implemented (can be enabled)
- UL Node Sync implemented (responds to DL Node Sync)
- Timing Info implemented (can be enabled)

⏳ **Optional Enhancements** (not required for basic compliance)
- Timing window validation
- Slot loss marking
- Jitter calculation per RFC 3550

## Performance Impact

### Expected Benefits
1. **Reduced Latency:** No round-trip slot.indication delay
2. **Predictable Timing:** VNF tick based on system clock, not network messages
3. **Spec Compliance:** Follows nFAPI specification properly

### Potential Concerns
1. **Clock Drift:** VNF and PNF clocks may drift over time
   - **Mitigation:** Enable DL Node Sync probing to synchronize
2. **Message Timing:** VNF may send messages too early/late for PNF
   - **Mitigation:** Enable Timing Info feedback to adjust timing offsets

## Future Enhancements

### Phase 2: Timing Window Validation
- Track actual message arrival times at PNF
- Compare against configured timing windows
- Report early/late arrivals via Timing Info

### Phase 3: Adaptive Timing
- VNF adjusts timing offsets based on Timing Info feedback
- Dynamically widen/narrow timing windows based on jitter
- Automatic slot synchronization via DL Node Sync

### Phase 4: Multi-Numerology Support
- Support different numerologies for different carriers
- Per-carrier timing windows
- Slot anchoring per spec requirements

## References

1. **nFAPI Specification SCF-225** - Section 2.1.3.4 "Delay Management between VNF and PHY"
2. **nFAPI Specification SCF-225** - Section 2.1.3.5 "API message order"
3. **nFAPI Specification SCF-225** - Section 4.1 "DL Node Sync / UL Node Sync / Timing Info"
4. **RFC 3550** - RTP jitter calculation (referenced by nFAPI spec)

## Summary

This implementation achieves **minimum viable spec compliance** for nFAPI delay management:
- ✅ VNF autonomous tick implemented
- ✅ SLOT.indication removed
- ✅ Infrastructure exists for full delay management (can be enabled)

The core requirement from SCF-225 Section 2.1.3.4 is satisfied:
> "the role of SLOT.indication message is replaced by the Delay Management procedure"

Further enhancements (timing windows, jitter calculation, adaptive timing) can be added incrementally based on deployment needs.
