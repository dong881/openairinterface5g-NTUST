# 5G FAPI PHY API Specification - Delay Management Detailed Description

**Document Title:** 5G FAPI: PHY API Specification  
**Issue Date:** August 2023  
**Version:** 222.07.00

---

## 1. Overview of Delay Management

Delay Management is a mechanism based on reception windows maintained at L1 (PHY) for each message type. The specification describes two types of delay management mechanisms.

### 1.1 Anchoring Mechanism of Reception Windows

Reception windows are anchored to specific slots. Each PDU carried in a message anchors its reception window to the numerology of that slot.

#### 1.1.1 Multi-numerology PDU Window Anchoring

**Example:** If a DL_TTI.request carries a 15 kHz PDSCH PDU and a 30 kHz SSB PDU:
- The 15 kHz PDSCH PDU is referenced to the 15 kHz slot's Rx window
- The 30 kHz SSB PDU is referenced to the 30 kHz slot's Rx window
- Both are related to the same DL_TTI.request message

#### 1.1.2 Special Slot Anchoring Rules

| Scenario              | Anchoring Rule                 | Description                                                      |
|-----------------------|--------------------------------|------------------------------------------------------------------|
| PRACH long preamble   | 15 kHz PRACH numerology        | PRACH PDUs for long preambles must be anchored to 15 kHz numerology |
| SSB (SSB supported)   | DL_TTI.request Rx window       | If supported/configured, an SSB PDU should use the highest numerology DL_TTI.request Rx window that is lower numerology than SSB numerology |
| PRACH (PRACH supported)| UL_TTI.request Rx window       | If supported/configured, PRACH PDU uses highest numerology UL_TTI.request Rx window that is lower numerology than PRACH numerology |

---

## 2. Two Delay Management Mechanisms

### 2.1 Delay Management with Timestamps

#### 2.1.1 Basic Concept
For a PHY supporting delay management and the nFAPI message format, supported slot procedures follow the documentation from SCF-225 section 2.1.3.

#### 2.1.2 Applicable Scenarios
This mechanism applies where:
- L2 and L1 can rely on timestamps for synchronization
- PHY supports nFAPI message format
- High-precision time synchronization is required

---

### 2.2 Delay Management without Timestamps

#### 2.2.1 Applicable Scenarios
Delay management can also be supported without relying on timestamps between L2 and L1, ensuring timely handling of P7 slot procedures.

#### 2.2.2 Core Mechanism Description

a. **Expected PHY Reception Window**
  - The PHY expects P7 slot messages from the VNF to arrive within a reception timing window. The window is maintained to buffer and process time-critical P7 messages, applying them at the target slot.
  - **Time-critical P7 messages include:**
    - DL_TTI.request (downlink transmission interval request)
    - UL_TTI.request (uplink transmission interval request)
    - UL_DCI.request (uplink/downlink control information request)
    - TX_DATA.request (transmit data request)

b. **Reception Timing Window Characteristic Parameters**
  - Parameters (per SCF-225 Figure 2-11):
    - Timing Window: total width/span of the reception window
    - <msg> Timing offset: offset relative to the target slot for each time-critical message type

**Parameter Definitions:**
- **Timing Window:** Defines the valid period for P7 message arrival at PHY, usually in microseconds (μs) or slot units
- **<msg> Timing Offset:** Offset relative to target slot start, may be negative (before slot start) or positive (after)

c. **P7 Message Reception Handling**
  - For slot S, handling is as follows:
    - If the message arrives within the timing window: normal processing at slot S
    - If the message arrives too early or too late: marked and reported to L2 via timing indication

| Arrival State | Handling     | Result                                  |
|--------------|-------------|-----------------------------------------|
| In window    | Processed    | Message applied for slot S as normal     |
| Too early    | Mark early   | PHY marks and builds timing report      |
| Too late     | Mark late    | PHY marks and builds timing report      |

**Difference from Timestamped Mechanism:**

| Feature              | Without Timestamp         | With Timestamp         |
|----------------------|--------------------------|------------------------|
| Timestamp in Header  | Not required             | Required               |
| Timing Report Mode   | Uses TIMING.indication   | May use other sync     |
| Timestamp Requirement| Timing indication msg not timestamp dependent | Depends on timestamps |
| DL/UL node sync      | Not required             | May be required        |

