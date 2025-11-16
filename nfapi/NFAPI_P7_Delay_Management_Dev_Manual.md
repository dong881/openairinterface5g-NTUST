# NFAPI P7 Delay Management (Timing Info) – Developer's Practical Handbook

## Document Reference
- **Base Specifications**: SCF225_5G_nFAPI_specification (July 2022, v225.3.0)
- **Related Specs**: SCF222_5G_FAPI_PHY_API_specification (August 2023, v222.07.00)
- **Scope**: nFAPI P7 slot procedures and delay management mechanisms

---

## Table of Contents
1. Core Concepts
2. Delay Management Architecture
3. Timing Window Management
4. Node Sync Mechanism
5. Timing Info Messages
6. TLV Configuration Parameters
7. Implementation Details
8. Message Flow & Timing
9. Error Handling
10. Practical Development Guidance

---

## 1. Core Concepts

### 1.1 nFAPI P7 Interface Architecture
The nFAPI P7 interface connects the Virtual Network Function (VNF) and the PHY instance on a slot-by-slot basis. Unlike traditional FAPI—implemented as shared memory with SLOT.indication for timing—nFAPI P7 relies on network transmission, requiring explicit delay management to ensure time-critical messages arrive in time for correct slot scheduling.

### 1.2 Why Delay Management?
Virtualized RANs create inherent transmission delay (fronthaul latency) between VNF and PNF. Message delivery order can be non-deterministic, and PHY requires configuration messages (DL_TTI, UL_TTI, TxData, UL_DCI) to arrive before their target slot starts.

### 1.3 Delay Management Objectives
- **Establish timing reference** between VNF and PHY
- **Define receive window** for each time-critical message
- **Monitor and report** message arrival statistics
- **Adjust dynamically** based on live feedback

---

## 2. Delay Management Architecture

### 2.1 System Overview
```
[ VNF (L2/L3 Scheduler) ] <---nFAPI P7 Messages---> [ PHY Instance ]
        |                                                 |
    Timing Calculation                                  Timing Window
        |                                                    |
    Send DL_TTI / UL_TTI, etc.     <--->     Receive, check timing
```

### 2.2 Key Message Types
| Message              | Use             | Origin | Target |
|----------------------|-----------------|--------|--------|
| DL_TTI.request       | DL transmission | VNF    | PHY    |
| UL_TTI.request       | UL reception    | VNF    | PHY    |
| TxData.request       | DL data         | VNF    | PHY    |
| UL_DCI.request       | UL DCI config   | VNF    | PHY    |

Each must arrive in its **dedicated timing window** prior to the slot start.

### 2.3 Two Implementation Modes
1. **Timestamp-based Delay Management** – Relies on transmit timestamp field, suitable for nFAPI deployments with eCPRI/UDP/IP.
2. **Event-driven Management** – No timestamp; relies on TIMING.indication events, suitable for FAPI-only or legacy deployments.

---

## 3. Timing Window Management

### 3.1 Definition
Each PHY instance manages one receive timing window per time-critical message. Defined by:
- **Timing Offset**: microseconds before slot start
- **Window Size**: duration in microseconds

#### Timing Rules Example (C pseudocode):
```c
window_start = slot_start_time - timing_offset;
window_end = window_start - timing_window;
if (msg_arrival_time >= window_end && msg_arrival_time <= window_start) {
    process_for_slot(); // on-time
} else if (msg_arrival_time > window_start) {
    record early offset;
} else if (msg_arrival_time < window_end) {
    record late offset, mark slot lost;
}
```

### 3.3 Special Slot Anchoring
Multiple numerologies require window anchoring:
- PRACH (long preamble): anchored to 15kHz PRACH window
- SSB: If SSB numerology > PDSCH, use PDSCH window
- PRACH: If PRACH numerology > PUSCH, use PUSCH window

---

## 4. Node Sync Mechanism

### 4.1 Purpose
Used for round-trip latency probing and slot number adjustment.

### 4.2 DL Node Sync (VNF → PHY)
Contains timestamp t1 and optional timing advance.

### 4.3 UL Node Sync (PHY → VNF)
Echoes t1, with t2 (PHY receive time) and t3 (PHY transmit time).

#### Roundtrip Latency Calculation
RTT = t3 - t2

---

## 5. Timing Info Messages

### 5.1 Purpose
- Report arrival/jitter of time-critical messages per slot
- Indicate window compliance (on-time, late, early)
- Provide feedback for VNF timing adjustments

### 5.2 Message Structure
Include:
- last_sfn, last_slot
- time_since_last_timing_info
- DL/UL TTI, TxData, UL DCI jitter
- DL/UL TTI, TxData, UL DCI latest delay & earliest arrival
- subcarrier_spacing

