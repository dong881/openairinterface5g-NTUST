# Testing Guide for nFAPI P7 Timing Fixes

## Overview
This document describes how to test the fixes for TX_DATA arriving too late and VNF dynamic timing adjustment issues.

## Problem Summary
- **Issue**: PNF reported TX_DATA messages arriving ~20 seconds late
- **Root Cause**: Stale time reference on PNF side when VNF adjusts slot counter
- **Fix**: Multiple-layer approach with stale reference detection and bounded slot adjustments

## Test Scenarios

### 1. Normal Operation Test
**Objective**: Verify system works correctly under normal conditions

**Steps**:
1. Start VNF and PNF with default timing parameters
2. Observe initial synchronization via timing info messages
3. Monitor for at least 10 minutes of operation

**Expected Results**:
- Initial timing info shows PNF and VNF synchronizing (< 5 second log output)
- No "TOO LATE" warnings after initial sync period
- Message deltas stay within configured timing window (< 1000µs)
- No stale time reference warnings

**Log Patterns to Watch**:
```
[INFO] VNF-SYNC: First timing info - PNF at X.Y, VNF tick was A.B (offset: Z slots)
[INFO] VNF-SYNC: Applying initial sync adjustment of W slots
[P7:0] Initializing time reference at SFN.Slot=X.Y
```

### 2. Stale Reference Detection Test
**Objective**: Verify that stale time references are detected and corrected

**Steps**:
1. Start VNF and PNF
2. Introduce artificial delay or desynchronization (if possible in test environment)
3. Observe time reference refresh behavior

**Expected Results**:
- Periodic time reference refresh every 512 frames (5.12 seconds)
- Staleness detected if no messages received for > 10 seconds
- Automatic refresh when delta > 10 seconds detected

**Log Patterns**:
```
[P7:0] Periodic time reference refresh at SFN.Slot=512.0
[PNF-TIMING] CRITICAL: Message TX_DATA for X.Y has MASSIVE delta=Z µs (> 10s) - FORCING time reference refresh
[DELAY-MGMT] STALE time reference detected: relative_us=X µs (> 20s) - reference needs refresh
[PNF-TIMING] After time reference refresh: delta=Y µs result=ON_TIME
```

### 3. Large Desynchronization Test
**Objective**: Verify bounded slot adjustments prevent massive jumps

**Steps**:
1. Configure VNF and PNF to start with large slot offset (e.g., 500+ slots difference)
2. Observe convergence behavior

**Expected Results**:
- Initial adjustment clamped to ±100 slots maximum
- Subsequent adjustments clamped to ±50 slots
- System gradually converges over multiple timing info exchanges
- No 20-second deltas even with large initial desync

**Log Patterns**:
```
[WARN] VNF-SYNC: Large desync detected: raw_adj=X slots, clamping to ±100 slots (will converge gradually)
[WARN] VNF-SYNC: Large offset (X slots, target Y) - clamping adjustment from A to B slots
```

### 4. Jitter and Latency Stress Test
**Objective**: Verify system handles network jitter and latency

**Steps**:
1. Introduce network delay using tc (traffic control) on Linux
   ```bash
   sudo tc qdisc add dev eth0 root netem delay 100ms 50ms
   ```
2. Monitor timing info and delay statistics
3. Verify adaptive timing adjustment works

**Expected Results**:
- VNF adaptive timing increases offset when high delays detected
- Timing info reports accurate jitter measurements
- System maintains synchronization despite jitter

**Log Patterns**:
```
[VNF-TIMING] High latency: Jitter(DL=X UL=Y ULDCI=Z TxData=W µs) Delays(...)
[ADAPT] VNF: CRITICAL JITTER TX_Data jitter=Xµs delay=Yµs → IMMEDIATE: target_slot_offset A→B slots + timing offset C→D µs
[ADAPT] VNF: Persistent TX_Data high delay/jitter (3 occurrences, delay=Xµs jitter=Yµs) → Increased timing offset: A → B µs
```

## Configuration Parameters for Testing

