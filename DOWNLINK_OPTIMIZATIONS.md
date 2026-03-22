# 5G NR Downlink Optimizations - Complete Technical Reference

This document provides a comprehensive overview of all downlink processing optimizations implemented in this OAI 5G NR codebase.

## Table of Contents

1. [Overview](#overview)
2. [Thread Pool Architecture](#1-thread-pool-architecture)
3. [Timing Reordering & Parallel Execution](#2-timing-reordering--parallel-execution)
4. [Precoding Optimizations](#3-precoding-optimizations)
5. [Modulation & Layer Mapping](#4-modulation--layer-mapping)
6. [AVX2 SIMD Optimizations](#5-avx2-simd-optimizations)
7. [Memory Management](#6-memory-management)
8. [O-RAN Fronthaul Optimizations](#7-o-ran-fronthaul-optimizations)
9. [MAC Scheduler Optimizations](#8-mac-scheduler-optimizations)
10. [Phase Rotation Optimizations](#9-phase-rotation-optimizations)
11. [Performance Results](#performance-results)

---

## Overview

The downlink processing pipeline has been extensively optimized to meet the 5G NR 500μs slot budget. Key optimization strategies include:

- **Parallel execution** via enkiTS thread pool with 8 worker threads
- **Timing reordering** to hide latencies through overlapped execution
- **PMI=0 fast path** to eliminate unnecessary precoding operations
- **AVX2 SIMD** for vectorized modulation and table lookups
- **Memory bandwidth reduction** through fused operations and eliminated copies
- **Ping-pong buffers** for race-free async operations

---

## 1. Thread Pool Architecture

### enkiTS Thread Pool

**Location**: `openair1/PHY/ENKITS_POOL/`

**Architecture**:
- enkiTS task scheduler with lock-free work-stealing
- 8 worker threads pinned to cores 6, 7, 8, 9, 10, 11, 13, 14
- Infinite spin count (`gc_SpinCount = UINT32_MAX`) for real-time performance
- Wake-up latency reduced from 9ms to <100μs

**Configuration** (in `enkits_pool.c`):
```c
static const int ENKITS_CORES[8] = {6, 7, 8, 9, 10, 11, 13, 14};

struct enkiTaskSchedulerConfig config;
config.numTaskThreadsToCreate = 8;
config.numExternalTaskThreads = 4;  // For L1_tx, L1_rx threads
```

**Key Commits**:
- `e2b8351f14` - Infinite spin count for real-time performance
- `abb69f6f78` - enkiTS spinning workers optimization

### Task Types

| Task Type | Function | Parallelism |
|-----------|----------|-------------|
| Memory Clear | `enkits_pool_memclear_tx()` | 8 tasks (4 antennas × 2 halves) |
| Symbol RE Mapping | `re_mapping_symbol_task()` | Up to 14 symbols |
| FH South Out | `fh_tx_symbol_task()` | 56 tasks (4 antennas × 14 symbols) |
| Modulation | `nr_modulate_layer_map_256qam_parallel()` | Parallel chunks |

---

## 2. Timing Reordering & Parallel Execution

### Async Memory Clear with Encoding Overlap

**Key Innovation**: Memory clear runs in parallel with PDSCH encoding, completely hiding the ~31μs memclear latency.

**Processing Flow**:
```
┌───────────────────────────────────────────────────────────┐
│ PARALLEL SECTION (encoding hides memclear)                │
│   1. enkits_pool_memclear_tx_async() - starts async clear │
│   2. nr_pdsch_encoding_phase() - LDPC encoding (~160μs)   │
│   3. enkits_pool_memclear_tx_wait() - completes instantly │
└───────────────────────────────────────────────────────────┘
                           ↓
┌───────────────────────────────────────────────────────────┐
│ SEQUENTIAL SECTION (requires clean txdataF)               │
│   4. PRS generation                                       │
│   5. SSB generation (PSS, SSS, PBCH)                      │
│   6. PDCCH generation                                     │
│   7. PDSCH codeword phase (scrambling, mod, RE mapping)   │
│   8. CSI-RS generation                                    │
│   9. Phase rotation                                       │
└───────────────────────────────────────────────────────────┘
```

**Location**: `openair1/SCHED_NR/phy_procedures_nr_gNB.c` lines 247-290

**Key Code**:
```c
// 1. Start async memory clear (returns immediately)
enkits_pool_memclear_tx_async((void***)gNB->common_vars.txdataF, ...);

// 2. PDSCH Encoding Phase (runs in parallel with async memory clear)
nr_pdsch_encoding_phase(msgTx, frame, slot);

// 3. Wait for async memory clear to complete
// Near-instant since encoding (~160μs) >> memclear (~31μs)
enkits_pool_memclear_tx_wait();
```

**Key Commits**:
- `23fd5728ca` - Async memclear with encoding overlap
- `f98e7e82e5` - Split memory clear into 8 tasks

### Ping-Pong Buffer Pattern

**Location**: `openair1/PHY/ENKITS_POOL/enkits_pool.c`

Avoids race conditions when consecutive slots both start async clears:

```c
static enkiTaskSet* g_memclear_async_task[2] = {NULL, NULL};  // Ping-pong
static volatile int g_memclear_async_in_progress[2] = {0, 0};
static memclear_task_args_t g_async_task_args[2][32];         // Two arg buffers
static int g_async_buffer_idx = 0;                            // Current: 0 or 1
```

---

## 3. Precoding Optimizations

### PMI=0 Fast Path (Identity Matrix Precoding)

**Key Innovation**: When PMI=0 for all PRGs, precoding is an identity matrix operation. Layer data maps directly to antennas without intermediate buffer or matrix multiplication.

**Benefits**:
- Eliminates ~98KB intermediate `txdataF_precoding` buffer
- Skips precoding matrix multiplication entirely
- Reduces latency by ~400-500μs per slot

**Location**: `openair1/PHY/NR_TRANSPORT/nr_dlsch.c`

**PMI Detection**:
```c
static inline bool check_all_pmi_zero(nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15)
{
  nfapi_nr_tx_precoding_and_beamforming_t *pb = &rel15->precodingAndBeamforming;

  // prg_size==0 means no PRG grouping, implicitly all PMI=0
  if (pb->prg_size == 0)
    return true;

  // Check all PRGs have PMI=0
  for (int prg = 0; prg < pb->num_prgs; prg++) {
    if (pb->prgs_list[prg].pm_idx != 0)
      return false;
  }
  return true;
}
```

**MAC Scheduler Forcing PMI=0**:

**Location**: `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_primitives.c`

Forces PMI=0 for all layer configurations (1-4 layers):
```c
// Force PMI=0 for fast path precoding
if (ps->nrOfLayers <= 4) {
  pmi = 0;  // Identity matrix - direct layer-to-antenna mapping
}
```

**Key Commits**:
- `cb37533c16` - PMI=0 fast path for 2-layer
- `3d99994c1d` - Extend PMI=0 fast path to all layer counts (1-4)
- `abb69f6f78` - PMI=0 fast path with enkiTS spinning workers

---

## 4. Modulation & Layer Mapping

### Fused Modulation + Layer Mapping for 256QAM

**Key Innovation**: Single-pass operation that performs modulation and layer mapping together, eliminating the intermediate `mod_symbs` buffer (~98KB).

**Location**: `openair1/PHY/MODULATION/nr_modulation.c`

**Function**: `nr_modulate_layer_map_256qam()`

**Before** (two-pass):
```
scrambled_data → nr_modulation() → mod_symbs (~98KB) → nr_layer_mapping() → tx_layers
```

**After** (single-pass):
```
scrambled_data → nr_modulate_layer_map_256qam() → tx_layers
```

**Benefits**:
- Eliminates ~98KB intermediate buffer allocation
- Single pass through input data
- Reduced memory bandwidth
- Better cache utilization

**Key Commit**: `b42bd05701` - Add fused modulation + layer mapping for 256QAM

### Parallel Modulation (enkiTS)

**Function**: `nr_modulate_layer_map_256qam_parallel()`

Parallelizes the fused modulation across multiple enkiTS workers by splitting the output into chunks.

**Key Commit**: `54351d645e` - AVX2 gather optimization and parallel 256QAM modulation

---

## 5. AVX2 SIMD Optimizations

### AVX2 Gather for 256QAM Table Lookup

**Key Innovation**: Uses AVX2 gather instructions (`_mm256_i32gather_epi32`) to perform 8 table lookups simultaneously instead of scalar lookups.

**Location**: `openair1/PHY/MODULATION/nr_modulation.c` lines 889-1029

**Single Layer (8 lookups at once)**:
```c
#ifdef __AVX2__
for (; i + 8 <= n_symbols; i += 8) {
  // Load 8 bytes and expand to 32-bit indices
  simde__m128i bytes = simde_mm_loadl_epi64((simde__m128i *)(scrambled_data + i));
  simde__m256i indices = simde_mm256_cvtepu8_epi32(bytes);
  // Gather 8 table entries at once
  simde__m256i results = simde_mm256_i32gather_epi32(table, indices, 4);
  // Store 8 results
  simde_mm256_storeu_si256((simde__m256i *)(out0 + i), results);
}
#endif
```

**Multi-Layer Deinterleaving**:

For 2-layer and 4-layer modes, combines gather with AVX2 shuffle/permute operations:
```c
// 2-layer: gather + deinterleave
simde__m256i shuf_even = simde_mm256_permutevar8x32_epi32(res0,
    simde_mm256_set_epi32(6, 4, 2, 0, 6, 4, 2, 0));
simde__m256i shuf_odd = simde_mm256_permutevar8x32_epi32(res0,
    simde_mm256_set_epi32(7, 5, 3, 1, 7, 5, 3, 1));
```

**Performance**: 8x throughput improvement for table lookups vs scalar code.

**Key Commit**: `54351d645e` - AVX2 gather optimization and parallel 256QAM modulation

### AVX512 Scrambling

**Location**: `openair1/PHY/NR_TRANSPORT/nr_dlsch.c`

Uses existing AVX512 implementation for PDSCH scrambling (memory-bandwidth bound operation).

---

## 6. Memory Management

### 8-Task Memory Clear (Full Worker Utilization)

**Key Innovation**: Split each antenna buffer into 2 halves, creating 8 tasks for 8 workers instead of 4 tasks for 4 antennas.

**Location**: `openair1/PHY/ENKITS_POOL/enkits_pool.c`

```c
// Split each antenna buffer into 2 tasks for 8 total tasks
const int tasks_per_antenna = 2;
int total_tasks = num_beams * num_antennas * tasks_per_antenna;  // 1 × 4 × 2 = 8

for (int ant = 0; ant < num_antennas; ant++) {
    int half_samples = samples_per_slot / 2;

    // First half (0 to 28672)
    task_args[task_idx].buffer = txdataF[beam][ant] + offset;
    task_args[task_idx].size = half_samples * sizeof(int32_t);
    task_idx++;

    // Second half (28672 to 57344)
    task_args[task_idx].buffer = txdataF[beam][ant] + offset + half_samples;
    task_args[task_idx].size = (samples_per_slot - half_samples) * sizeof(int32_t);
    task_idx++;
}
```

**Benefits**:
- 100% worker utilization (8 tasks → 8 workers)
- Better cache fit (114.5KB per task vs 229KB)
- Improved load balancing with work-stealing

**Performance**: ~31μs average, effective bandwidth 29.3 GB/s

**Key Commit**: `f98e7e82e5` - Split memory clear into 8 tasks

### Non-Temporal Stores (Attempted, Reverted)

Non-temporal stores were tested for memory clear but **caused 2x slowdown** in RE Mapping due to cache misses. Regular memset is better because the buffer is immediately reused by signal generation.

---

## 7. O-RAN Fronthaul Optimizations

### Direct txdataF Access (Zero-Copy)

**Key Innovation**: O-RAN xRAN library reads directly from PHY `txdataF` buffer, eliminating the intermediate `txdataF_BF` copy.

**Location**: `radio/fhi_72/oran_isolate.c`, `openair1/SCHED_NR/nr_ru_procedures.c`

**Before**:
```
txdataF → memcpy → txdataF_BF → xran packetization
           ~115μs
```

**After**:
```
txdataF → xran packetization (direct access)
           ~0μs
```

**Implementation**:
```c
// In nr_ru_procedures.c - skip memcpy for O-RAN mode
if (ru->if_south == REMOTE_IF4p5_ORAN) {
  // Direct txdataF access - no memcpy needed
  // O-RAN reads directly from gNB->common_vars.txdataF
} else {
  // Original path for other fronthaul modes
  memcpy(txdataF_BF[ant], &txdataF[ant][slot_offset], ...);
}
```

**Key Commit**: `61342aabc8` - O-RAN FHI 7.2 TX with direct txdataF access

### Parallel FH South Out (Symbol-Level)

**Key Innovation**: Parallelize fronthaul packet generation across 56 tasks (4 antennas × 14 symbols) using enkiTS.

**Location**: `radio/fhi_72/oaioran.c`

```c
typedef struct {
  ru_info_t *ru;
  int tti;
  int ant_id;
  int sym_id;
} fh_tx_symbol_task_args_t;

static void fh_tx_symbol_task(uint32_t start, uint32_t end,
                               uint32_t threadNum, void* pArgs) {
  // Process one antenna-symbol combination
  for (uint32_t i = start; i < end; i++) {
    fh_tx_symbol_task_args_t *args = &((fh_tx_symbol_task_args_t*)pArgs)[i];
    // xRAN packetization for single antenna-symbol
  }
}
```

**Performance**:
- `fh_south_out` reduced from ~173μs to ~66μs (2.6x speedup)
- Total RU TX time: ~273μs → ~66μs (4x improvement)

**Key Commit**: `97facf00da` - FH South Out with symbol-level parallelization

---

## 8. MAC Scheduler Optimizations

### Maximum RB Allocation

**Key Innovation**: Always allocate maximum RBs when RLC buffer has any pending data. Compensates for stale buffer queries in the scheduling pipeline.

**Location**: `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_dlsch.c`

**Rationale**: The RLC buffer status query occurs before scheduling decisions, but more data may arrive by transmission time. Allocating max RBs ensures the pipe is fully utilized.

**Performance**: ~15% throughput improvement (520 Mbps → ~600 Mbps with 273 RBs)

**Key Commit**: `255a856053` - MAC scheduler: Always allocate max RBs when buffer has data

### GTP-U Receive Buffer Increase

**Location**: `openair3/ocp-gtpu/gtp_itf.cpp`

Increased SO_RCVBUF to 64MB to prevent packet drops during high throughput downlink.

**Key Commit**: `0636f68ed8` - Increase GTP-U receive buffer to 64MB

---

## 9. Phase Rotation Optimizations

### Symbol-First Processing

**Key Innovation**: Reorder loops to process all antennas for the same symbol, improving L2 cache locality.

**Location**: `openair1/SCHED_NR/phy_procedures_nr_gNB.c`

**Before** (antenna-first):
```c
for (int ant = 0; ant < num_antennas; ant++) {
  for (int sym = 0; sym < 14; sym++) {
    apply_rotation(ant, sym);  // Poor cache locality
  }
}
```

**After** (symbol-first):
```c
for (int sym = 0; sym < 14; sym++) {
  // Hoist invariants: rotation coefficients, symbol offsets
  for (int ant = 0; ant < num_antennas; ant++) {
    apply_rotation(ant, sym);  // Better L2 cache hit rate
  }
}
```

**Benefits**:
- 52KB working set per symbol fits in L2 cache
- Hoisted invariant calculations
- Inlined rotation logic

### 2-Layer Detection

When only 2 layers are active, skip phase rotation for zero-filled antennas 2-3:

```c
int active_antennas = (nrOfLayers == 2) ? 2 : num_antennas;
for (int ant = 0; ant < active_antennas; ant++) {
  // Only process active antennas
}
```

**Performance**: 50% reduction in rotation operations for 2-layer mode.

**Key Commit**: `ec6b97a5d1` - Phase rotation with symbol-first processing and 2-layer detection

---

## Performance Results

### Slot Timing Summary (273 RBs, 4x4 MIMO)

| Phase | Before | After | Improvement |
|-------|--------|-------|-------------|
| Memory Clear | ~35μs (blocking) | ~0μs (hidden) | 100% hidden |
| PDSCH Encoding | ~160μs | ~160μs | (overlaps memclear) |
| Precoding | ~400μs | ~0μs | Eliminated (PMI=0) |
| RE Mapping | ~90μs | ~31μs | -65% |
| FH South Out | ~173μs | ~66μs | -62% |
| feptx_prec | ~115μs | ~0μs | Eliminated (direct access) |

### Overall Metrics

- **Average slot time**: 457.7μs (within 500μs budget)
- **P95 latency**: 572μs
- **P99 latency**: 606μs
- **Throughput**: ~600 Mbps (up from ~520 Mbps)

---

## Commit History Summary

| Commit | Description |
|--------|-------------|
| `54351d645e` | AVX2 gather optimization and parallel 256QAM modulation |
| `255a856053` | MAC scheduler: Always allocate max RBs |
| `b42bd05701` | Fused modulation + layer mapping for 256QAM |
| `3d99994c1d` | Extend PMI=0 fast path to all layer counts (1-4) |
| `0636f68ed8` | GTP-U receive buffer to 64MB |
| `23fd5728ca` | Async memclear with encoding overlap |
| `97facf00da` | FH South Out symbol-level parallelization |
| `e2b8351f14` | enkiTS infinite spin count for real-time |
| `61342aabc8` | O-RAN direct txdataF access |
| `abb69f6f78` | PMI=0 fast path and enkiTS spinning workers |
| `f98e7e82e5` | Split memory clear into 8 tasks |
| `ec6b97a5d1` | Phase rotation optimization |

---

## Future Optimization Opportunities

| Optimization | Status | Expected Benefit |
|-------------|--------|------------------|
| PRS generation parallelization | Ready | Parallel symbol processing |
| SSB generation parallelization | Ready | Parallel symbol processing |
| CSI-RS generation parallelization | Ready | Parallel symbol processing |
| ACC100 multi-queue encoding | Tested (reverted) | Incompatible with enkiTS |
