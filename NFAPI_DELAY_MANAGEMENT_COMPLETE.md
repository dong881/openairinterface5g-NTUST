# nFAPI P7 Delay Management - Complete Implementation

## Overview

This document describes the complete implementation of nFAPI P7 delay management per SCF-225 specification Section 2.1.3.4 and SCF-222, including DL/UL Node Sync, PNF delay/jitter calculation, and VNF dynamic timing adjustment.

## Problem Statement (Translation)

Based on the previous merge's changes, the following were still not implemented:
1. DL sync and UL sync message sending and receiving
2. PNF's correct calculation of delay and jitter
3. VNF's dynamic adjustment based on feedback

## Solution Architecture

### nFAPI Spec Requirements (SCF-225 Section 2.1.3.4)

The implementation follows **Timestamp-based Mode 1 with Node Sync**:

1. **VNF Autonomous Timing:** VNF maintains its own slot counter (already implemented in previous merge)
2. **No SLOT.indication:** PNF does not send SLOT.indication to VNF (already removed in previous merge)
3. **Delay Management Components:**
   - **DL Node Sync / UL Node Sync:** Round-trip latency measurement with timestamps (t1, t2, t3, t4)
   - **Timing Info:** PNF reports message arrival statistics (jitter, delay, earliest arrival)
   - **Dynamic Adjustment:** VNF adjusts timing offsets based on PNF feedback

## Implementation Summary

### 1. PNF Side: Delay and Jitter Calculation

#### Files Modified
- `nfapi/open-nFAPI/pnf/inc/pnf_p7.h`
- `nfapi/open-nFAPI/pnf/src/pnf_p7.c`

#### Changes

**Added to pnf_p7.h (pnf_p7_t structure):**
```c
// Message arrival tracking for delay management (per SCF-225 Section 2.1.3.4)
uint32_t dl_tti_prev_arrival_time;  // Previous arrival timestamp for jitter calculation
uint32_t tx_data_prev_arrival_time;
uint32_t ul_tti_prev_arrival_time;
uint32_t ul_dci_prev_arrival_time;

uint32_t dl_tti_latest_delay;       // Latest delay in microseconds
uint32_t tx_data_latest_delay;
uint32_t ul_tti_latest_delay;
uint32_t ul_dci_latest_delay;

uint32_t dl_tti_earliest_arrival;   // Earliest arrival offset in microseconds
uint32_t tx_data_earliest_arrival;
uint32_t ul_tti_earliest_arrival;
uint32_t ul_dci_earliest_arrival;
```

**Added to pnf_p7.c:**

1. **RFC 3550 Jitter Calculation Function:**
```c
static void update_jitter_rfc3550(uint32_t *jitter, uint32_t current_arrival, uint32_t prev_arrival,
                                   uint32_t current_tx, uint32_t prev_tx) {
    // J(i) = J(i-1) + (|D(i-1,i)| - J(i-1))/16
    // where D(i-1,i) = (R(i) - R(i-1)) - (S(i) - S(i-1))
    int32_t arrival_delta = (int32_t)(current_arrival - prev_arrival);
    int32_t tx_delta = (int32_t)(current_tx - prev_tx);
    int32_t delta = arrival_delta - tx_delta;
    uint32_t abs_delta = (delta < 0) ? -delta : delta;
    *jitter = *jitter + (abs_delta - *jitter) / 16;
}
```

2. **Message Arrival Tracking Function:**
```c
static void track_message_arrival(pnf_p7_t* pnf_p7, uint32_t rx_hr_time, uint16_t sfn, uint16_t slot,
                                   uint32_t *prev_arrival, uint32_t *jitter,
                                   uint32_t *latest_delay, uint32_t *earliest_arrival,
                                   const char *msg_name) {
    // Calculate expected and actual arrival times
    // Update delay, earliest arrival, and jitter statistics
    // Store arrival timestamp for next calculation
}
```

3. **Updated Message Handlers:**
   - Modified `pnf_handle_dl_tti_request()` to accept `rx_hr_time` and track arrivals
   - Modified `pnf_handle_tx_data_request()` to accept `rx_hr_time` and track arrivals
   - Modified `pnf_handle_ul_tti_request()` to accept `rx_hr_time` and track arrivals
   - Modified `pnf_handle_ul_dci_request()` to accept `rx_hr_time` and track arrivals

4. **Enhanced Timing Info:**
   - Updated `pnf_nr_pack_and_send_timing_info()` to send calculated statistics
   - Reports jitter, latest delay, and earliest arrival for all message types
   - Resets tracking variables after sending

