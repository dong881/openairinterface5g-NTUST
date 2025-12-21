# Timing Diagram: Dynamic Adjustment Algorithm

This document provides visual representations of how the dynamic timing adjustment algorithm works.

## Slot Timing Overview

```
Time ────────────────────────────────────────────────────────────────>

VNF:  │<─ slot_duration_us ─>│<─ slot_duration_us ─>│<─ slot_duration_us ─>│
      │                      │                      │                      │
      │  trigger_scheduler   │  trigger_scheduler   │  trigger_scheduler   │
      │  ┌──────────┐        │  ┌──────────┐        │  ┌──────────┐        │
      │  │ Sched    │        │  │ Sched    │        │  │ Sched    │        │
      │  │ + Pack   │        │  │ + Pack   │        │  │ + Pack   │        │
      │  └──────────┘        │  └──────────┘        │  └──────────┘        │
      │        │             │        │             │        │             │
      │        └─ Send msgs  │        └─ Send msgs  │        └─ Send msgs  │
      │                      │                      │                      │
Slot: N                      N+1                    N+2                    N+3
```

## Problem: Variable Processing Time

Without dynamic adjustment, when processing time varies, messages may arrive too late:

```
Slot Duration = 1000 μs
Base Margin = 500 μs

Case 1: Fast Processing (200 μs)
────────────────────────────────────
│◄────── 1000 μs ──────►│
│                       │
│ Process: 200 μs       │
│ ▓▓▓░░░░░░░░░░░░░░░░░ │
│                       │
│◄──500 μs─►            │
│ margin   │ Send       │
│          │ ─────────► PNF receives (OK ✓)
│                       │

Case 2: Slow Processing (800 μs)  
────────────────────────────────────
│◄────── 1000 μs ──────►│
│                       │
│ Process: 800 μs       │
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░ │
│                       │
│◄──500 μs─►            │
│ margin   │ Send       │
│                │      │ 
│                │ ─────┼───────► PNF receives (TOO LATE ✗)
│                │      │         800 μs > 500 μs margin
│                       │

Result: Message arrives 300 μs late!
```

## Solution: Dynamic Adjustment

With dynamic adjustment, the effective margin adapts to processing time:

```
Processing Time EWMA = 800 μs
Safety Factor = 1.2
Processing Time Buffer = 800 * 1.2 = 960 μs

New Effective Margin = 500 + 960 = 1460 μs

Case 2 Revisited: Slow Processing (800 μs)
────────────────────────────────────────────────────────
│◄─────────── 1000 μs ──────────►│
│                                 │
│ Process: 800 μs                 │
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░░░ │
│                                 │
│◄────── 1460 μs ───────►         │
│ effective margin      │ Send    │
│     (adjusted)        │ ────────┼─────────────►
│                       │         │              PNF receives
│                                 │              (OK ✓)
│                                 │
Adjustment: Send 460 μs earlier than before
(slot gets triggered 460 μs earlier via us_adjustment)
```

## EWMA Tracking

The algorithm uses EWMA to track processing time smoothly:

```
Processing Time Samples (μs): 200, 250, 300, 800, 750, 700, 720, 680

Without EWMA (immediate reaction):
──────────────────────────────────────────────
200   250   300   800 ← Sudden spike!
  └───┴─────┴─────┘
    avg=250        Adjustment jumps to 800
                   → Overreaction

With EWMA (α=0.125):
──────────────────────────────────────────────
Initial: EWMA = 200

Sample 1: 250
EWMA = (250 + 7*200)/8 = 212.5

Sample 2: 300
EWMA = (300 + 7*212.5)/8 = 223.4

Sample 3: 800 ← Spike
EWMA = (800 + 7*223.4)/8 = 295.5  ← Smooth increase

Sample 4: 750
EWMA = (750 + 7*295.5)/8 = 352.1

Sample 5: 700
EWMA = (700 + 7*352.1)/8 = 395.8

Result: Smooth adaptation, no overreaction
```

## Complete Timing Flow with Sync Messages

```
VNF                                         PNF
───                                         ───

Slot N: trigger_scheduler
  │
  ├─ Measure processing time
  │  (start to finish)
  │
  └─ Update statistics
     (max, avg, EWMA)
       │
       │
Slot N+2: Send DL_NODE_SYNC ─────────────────────────────> Receive at t2
       t1 = timestamp                                      │
                                                           │
                              <────────────────────────────┘
                              Send UL_NODE_SYNC at t3
                                                 │
Receive at t4 <──────────────────────────────────┘
  │
  ├─ Calculate offset: ((t2-t1) - (t4-t3))/2
  │
  ├─ Get current EWMA of processing time
  │
  ├─ Calculate effective_margin:
  │    = base_margin + (EWMA * 1.2)
  │
  ├─ Calculate adjustments:
  │    slot_adj = (offset + eff_margin) / slot_us
  │    us_adj = (offset + eff_margin) % slot_us
  │
  └─ Apply adjustments in next slot timing cycle
       │
       │
Slot N+3: Apply us_adjustment
       │    (shifts slot timing phase)
       │
       └─ Apply slot_adjustment
            (shifts slot number)
              │
              └─> Scheduler triggers at corrected time
                  Messages sent early enough to account
                  for processing time
```

## Convergence Example

```
Time (slots) →  0    5    10   15   20   25   30   35   40
────────────────────────────────────────────────────────────
Processing      200  220  240  260  280  300  300  300  300
Time (μs)       │    │    │    │    │    │    │    │    │

EWMA            200  205  212  222  235  252  265  275  283
                ●────●────●────●────●────●────●────●────●
                                                  └─ Converged

Effective       700  705  712  722  735  752  765  775  783
Margin (μs)     │    │    │    │    │    │    │    │    │

Timing          0    -5  -12  -22  -35  -52  -65  -75  -83
Offset (μs)     │    │    │    │    │    │    │    │    │
                ├────┼────┼────┼────┼────┼────┼────┼────●
                                                  └─ Within ±10 μs
                                                     (sync_locked)

Phase:          │    │                   │         │
                │    Initial             │ Adapting│ Stable
                │    samples             │         │
                │                        │         └─ Locked
```

## Parameter Sensitivity

### EWMA Alpha (α) Effect

```
α = 1/4 (fast response)
─────────────────────────
Input:  100 200 300 200 100
EWMA:   100 125 169 190 168
         └── Fast reaction to changes

α = 1/8 (balanced)
─────────────────────────  
Input:  100 200 300 200 100
EWMA:   100 113 136 144 139
         └── Smooth tracking (DEFAULT)

α = 1/16 (slow response)
─────────────────────────
Input:  100 200 300 200 100
EWMA:   100 106 119 124 123
         └── Very smooth, slow adaptation
```

### Safety Factor Effect

```
Base Margin = 1000 μs
EWMA = 400 μs

Safety Factor   Buffer    Effective    Headroom
                          Margin       for spikes
──────────────────────────────────────────────────
1.0x            400       1400         0 μs
1.1x            440       1440         40 μs
1.2x (DEFAULT)  480       1480         80 μs  ← Recommended
1.5x            600       1600         200 μs
2.0x            800       1800         400 μs (too conservative)
```

## Summary

The dynamic timing adjustment algorithm ensures:
1. ✓ Messages sent early enough to complete processing
2. ✓ Smooth adaptation to changing conditions
3. ✓ Stable convergence without oscillation
4. ✓ Safety margin for occasional spikes
5. ✓ Minimal overhead and complexity
