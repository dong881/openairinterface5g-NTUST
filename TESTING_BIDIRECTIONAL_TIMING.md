# Testing Plan: Bidirectional Adaptive Timing Adjustment

## Overview

This document provides comprehensive testing procedures for the bidirectional adaptive timing adjustment implementation that fixes TOO EARLY message arrivals in nFAPI P7.

## Test Environment Setup

### Prerequisites
```bash
# 1. Build OAI with changes
cd cmake_targets
source ../oaienv
./build_oai -w USRP --gNB --nrUE --ninja

# 2. Verify build success
ls -la ran_build/build/nr-softmodem
ls -la ran_build/build/nr-uesoftmodem

# 3. Configure logging level
export NFAPI_TRACE_LEVEL=2  # INFO level
export LOG_LEVEL=info
```

### Test Configurations

#### Configuration 1: Local Testbed (Low Latency)
```bash
# gNB configuration
timing_offset_us=500      # Start with default
timing_window_us=200
mu=1                      # 30kHz SCS

# Expected behavior
# - Initial convergence: 3-6 seconds
# - Final delta: -100 to 0 µs
# - target_slot_offset: 1-2 slots
```

#### Configuration 2: Medium Latency Network
```bash
# gNB configuration  
timing_offset_us=500      # Start with default
timing_window_us=250      # Wider window for jitter
mu=1

# Expected behavior
# - Initial convergence: 6-9 seconds
# - Final delta: -150 to +50 µs
# - target_slot_offset: 1-3 slots
```

#### Configuration 3: High Latency / High Jitter
```bash
# gNB configuration
timing_offset_us=500      # Start with default
timing_window_us=300      # Wide window for high jitter
mu=1

# Expected behavior
# - Adaptive will increase offset to 800-1000µs
# - Initial convergence: 9-15 seconds
# - Final delta: -200 to +100 µs
# - target_slot_offset: 2-4 slots
```

## Test Cases

### Test Case 1: Basic Convergence from TOO EARLY

**Objective**: Verify system converges when messages arrive too early

**Initial State**:
- timing_offset_us = 800µs (simulating previous configuration)
- target_slot_offset = 2
- Messages arriving ~6500µs early

**Procedure**:
1. Start gNB with timing_offset=800µs
2. Wait for first timing info message
3. Monitor logs for TOO EARLY warnings
4. Observe adaptive adjustments

**Expected Results**:
```log
[VNF-SYNC] Messages arriving too early (avg_delay=-6500µs, ~-13 slots) → Reduced target_slot_offset to 1
[ADAPT] VNF: CRITICAL EARLY TX_Data delay=-6500µs → IMMEDIATE: target_slot_offset 2→1 slots + timing offset 800µs→500µs
[VNF-SYNC] Messages arriving too early (avg_delay=-3000µs, ~-6 slots) → Reduced target_slot_offset to 1 (already at target)
[ADAPT] VNF: CRITICAL EARLY TX_Data delay=-3000µs → Decreased timing offset: 500µs→300µs (min reached)
[INFO] Messages now arriving within timing window
```

**Success Criteria**:
- ✅ Convergence within 9 seconds
- ✅ Final delta between -200µs and 0µs
- ✅ No persistent TOO EARLY warnings after convergence
- ✅ Stable operation for 10+ minutes

### Test Case 2: Basic Convergence from TOO LATE

**Objective**: Verify system still handles TOO LATE arrivals

**Initial State**:
- timing_offset_us = 300µs (artificially low)
- target_slot_offset = 1
- Messages arriving ~200µs late

**Procedure**:
1. Start gNB with timing_offset=300µs
2. Wait for first timing info message
3. Monitor logs for TOO LATE warnings
4. Observe adaptive adjustments

**Expected Results**:
```log
[ADAPT] VNF: CRITICAL LATE TX_Data delay=200µs → IMMEDIATE: target_slot_offset 1→2 slots + timing offset 300µs→500µs
[INFO] Messages now arriving within timing window
```

**Success Criteria**:
- ✅ Convergence within 6 seconds
- ✅ Final delta between -100µs and 0µs
- ✅ No persistent TOO LATE warnings after convergence
- ✅ Stable operation for 10+ minutes

### Test Case 3: Oscillation Prevention

**Objective**: Verify system doesn't oscillate between TOO EARLY and TOO LATE

**Initial State**:
- timing_offset_us = 500µs
- Simulated network with ±300µs jitter

**Procedure**:
1. Start gNB with default configuration
2. Monitor adjustments over 30 minutes
3. Count number of offset changes
4. Verify no rapid back-and-forth adjustments

**Expected Results**:
```log
[ADAPT] VNF: Initial adjustment based on high jitter
[INFO] Timing offset stabilized at 600µs
[INFO] No further adjustments for 25 minutes
```