### 2. VNF Side: Dynamic Timing Adjustment

#### Files Modified
- `nfapi/open-nFAPI/vnf/inc/vnf_p7.h`
- `nfapi/open-nFAPI/vnf/src/vnf_p7.c`
- `nfapi/open-nFAPI/vnf/src/vnf_p7_interface.c`

#### Changes

**Added to vnf_p7.h (nfapi_vnf_p7_connection_info_t structure):**
```c
// Dynamic timing adjustment based on PNF feedback (SCF-225 Section 2.1.3.4)
uint32_t dl_tti_timing_offset;      // Microseconds before slot start (TLV 0x0106)
uint32_t tx_data_timing_offset;     // Microseconds before slot start (TLV 0x0109)
uint32_t ul_tti_timing_offset;      // Microseconds before slot start (TLV 0x0107)
uint32_t ul_dci_timing_offset;      // Microseconds before slot start (TLV 0x0108)
uint32_t timing_window;             // Window duration in microseconds (TLV 0x011E)

uint32_t last_jitter_dl_tti;        // Last reported jitter for adaptation
uint32_t last_jitter_tx_data;
uint32_t last_jitter_ul_tti;
uint32_t last_jitter_ul_dci;

uint32_t late_count_dl_tti;         // Count of late arrivals for threshold detection
uint32_t late_count_tx_data;
uint32_t late_count_ul_tti;
uint32_t late_count_ul_dci;
```

**Added to vnf_p7.c:**

1. **Dynamic Adjustment Function:**
```c
static void adjust_timing_parameters(nfapi_vnf_p7_connection_info_t *p7_con, 
                                      uint32_t jitter, uint32_t latest_delay,
                                      uint32_t *timing_offset, uint32_t *late_count,
                                      const char *msg_name) {
    // Increase offset when messages arrive late (≥3 times)
    // Reduce offset when jitter is low and messages arrive on time
    // Adjust timing window based on jitter (window = 3 × jitter)
}
```

**Algorithm Details:**
- **Late arrival threshold:** 3 consecutive late arrivals
- **Offset increase:** delay + jitter + 50µs margin
- **Offset decrease:** 10% reduction when jitter < offset/4
- **Offset bounds:** [100µs, 2000µs]
- **Window calculation:** 3 × jitter, bounds [50µs, 300µs]

2. **Enhanced Timing Info Handler:**
   - Updated `vnf_nr_handle_timing_info()` to call adjustment function
   - Stores jitter values from PNF
   - Adjusts offsets for all message types (DL_TTI, TX_Data, UL_TTI, UL_DCI)
   - Logs timing statistics for monitoring

**Added to vnf_p7_interface.c:**

- Initialize timing parameters in `nfapi_vnf_p7_add_pnf()`:
  - Default timing offsets: 500µs (medium latency network)
  - Default timing window: 150µs
  - Initialize all jitter and late count trackers to 0

### 3. VNF Side: Enable DL/UL Node Sync

#### Files Modified
- `nfapi/oai_integration/nfapi_vnf.c`

#### Changes

**Added to vnf_p7_info structure:**
```c
// DL Node Sync for round-trip latency measurement (SCF-225 Section 2.1.3.4)
uint32_t dl_node_sync_counter;       // Counter for periodic DL Node Sync
uint32_t dl_node_sync_period_slots;  // Period in slots for DL Node Sync
```

**Enhanced Autonomous Tick Thread:**
```c
void *vnf_nr_autonomous_tick_thread(void *ptr) {
    // ... existing code ...
    
    // Initialize DL Node Sync (period: 100 slots)
    p7_vnf->dl_node_sync_counter = 0;
    p7_vnf->dl_node_sync_period_slots = 100;
    
    while (!p7_vnf->vnf_terminate) {
        // ... slot advancement ...
        
        // Send periodic DL Node Sync
        if (p7_vnf->periodic_timing_enabled) {
            p7_vnf->dl_node_sync_counter++;
            if (p7_vnf->dl_node_sync_counter >= p7_vnf->dl_node_sync_period_slots) {
                p7_vnf->dl_node_sync_counter = 0;
                
                // Send to all connected PNFs
                // Calls vnf_nr_sync() → vnf_nr_build_send_dl_node_sync()
            }
        }
    }
}
```

## Complete Message Flow

### 1. DL/UL Node Sync Flow (Latency Measurement)