#### Jitter Calculation (RFC 3550 with adjustments):
```
delta = (arrival_time_n - arrival_time_{n-1}) - (transmit_ts_n - transmit_ts_{n-1})
jitter = jitter + (|delta| - jitter)/16
```
---

## 6. TLV Configuration Parameters

### 6.1 Timing Offset TLVs
| TLV Tag | Name                 | Range   | Units |
|---------|----------------------|---------|-------|
| 0x0106  | DL_TTI Timing offset | 0-65535 | µs    |
| 0x0107  | UL_TTI Timing offset | 0-65535 | µs    |
| 0x0108  | UL_DCI Timing offset | 0-65535 | µs    |
| 0x0109  | TxData Timing offset | 0-65535 | µs    |

### 6.2 Timing Window TLVs
| TLV Tag | Name           | Range   | Units |
|---------|----------------|---------|-------|
| 0x011E  | Timing window  | 0-30000 | µs    |

### 6.3 Info Mode TLVs
| TLV Tag | Name            | Kind      | Values|
|---------|-----------------|-----------|-------|
| 0x011F  | Info mode       | bitmap    | periodic, aperiodic |
| 0x0120  | Info period     | slots     | 1-255 |

---

## 7. Implementation Details

### 7.1 VNF Implementation
- Initialize by exchanging PNF_PARAM and CONFIG requests
- Use initial timing offset and window size based on network latency
- Periodically send Node Sync, monitor Timing Info
- Adjust configuration upon detecting high latest delays or jitter

### 7.2 PHY Implementation
- Maintain per-message window structures
- On message receive, check timing
- Collect statistics and generate Timing Info as per mode (periodic/event)

### 7.3 Sample Configurations
- Low latency: offset = 300µs, window = 100µs
- Medium latency: offset = 500µs, window = 150µs
- High latency: offset = 800µs, window = 200µs
---

## 8. Message Flow & Timing

### 8.1 Initialization
```
[VNF] -> [PHY]:
- PNF_PARAM.request → response
- CONFIG.request ↑ confirmed
- START.request ↑ running
- DL Node Sync / UL Node Sync for timing
- Regular slot-based messages
- Timing Info feedback and adaptation
```

### 8.2 Per-Slot Timeline Example
For slot N, offset = 500µs, window = 200µs:
- DL_TTI must arrive between slot_start-500µs and slot_start-300µs
- Late arrival: slot lost, Timing Info reports delay
- Early arrival: optionally buffered

---

## 9. Error Handling

### 9.1 Late Arrival
- Slot is marked lost
- Timing Info reports positive delay
- VNF may send Node Sync to recalibrate

### 9.2 VNF Recovery
- On multiple lost slots, send Node Sync and update offset as needed
- Fallback to conservative timing parameters

### 9.3 PHY Edge Cases
- On window edge: treat as valid (inclusive)
- Zero window: treat as "immediate" messaging
- Timestamp wrapping: handle via modulo arithmetic

---

## 10. Practical Development Guidance

### 10.1 Configuration Recommendations
- Analyze initial latency using Node Sync
- Set conservative timing offsets, widen window if high jitter
- Enable both periodic and event-driven Timing Info in variable networks
- Monitor and adjust offsets based on real-time feedback

### 10.2 Testing Scenarios
- On-time, early, and late message tests
- Multinumerology slot anchoring
- Stress and long-run stability tests
- Network latency variation tests

### 10.3 Debugging & Metrics
- Log arrival timing, window checks, and slot losses
- Aggregate jitter and loss statistics
- Regularly verify with Node Sync probes

---

## 11. Glossary
| Term                 | Definition |
|----------------------|------------|
| Fronthaul            | VNF-PNF network connection |
| PHY Instance         | Single L1 physical entity |
| Receive Timing Window| Range for valid message arrival |
| Timing Offset        | Advanced time before slot start |
| Timing Window        | Width of valid arrival window |
| Node Sync            | VNF-PHY latency probing |
| Timing Info          | PHY feedback for VNF timing |
| Jitter               | Variation in message arrival times (RFC 3550) |
| Timing Advance (TA)  | VNF slot number adjustment instruction |
| Numerology           | Subcarrier spacing (15/30/60/120/240kHz) |
| SFN                  | System Frame Number (0-1023) |
| Slot                 | Slot within SFN (0-159) |
| DL_TTI               | Downlink Transmission Interval message |
| UL_TTI               | Uplink Transmission Interval message |
| TxData               | Downlink data transfer message |
| UL_DCI               | Uplink DCI configuration message |

---

**Document Date**: November 16, 2025
**Version**: 1.0
**Source Specs**: SCF225 v225.3.0, SCF222 v222.07.00