**TIMING.indication Message:**
- Sent L1 to L2 to report timing status of message arrivals
- Trigger mode: event-driven (too early/too late), periodic, or hybrid
- Initial timing indication always triggered when PHY transitions to RUNNING state

**Slot-level Time Awareness:**
- No DL/UL node sync required for non-timestamped delay management
- Slot-level time awareness via TIMING.indication
- L2 should adjust transmit window according to indication

---

## 3. Non-timestamped Delay Management Sequence Example

### 3.1 Timeline Event Sequence

| Time   | L1 Status        | L2 Status           | Event Description                       |
|--------|------------------|---------------------|------------------------------------------|
| Init   | Monitors slots 0/0-0/3 | SFN/slot 0/0-0/3 | L2 initially ignores L1/L2 delay assumption |
| T1     | Monitors slots 8/11-8/13 | SFN/slot 0/1-0/2 | L2 converging to L1                     |
| T2     | Monitors slots 8/12-8/13 | SFN/slots 8/11-8/12 | In progress                          |
| T3     | Monitors slots 9/0-9/3   | SFN/slots 8/12-8/13 | In progress                          |
| T4     | Received                 | SFN/slots 9/0-9/2   | DL_TTI for SFN 9/0 arrives too late, triggers indication |
| T5     | Monitors slots 9/1-9/2   | SFN/slots 0/1-0/2   | DL_TTI for SFN 0/1 arrives too late, triggers indication |
| Later  | -                        | Adjusted            | L2 adjusts transmit window per indication |

### 3.2 Key Event Descriptions
- L2 initially assumes delay negligible
- L2 converges to L1 through timing indication feedback
- Multiple "too late" events trigger indication and window adjustment
- L2 gradually synchronizes to L1 slot timing

---

## 4. Slot Procedures

### 4.1 Purpose
- **Control:** Control DL and UL frame structures
- **Data Transfer:** Transfer slot data between L2/L3 and PHY

### 4.2 Supported Procedures by PHY API

- **Slot Message Transmission (SLOT.indication):** Intervals include 62.5μs, 125μs, 250μs, 500μs, and 1ms
- **SFN/Slot Synchronization:** Sync between L2/L3 and PHY

**Downlink Procedures:**
- BCH transmission
- PCH transmission
- DL-SCH transmission
- DCI (downlink control info) transmission
- CSI-RS (channel state info reference signal) transmission

**Uplink Procedures:**
- RACH reception
- UL-SCH reception
- UCI (uplink control info) reception
- SRS (sounding reference signal) reception

---

## 5. SLOT Signal

### 5.1 Scope
- Applies only to PHYs supporting SFN/slot sync per SCF-225

### 5.2 SLOT.indication Message
- Sent by PHY to L2/L3 at slot start
- Indicates slot SFN and slot info
- Reference for L2/L3 slot timing, may include timestamp

| Parameter    | Type     | Description                               |
|--------------|----------|-------------------------------------------|
| SFN          | Integer  | System frame number (0-1023)              |
| Slot         | Integer  | Slot number, numerology-dependent         |
| Timestamp    | Optional | Precise slot start time (if supported)    |

---

## 6. Parameter Summary Tables

### 6.1 Delay Management Core Parameters

| Parameter           | Code    | Type          | Range/Unit      | Required | Description                                       |
|---------------------|---------|---------------|-----------------|----------|---------------------------------------------------|
| Timing Window       | TW      | Numeric       | μs or slot unit | Yes      | Window width for valid message arrival            |
| DL_TTI Timing Offset| DL_TO   | Signed Int    | Rel. to target  | Yes      | Offset for DL_TTI.request                        |
| UL_TTI Timing Offset| UL_TO   | Signed Int    | Rel. to target  | Yes      | Offset for UL_TTI.request                        |
| UL_DCI Timing Offset| DCI_TO  | Signed Int    | Rel. to target  | Yes      | Offset for UL_DCI.request                        |
| TX_DATA Timing Offset| TX_TO  | Signed Int    | Rel. to target  | Yes      | Offset for TX_DATA.request                       |