```
VNF Autonomous Tick (every 100 slots)
    ↓
[VNF] Calculate t1 = current time
    ↓
[VNF] Send DL_NODE_SYNC(t1) → [PNF]
    ↓
[PNF] Receive at time, calculate t2 = receive_time
[PNF] Calculate t3 = transmit_time
    ↓
[PNF] Send UL_NODE_SYNC(t1, t2, t3) → [VNF]
    ↓
[VNF] Receive at time, calculate t4 = receive_time
[VNF] RTT = (t4 - t1) - (t3 - t2)
[VNF] Latency = RTT / 2
[VNF] Update filtered latency with exponential moving average
```

### 2. Message Arrival Tracking & Timing Info Flow

```
[VNF] Send DL_TTI.request at time T
    ↓
[PNF] Receive at rx_hr_time
[PNF] Calculate arrival delay relative to slot boundary
[PNF] Update jitter using RFC 3550 algorithm
[PNF] Store latest delay and earliest arrival
    ↓
[PNF] Every N slots (configured period):
[PNF] Send TIMING_INFO(jitter, delay, earliest_arrival)
    ↓
[VNF] Receive TIMING_INFO
[VNF] For each message type:
      - If late ≥3 times: Increase timing offset
      - If jitter low: Reduce timing offset
      - Adjust timing window = 3 × jitter
```

### 3. Dynamic Adjustment Example

**Initial State:**
- Timing offset: 500µs
- Timing window: 150µs
- Jitter: 50µs

**Scenario 1: Messages arriving late**
- Late arrivals: 3 consecutive times with delay=200µs
- **Action:** Increase offset = 500 + 200 + 50 + 50 = 800µs
- **Result:** Messages now arrive on time

**Scenario 2: Network improves, jitter decreases**
- Jitter drops to 30µs (< 800/4 = 200µs)
- **Action:** Reduce offset by 10% = 800 - 80 = 720µs
- Window adjusted: 3 × 30 = 90µs

## Configuration

### VNF Configuration (nfapi_vnf.c)
```c
// Enable periodic timing (already configured)
vnf.p7_vnfs[0].periodic_timing_enabled = 1;
vnf.p7_vnfs[0].aperiodic_timing_enabled = 0;
vnf.p7_vnfs[0].periodic_timing_period = 10;  // slots

// DL Node Sync period (new)
vnf.p7_vnfs[0].dl_node_sync_period_slots = 100;  // every 100 slots
```

### PNF Configuration (via P5 CONFIG.request)
```c
// TLV 0x011F: Timing Info Mode
req->nfapi_config.timing_info_mode.value = 1;  // Periodic

// TLV 0x0120: Timing Info Period
req->nfapi_config.timing_info_period.value = 10;  // slots
```

### Default Timing Parameters (vnf_p7_interface.c)
```c
// Initialized when PNF is added
node->dl_tti_timing_offset = 500;    // 500µs (TLV 0x0106)
node->tx_data_timing_offset = 500;   // 500µs (TLV 0x0109)
node->ul_tti_timing_offset = 500;    // 500µs (TLV 0x0107)
node->ul_dci_timing_offset = 500;    // 500µs (TLV 0x0108)
node->timing_window = 150;           // 150µs (TLV 0x011E)
```

## Testing Recommendations

### 1. Basic Functionality Test
```bash
# Start gNB with nFAPI VNF
cd cmake_targets
./ran_build/build/nr-softmodem -O <config> --nfapi VNF

# Verify in logs:
# "[VNF] Starting autonomous tick thread (per nFAPI spec 2.1.3.4)"
# "[VNF] Timing Info: PNF SFN.Slot=X.Y Jitter(us): DL_TTI=Z ..."
# "[PNF] DL_TTI arrival: expected=A actual=B delay=C jitter=D"
```

### 2. Delay/Jitter Calculation Test
- Monitor PNF logs for jitter values
- Verify jitter follows RFC 3550 algorithm
- Check delay and earliest arrival tracking

### 3. Node Sync Test
- Monitor VNF logs for DL Node Sync sending (every 100 slots)
- Monitor PNF logs for UL Node Sync responses
- Verify t1, t2, t3, t4 timestamp calculation
- Check round-trip latency calculation

### 4. Dynamic Adjustment Test
- Introduce network latency (e.g., using `tc` command)
- Verify VNF increases timing offsets when late arrivals detected
- Reduce network latency
- Verify VNF reduces timing offsets when jitter is low

### 5. Long-term Stability Test
- Run for extended period (hours)
- Verify timing offsets stabilize
- Check jitter remains within acceptable bounds
- Ensure no message loss

## Monitoring & Debugging

### Enable Debug Logs