### Timing Window Configuration (PNF side)
Configured via `oai_pnf_p7_configure_delay_state()`:
- `dl_tti_timing_offset`: 800µs (default)
- `ul_tti_timing_offset`: 800µs (default)
- `ul_dci_timing_offset`: 800µs (default)
- `tx_data_timing_offset`: 800µs (default)
- `timing_window`: 200µs (default)

### VNF Timing Configuration
Configured in `g_vnf_delay_ctx` structure:
- `dl_tti_timing_offset_us`: 800µs (default, increased from 500µs)
- `timing_window_us`: 200µs (default, increased from 150µs)
- `target_slot_offset`: Calculated from timing_offset_us and numerology

### Test Configurations

**Low Latency Network** (local testbed):
```c
timing_offset_us = 300;   // 300µs before slot start
timing_window_us = 100;   // 100µs window
```

**Medium Latency Network** (fronthaul over IP):
```c
timing_offset_us = 500;   // 500µs before slot start
timing_window_us = 150;   // 150µs window
```

**High Latency Network** (split DU-RU over WAN):
```c
timing_offset_us = 800;   // 800µs before slot start
timing_window_us = 200;   // 200µs window
```

## Debugging Commands

### Check Timing Reference State
Look for these log entries:
```bash
grep "time reference" /path/to/log.txt
grep "STALE" /path/to/log.txt
```

### Monitor Slot Synchronization
```bash
grep "VNF-SYNC" /path/to/log.txt
grep "slot synchronization adjustment" /path/to/log.txt
```

### Track Message Arrival Times
```bash
grep "PNF-TIMING" /path/to/log.txt | grep "TOO LATE"
grep "delta.*µs" /path/to/log.txt
```

### Monitor Adaptive Timing Adjustments
```bash
grep "ADAPT" /path/to/log.txt
grep "timing offset.*→" /path/to/log.txt
```

## Success Criteria

### Pass Conditions
1. ✅ No deltas > 10 seconds after initial synchronization
2. ✅ Periodic time reference refresh occurs every 512 frames
3. ✅ Slot adjustments are bounded to ±100 slots (initial) and ±50 slots (subsequent)
4. ✅ System converges to stable synchronization within 10 seconds
5. ✅ Message arrival stays within configured timing window (< 1ms) after sync
6. ✅ Adaptive timing adjustments trigger correctly for high jitter/delay

### Fail Conditions
1. ❌ Persistent "TOO LATE" messages with delta > 10 seconds
2. ❌ Time reference never refreshes (no periodic refresh logs)
3. ❌ Slot adjustments exceed ±100 slots in single jump
4. ❌ System never converges to stable state
5. ❌ Stale reference warnings persist after multiple refresh attempts

## Known Limitations

1. **Full OAI Build Required**: Integration testing requires complete OAI build environment with asn1c and all dependencies
2. **Hardware Requirements**: RF hardware (USRP) or RF simulator needed for end-to-end testing
3. **Timing Precision**: Microsecond-level timing requires real-time kernel or low-latency system configuration
4. **Network Conditions**: Testing under various network conditions requires traffic control tools or network emulation

## Next Steps for Full Validation

1. **Build OAI**: Install dependencies and build full OAI gNB/UE
   ```bash
   cd cmake_targets
   ./build_oai -I --install-optional-packages
   ./build_oai -w USRP --gNB --nrUE --ninja
   ```

2. **Run RF Simulator Test**:
   ```bash
   cd ci-scripts
   ./run_locally.sh xml_files/nsa_rf_simulator.xml
   ```

3. **Analyze Logs**:
   ```bash
   cd cmake_targets/log/nsa_rf_simulator.d
   grep -E "(TIMING|SYNC|ADAPT|STALE)" *.log
   ```

4. **Verify Timing Info Reports**: Check that timing info messages show reasonable jitter and delay values (< 1000µs for local tests)

5. **Long-Duration Test**: Run for > 1 hour to verify stability and convergence

## Additional Resources

- SCF-225: 5G nFAPI specification (Section 2.6 - P7 Delay Management)
- SCF-222: 5G FAPI PHY API specification (Delay Management)
- OpenAirInterface documentation: `doc/BUILD.md`, `doc/RUNMODEM.md`
- nFAPI P7 Delay Management Developer Manual: `nfapi/NFAPI_P7_Delay_Management_Dev_Manual.md`