### 6.2 Timing Parameter Definitions

| Parameter              | Definition                  | Typical Range        | Configuration             |
|------------------------|----------------------------|----------------------|---------------------------|
| <msg> Timing Offset    | Time before/after target   | Configurable (often negative for early arrival) | By configuration command |
| Receive Time Window Size| Valid window width        | Few hundred μs to ms | Per PHY/system needs      |

### 6.3 TIMING.indication Message Parameters

| Parameter        | Code | Type    | Description                                  |
|------------------|------|---------|----------------------------------------------|
| SFN              | SFN  | Integer | System frame number                         |
| Slot             | SLOT | Integer | Slot number                                 |
| Timing Report Status | TRS | Enum  | "TOO_EARLY" or "TOO_LATE"                  |
| Num Timing Reports   | NTR | Integer| Number of timing reports                    |

### 6.4 Slot Timing Parameters

| Parameter      | Code    | Type     | Description                                 |
|---------------|---------|----------|---------------------------------------------|
| Slot Duration | SD      | Enum     | 62.5μs, 125μs, 250μs, 500μs, 1ms            |
| Numerology    | NUM     | Integer  | 15 kHz, 30 kHz, 60 kHz, 120 kHz, etc        |

---

## 7. Workflow & Timing Diagrams

### 7.1 Non-timestamped Delay Management Workflow

```
Start
  ↓
PHY enters RUNNING state
  ↓
PHY sends initial TIMING.indication
  ↓
L2 receives indication, estimates delay
  ↓
L2 sends P7 message (DL_TTI/UL_TTI/etc)
  ↓
P7 message arrives at PHY
  ↓
PHY checks window
  ↓
├─ In window → process for target slot
│
└─ Out of window → mark TOO_EARLY/TOO_LATE
    ↓
    PHY sends TIMING.indication (as event or periodic)
    ↓
    L2 receives feedback, adjusts transmit window
    ↓
    L2 resends/adapts
    ↓
    Iterate until sync
```

### 7.2 Basic Slot Procedure Sequence

```
Start (PHY boot)
  ↓
Init SFN=0, Slot=0
  ↓
PHY enters RUNNING
  ↓
Send SLOT.indication(SFN=0, Slot=0)
  ↓
L2/L3 receives and starts slot operations
  ↓
For each slot interval:
  - Prepare DL message → DL_TTI.request
  - Send to PHY (within window)
  - PHY executes DL for target slot
  - Prepare UL → UL_TTI.request
  - Send to PHY (within window)
  - PHY receives UL
  - UL_TTI processed
  - Slot++
  - If Slot==Slots per frame
    - Slot=0; SFN++
Repeat until stop
```

### 7.3 Message Arrival Timing Relation

```
Timeline →
Target slot start ↓
   |------|------------|
   ↑      ↑            ↑
Current   Timing      Window
Time     Window End

<msg> Timing Offset (often negative - early)

[Window interval]
   ↓
   ├─ Message arrives too early
   │
   ├─ Message arrives in window ✓ ← Valid
   │
   └─ Message arrives too late
```

---

## 8. Configuration Recommendations

### 8.1 Typical Timing Window Sizes

| Scenario           | Recommended Value  | Description                              |
|--------------------|-------------------|------------------------------------------|
| Low latency        | Small (100-200μs) | Tight transmit window for L2/L3          |
| Standard           | Medium (300-500μs)| Balanced for latency/flexibility         |
| High tolerance     | Large (1-2ms)     | For larger delay tolerance               |

### 8.2 Timing Offset Recommendations
- **DL_TTI Offset:** Often negative for early arrival before slot
- **UL_TTI Offset:** Usually earlier than DL_TTI for more prep time
- **TX_DATA Offset:** Earliest arrival to ensure PHY data processing

### 8.3 TIMING.indication Message Configurations
- **Event-driven:** For latency-sensitive systems
- **Periodic:** For systems needing regular monitoring
- **Hybrid:** Recommended for deployment (event-driven for anomalies, periodic for regular monitoring)