**Success Criteria**:
- ✅ Less than 5 total adjustments in 30 minutes
- ✅ No adjustments reversing previous direction within 10 seconds
- ✅ Rate limiting prevents rapid changes (3s minimum interval)
- ✅ Hysteresis (consecutive=3) prevents single-spike reactions

### Test Case 4: High Jitter Handling

**Objective**: Verify system adapts to high network jitter

**Initial State**:
- timing_offset_us = 500µs
- Network with 250µs jitter

**Procedure**:
1. Start gNB with default configuration
2. Monitor jitter values in timing info
3. Observe adaptive response to critical jitter

**Expected Results**:
```log
[ADAPT] VNF: CRITICAL JITTER TX_Data jitter=250µs → IMMEDIATE: timing offset 500µs→950µs
[VNF-TIMING] Jitter reduced to acceptable levels
```

**Success Criteria**:
- ✅ System increases offset when jitter > 200µs
- ✅ Increased offset accommodates variance
- ✅ Messages arrive on-time despite jitter
- ✅ System stable with high jitter

### Test Case 5: Long-Duration Stability

**Objective**: Verify no drift or instability over extended operation

**Procedure**:
1. Start gNB with default configuration
2. Let system converge (first 15 seconds)
3. Monitor for 2 hours
4. Collect statistics on message arrivals

**Expected Results**:
- Deltas remain within [-200, +50]µs for > 95% of messages
- No persistent TOO EARLY or TOO LATE warnings
- Less than 2 adjustments per hour after initial convergence
- No memory leaks or resource exhaustion

**Success Criteria**:
- ✅ 2-hour continuous operation
- ✅ On-time arrival rate > 95%
- ✅ Maximum 4 total adjustments after first 15 seconds
- ✅ Memory usage stable (< 1% growth per hour)

### Test Case 6: Fast Restart/Reconnection

**Objective**: Verify rapid re-convergence after PNF restart

**Procedure**:
1. Start gNB and let converge
2. Note final timing_offset_us and target_slot_offset values
3. Restart PNF (simulate disconnect/reconnect)
4. Monitor re-convergence time

**Expected Results**:
```log
[VNF-SYNC] First timing info after restart - PNF at 0.0, VNF at 1023.19
[VNF-SYNC] Large desync detected: clamping adjustment
[INFO] Re-converged within 6 seconds
```

**Success Criteria**:
- ✅ Re-convergence within 10 seconds
- ✅ Final parameters similar to pre-restart values
- ✅ No excessive slot jumps (< 100 slots per adjustment)
- ✅ System stable after reconnection

## Monitoring and Metrics

### Key Log Patterns

#### Successful Convergence
```bash
grep -E "(VNF-SYNC|ADAPT|PNF-DELAY)" /var/log/oai.log | grep -E "(Reduced|Increased|Applying)"
```

Expected patterns:
- Initial large adjustments (first 10 seconds)
- Decreasing magnitude of adjustments
- Final stable state with no adjustments

#### Timing Window Compliance
```bash
grep "PNF-DELAY.*TOO" /var/log/oai.log | wc -l
```

Expected: < 20 total TOO EARLY/LATE messages in first 30 seconds, then 0

#### Adaptive Adjustment Activity
```bash
grep "ADAPT.*VNF" /var/log/oai.log | tail -20
```

Expected: Decreasing frequency, final adjustments > 5 minutes apart

### Performance Metrics

Collect these metrics during testing:

1. **Convergence Time**
   ```bash
   # Time from start to first "ON TIME" period (no TOO EARLY/LATE for 60s)
   # Target: < 15 seconds
   ```

2. **On-Time Percentage**
   ```bash
   # Count messages within timing window vs total messages
   # Target: > 95%
   ```

3. **Adjustment Frequency**
   ```bash
   # Number of offset/slot adjustments per hour after convergence
   # Target: < 2 per hour
   ```

4. **Stability Duration**
   ```bash
   # Longest period with no adjustments
   # Target: > 30 minutes
   ```

## Debugging Failed Tests

### Symptom: Persistent TOO EARLY

**Possible Causes**:
1. target_slot_offset not decreasing
2. Minimum offset (300µs) too high for network
3. Slot synchronization conflicting with adaptive timing

**Debug Steps**:
```bash
# Check current target_slot_offset
grep "target_slot_offset" /var/log/oai.log | tail -5

# Check if MIN_OFFSET_US is reached
grep "min.*offset" /var/log/oai.log

# Check slot synchronization adjustments
grep "VNF-SYNC.*Applying" /var/log/oai.log | tail -20
```