**PNF Side (pnf_p7.c):**
```c
// In track_message_arrival(), set to 1:
if (1) {  // Enable for debugging
    NFAPI_TRACE(NFAPI_TRACE_INFO, "[PNF] %s arrival: expected=%u actual=%u delay=%d jitter=%u\n",
                msg_name, expected_time, actual_time, delay, *jitter);
}
```

**VNF Side (vnf_p7.c):**
```c
// In adjust_timing_parameters(), set to 1:
if (1) {  // Enable for debugging
    NFAPI_TRACE(NFAPI_TRACE_INFO, "[VNF] Adjusting timing window: %u -> %u us\n",
                p7_con->timing_window, recommended_window);
}
```

### Key Metrics to Monitor

1. **PNF Metrics:**
   - Jitter per message type (µs)
   - Latest delay per message type (µs)
   - Message arrival rate (on-time vs late)
   - Earliest arrival times

2. **VNF Metrics:**
   - Timing offsets per message type (µs)
   - Timing window size (µs)
   - Late arrival counts
   - Round-trip latency from Node Sync (µs)

3. **Performance Metrics:**
   - Message loss rate
   - Slot loss rate
   - VNF-PNF time delta (slots)

## Spec Compliance Checklist

✅ **SCF-225 Section 2.1.3.4 - VNF Autonomous Timing**
- VNF maintains independent slot counter (previous merge)
- Not driven by PNF slot.indication (previous merge)

✅ **SCF-225 Section 2.1.3.4 - Timestamp-based Mode 1**
- DL Node Sync implemented and enabled
- UL Node Sync responses with t1/t2/t3 timestamps
- t4 calculation and RTT measurement
- Latency filtering and offset adjustment

✅ **SCF-225 Section 2.1.3.4 - Timing Info**
- Jitter calculation per RFC 3550
- Latest delay tracking
- Earliest arrival tracking
- Periodic reporting to VNF

✅ **SCF-225 Section 2.1.3.4 - Dynamic Adjustment**
- VNF adjusts timing offsets based on jitter and late arrivals
- Timing window adapts to jitter
- Threshold-based adjustment (3 late arrivals)
- Gradual offset reduction when network improves

✅ **SCF-222 - Delay Management TLVs**
- 0x0106: DL_TTI Timing Offset
- 0x0107: UL_TTI Timing Offset
- 0x0108: UL_DCI Timing Offset
- 0x0109: TX_Data Timing Offset
- 0x011E: Timing Window
- 0x011F: Timing Info Mode
- 0x0120: Timing Info Period

## Performance Impact

### Expected Benefits
1. **Adaptive Timing:** System automatically adjusts to network conditions
2. **Reduced Message Loss:** Dynamic offsets prevent late arrivals
3. **Optimized Performance:** Offsets reduce when network improves
4. **Latency Awareness:** VNF knows actual network latency via Node Sync

### Resource Usage
- **CPU:** Minimal overhead for timestamp calculations
- **Memory:** ~100 bytes per PNF connection for tracking
- **Network:** +2 messages per Node Sync period (100 slots = ~100ms @ 15kHz)

## Future Enhancements

### Phase 2: Advanced Features
- Multi-numerology support with per-carrier timing
- Slot loss marking and recovery
- Advanced filtering algorithms for jitter estimation
- Predictive offset adjustment based on trends

### Phase 3: Configuration Interface
- Runtime configuration of timing parameters
- Dynamic period adjustment based on network conditions
- Per-message-type offset configuration via TLVs
- Statistics export for external monitoring

## References

1. **nFAPI Specification SCF-225** - Section 2.1.3.4 "Delay Management between VNF and PHY"
2. **nFAPI Specification SCF-222** - "5G FAPI PHY API Delay Management"
3. **RFC 3550** - RTP: A Transport Protocol for Real-Time Applications (Jitter calculation)
4. **Previous merge documentation** - NFAPI_DELAY_MANAGEMENT_IMPLEMENTATION.md

## Summary

This implementation completes the nFAPI P7 delay management per SCF-225 and SCF-222 specifications:

- ✅ **PNF:** Calculates delay and jitter using RFC 3550 algorithm
- ✅ **VNF:** Dynamically adjusts timing offsets based on PNF feedback
- ✅ **Node Sync:** Periodic DL/UL Node Sync for round-trip latency measurement
- ✅ **Timing Info:** PNF reports arrival statistics to VNF
- ✅ **Adaptive:** System automatically adapts to network conditions

The implementation is minimal, non-invasive, and builds upon the existing infrastructure. All changes follow nFAPI specification requirements and maintain compatibility with the existing codebase.