**Solution**:
- If target_slot_offset > 2: Delay-based adjustment not working, check avg_delay calculation
- If offset at MIN (300µs): Consider lowering MIN_OFFSET_US to 200µs
- If slot sync interfering: Adjust sync thresholds (gradual_threshold)

### Symptom: Persistent TOO LATE

**Possible Causes**:
1. Network latency higher than max offset
2. Adaptive adjustment not increasing fast enough
3. PNF time reference stale

**Debug Steps**:
```bash
# Check current offset values
grep "timing offset" /var/log/oai.log | tail -10

# Check for stale reference warnings
grep "STALE.*reference" /var/log/oai.log

# Check max offset reached
grep "MAX_OFFSET" /var/log/oai.log
```

**Solution**:
- If offset at MAX (30000µs): Network latency too high for current architecture
- If stale reference: PNF time reference refresh not working, check pnf_p7.c logic
- If not increasing: Check consecutive_high_delay_count, may need lower trigger threshold

### Symptom: Oscillation (Rapid Back-and-Forth)

**Possible Causes**:
1. Rate limiting not working (< 3s between adjustments)
2. Consecutive trigger too low (triggering on transients)
3. Slot sync and adaptive timing fighting each other

**Debug Steps**:
```bash
# Check adjustment timing
grep "ADAPT.*VNF" /var/log/oai.log | awk '{print $1, $2}' | uniq -c

# Check if consecutive triggers are being reset prematurely
grep "consecutive.*count" /var/log/oai.log

# Check slot sync frequency
grep "VNF-SYNC.*Applying" /var/log/oai.log | awk '{print $1, $2}'
```

**Solution**:
- If < 3s between adjustments: Rate limiting broken, check now_ms calculation
- If triggers resetting: Increase CONSECUTIVE_TRIGGER from 3 to 5
- If slot sync too frequent: Increase sync thresholds (sync_threshold, gradual_threshold)

## Success Criteria Summary

### Must Pass (Critical)
- [ ] Test Case 1: TOO EARLY convergence < 9 seconds
- [ ] Test Case 2: TOO LATE convergence < 6 seconds
- [ ] Test Case 5: 2-hour stability with > 95% on-time rate
- [ ] No oscillation (Test Case 3)

### Should Pass (Important)
- [ ] Test Case 4: High jitter handling
- [ ] Test Case 6: Fast restart/reconnection
- [ ] All performance metrics within targets
- [ ] No memory leaks or resource exhaustion

### Nice to Have (Optional)
- [ ] Convergence faster than target times
- [ ] On-time rate > 99%
- [ ] Zero adjustments after initial convergence
- [ ] Supports extreme network conditions (> 1ms latency)

## Reporting Results

### Test Report Template
```markdown
# Test Report: Bidirectional Adaptive Timing

**Date**: YYYY-MM-DD
**Tester**: Name
**Build**: Commit SHA
**Configuration**: Low/Medium/High Latency

## Summary
- Total test cases: X
- Passed: Y
- Failed: Z
- Issues found: N

## Test Results

### Test Case 1: TOO EARLY Convergence
- Status: PASS/FAIL
- Convergence time: X seconds
- Final delta: X µs
- Notes: ...

[Repeat for each test case]

## Performance Metrics
- Convergence time: X seconds (target < 15s)
- On-time rate: X% (target > 95%)
- Adjustment frequency: X per hour (target < 2)
- Stability duration: X minutes (target > 30)

## Issues Found
1. Issue description
   - Severity: Critical/Major/Minor
   - Steps to reproduce: ...
   - Expected vs actual: ...

## Recommendations
- Parameter tuning suggestions
- Code fixes needed
- Additional testing required

## Conclusion
Overall assessment: PASS/FAIL/PARTIAL
```

## Next Steps After Testing

1. **If All Tests Pass**:
   - Merge PR to main branch
   - Update deployment documentation
   - Monitor production deployment

2. **If Tests Fail**:
   - Analyze logs using debugging procedures
   - Adjust parameters or fix code
   - Re-run failed tests
   - Document lessons learned

3. **Parameter Tuning**:
   - If convergence too slow: Decrease CONSECUTIVE_TRIGGER, decrease MIN_ADJUSTMENT_INTERVAL_MS
   - If oscillation occurs: Increase CONSECUTIVE_TRIGGER, increase MIN_ADJUSTMENT_INTERVAL_MS
   - If persistent early: Decrease MIN_OFFSET_US
   - If persistent late: Increase MAX_OFFSET_US (if network truly that slow)

## Conclusion

This testing plan provides comprehensive validation of the bidirectional adaptive timing adjustment. Following these procedures will ensure the implementation correctly handles both TOO EARLY and TOO LATE scenarios, converges quickly, remains stable, and prevents oscillation.
