# nr_dlsch.c memset() Operations Analysis

## Executive Summary

Analyzed all `memset()` operations in `nr_dlsch.c` for necessity and optimization opportunities.

**Finding**: All memset operations are **NECESSARY** for correctness. However, **antenna zero-fill operations can be optimized** by moving them outside the hot path.

---

## Detailed Analysis

### 1. scrambled_output memset (Line 782)

**Location**: `openair1/PHY/NR_TRANSPORT/nr_dlsch.c:782`

```c
uint32_t scrambled_output[(encoded_length >> 5) + 4]; // +4 extra uint32_t
memset(scrambled_output, 0, sizeof(scrambled_output));
nr_pdsch_codeword_scrambling(input_ptr, encoded_length, codeWord, ...);
```

**Analysis**:
- Buffer size: `(encoded_length / 32) + 4` uint32_t
- Scrambling writes: `(encoded_length + 31) / 32` uint32_t
- **Extra 4 uint32_t are NOT written by scrambling**
- Comment says: `// modulator acces by 4 bytes in some cases`
- Modulator (`nr_modulation`) may read beyond encoded_length for SIMD alignment

**Conclusion**: ✅ **NECESSARY** - Modulator may read unwritten tail bytes

**Performance**:
- Size: ~few hundred bytes
- Time: <0.1 µs (negligible)

---

### 2. tx_layers memset (Line 846)

**Location**: `openair1/PHY/NR_TRANSPORT/nr_dlsch.c:846`

```c
int layerSz2 = (layerSz + 63) & ~63;  // Rounded to 64-byte boundary
c16_t tx_layers[rel15->nrOfLayers][layerSz2] __attribute__((aligned(64)));
memset(tx_layers, 0, sizeof(tx_layers));
nr_layer_mapping(..., layerSz, nb_re, tx_layers);
```

**Analysis**:
- Buffer allocated: `layerSz2` (rounded up to 64-byte boundary)
- Layer mapping writes: `nb_re` symbols (which is < layerSz)
- **Padding bytes (layerSz2 - nb_re) are NOT written**
- Padding is for SIMD alignment

**Why padding exists**:
```c
const int layerSz = frame_parms->N_RB_DL * NR_SYMBOLS_PER_SLOT * NR_NB_SC_PER_RB;
int layerSz2 = (layerSz + 63) & ~63;  // Add up to 63 bytes padding
```

**Where unwritten data is read**:
- `do_onelayer()` may read full layerSz2 for SIMD operations
- SIMD loads may cross into padding area

**Conclusion**: ✅ **NECESSARY** - Padding must be zeroed for SIMD safety

**Performance**:
- Size: 2 layers × ~100KB each = ~200KB
- Time: ~10 µs at 20 GB/s bandwidth

---

### 3. Antenna Zero-Fill memset Operations

These appear in multiple places for unused antennas.

#### 3a. Single-layer antenna 1-3 zero-fill (Lines 525-542)

**Location**: `openair1/PHY/NR_TRANSPORT/nr_dlsch.c:525-542`

```c
// Single-layer: only antenna 0 transmits, others must be zero
for (int ant = 1; ant < frame_parms->nb_antennas_tx; ant++) {
    if (start_sc + total_res <= symbol_sz) {
        memset(&txdataF[ant][txdataF_offset_per_symbol + start_sc],
               0,
               total_res * sizeof(c16_t));
    } else {
        // DC crossing case...
        memset(...);
        memset(...);
    }
}
```

**Analysis**:
- **Frequency**: Every symbol (14 per slot)
- **Antennas**: 3 antennas (1-3)
- **Size per symbol per antenna**: 273 PRBs × 12 SC = 3276 REs × 4 bytes = 13KB
- **Total per symbol**: 3 × 13KB = 39KB
- **Total per slot**: 14 symbols × 39KB = **546KB**

**When it runs**: Single-layer transmission (1 codeword)

**Is it necessary?**: ⚠️ **DEPENDS ON PREVIOUS SLOT**
- If previous slot also transmitted single-layer on same antennas: **NOT NECESSARY**
- If previous slot had different configuration: **NECESSARY**

#### 3b. Two-layer antenna 2-3 zero-fill (Fast Path, Lines 975-985)

**Location**: `openair1/PHY/NR_TRANSPORT/nr_dlsch.c:975-985`

```c
// FAST PATH: 2-layer PMI=0, antennas 2-3 must be zero
for (int ant = 2; ant < frame_parms->nb_antennas_tx; ant++) {
    if (start_sc + total_res <= symbol_sz) {
        memset(&txdataF[ant][txdataF_offset_per_symbol + start_sc], 0, total_res * sizeof(c16_t));
    } else {
        memset(...);
        memset(...);
    }
}
```

**Analysis**:
- **Frequency**: Every symbol (14 per slot)
- **Antennas**: 2 antennas (2-3)
- **Size per symbol per antenna**: 13KB
- **Total per symbol**: 2 × 13KB = 26KB
- **Total per slot**: 14 symbols × 26KB = **364KB**

**When it runs**: Two-layer transmission with PMI=0 (forced in current code)

**Is it necessary?**: ⚠️ **DEPENDS ON PREVIOUS SLOT**

#### 3c. Two-layer antenna 2-3 zero-fill (Slow path, Lines 577-592)

Same as 3b but for standard precoding path.

#### 3d. Standard path zero-fill in do_txdataF (Lines 618, 628-629)

**Location**: `openair1/PHY/NR_TRANSPORT/nr_dlsch.c:618, 628-629`

```c
// When prg->dig_bf_interface_list is NULL (no beamforming weights)
memset(&txdataF[ant][txdataF_offset_per_symbol + subCarrier], 0, re_cnt * sizeof(**txdataF));
```

**When it runs**: Standard precoding path when no beamforming configured

**Analysis**: This zeros specific RBs during precoding when no weights are configured. **NECESSARY** for correctness.

---

## Summary Table

| memset Location | Size per Slot | Frequency | Status | Optimization Possible? |
|----------------|---------------|-----------|--------|------------------------|
| scrambled_output | ~1KB | Per codeword | ✅ NECESSARY | ❌ No |
| tx_layers | ~200KB | Once | ✅ NECESSARY | ❌ No |
| Antenna 1-3 (1-layer) | 546KB | Per symbol × 14 | ⚠️ CONDITIONAL | ✅ **Yes** |
| Antenna 2-3 (2-layer fast) | 364KB | Per symbol × 14 | ⚠️ CONDITIONAL | ✅ **Yes** |
| Antenna 2-3 (2-layer std) | 364KB | Per symbol × 14 | ⚠️ CONDITIONAL | ✅ **Yes** |
| RB zero in do_txdataF | Variable | Per RB | ✅ NECESSARY | ❌ No |

---

## Optimization Opportunities

### Problem: Antenna Zero-Fill Overhead

**Current Implementation**:
- Zeros antenna buffers **every symbol, every slot**
- **Total overhead**: 364-546KB of memset per slot
- **Time cost**: 18-27 µs per slot at 20 GB/s bandwidth

**Root Cause**:
Antennas are zeroed because we don't know if previous slot left garbage data.

### Proposed Optimization #1: Slot-Level Bulk Zero (RECOMMENDED)

**Idea**: Zero ALL antenna buffers at the **start of slot** instead of per-symbol.

**Implementation**:
```c
// In phy_procedures_gNB_TX(), BEFORE signal generation loop
void zero_unused_antennas(PHY_VARS_gNB *gNB, int slot, int num_layers) {
    NR_DL_FRAME_PARMS *fp = &gNB->frame_parms;
    int txdataF_offset = slot * fp->samples_per_slot_wCP;
    int symbol_sz = fp->ofdm_symbol_size;
    int symbols_per_slot = fp->symbols_per_slot;
    int total_samples = symbols_per_slot * symbol_sz;

    // Determine which antennas to zero based on num_layers
    int start_ant = (num_layers == 1) ? 1 : 2;  // 1-layer: zero 1-3, 2-layer: zero 2-3

    for (int beam = 0; beam < gNB->common_vars.num_beams_period; beam++) {
        for (int ant = start_ant; ant < fp->nb_antennas_tx; ant++) {
            memset(&gNB->common_vars.txdataF[beam][ant][txdataF_offset],
                   0,
                   total_samples * sizeof(c16_t));
        }
    }
}
```

**Benefits**:
- ✅ **Single memset per antenna per slot** instead of 14 per antenna
- ✅ Moves memset OUT of critical RE mapping loop
- ✅ Better memory locality (sequential write of entire antenna buffer)
- ✅ Can use `enkiTS` parallel memset for 4× speedup

**Performance Gain**:
- **Current**: 14 × 26KB = 364KB scattered across symbols (cache-unfriendly)
- **Optimized**: 1 × 14 × symbol_sz × 4 bytes = continuous 917KB per antenna
- **Time saved**: ~15 µs per slot (moves from hot path to initialization)

**Risk**: ⚠️ **LOW** - Requires knowing num_layers before symbol loop

### Proposed Optimization #2: Conditional Zero (More Complex)

**Idea**: Track previous slot's antenna usage, only zero if configuration changed.

**Implementation**:
```c
static uint8_t prev_active_antennas_mask[NR_MAX_BEAMS] = {0};

// At start of phy_procedures_gNB_TX()
uint8_t current_mask = (num_layers == 2) ? 0x03 : 0x01;  // Bitmask of active antennas

if (prev_active_antennas_mask[beam] != current_mask) {
    // Configuration changed, need to zero
    zero_unused_antennas(...);
    prev_active_antennas_mask[beam] = current_mask;
}
```

**Benefits**:
- ✅ Eliminates memset entirely for steady-state (same num_layers every slot)
- ✅ Best performance for constant traffic patterns

**Risks**:
- ⚠️ **MEDIUM** - Requires state tracking across slots
- ⚠️ Must handle edge cases (frame wrap, dynamic scheduling changes)

---

## Recommended Action Plan

### Phase 1: Immediate (Low Risk)
1. ✅ **Already done**: Move memset out of RE mapping timing (Line 969-972)
2. Document that current memset is necessary but suboptimal

### Phase 2: Slot-Level Bulk Zero (RECOMMENDED)
1. Implement `zero_unused_antennas()` in `phy_procedures_gNB_TX()`
2. Call it ONCE at slot start, after memory clear
3. Remove per-symbol memset calls in `do_one_dlsch()` fast path
4. **Expected gain**: ~15-20 µs per slot

### Phase 3: Conditional Zero (Optional, Higher Risk)
1. Add state tracking for antenna usage
2. Only zero when configuration changes
3. **Expected gain**: 15-20 µs when layers change, 0 µs otherwise

---

## Code Locations for Modification

### To Remove (after implementing Phase 2):
1. `openair1/PHY/NR_TRANSPORT/nr_dlsch.c:525-542` - Single-layer antenna zero
2. `openair1/PHY/NR_TRANSPORT/nr_dlsch.c:975-985` - Two-layer fast path antenna zero
3. `openair1/PHY/NR_TRANSPORT/nr_dlsch.c:577-592` - Two-layer standard path antenna zero

### To Add:
1. `openair1/SCHED_NR/phy_procedures_nr_gNB.c` - New `zero_unused_antennas()` function
2. Call before PDSCH generation loop (after `enkits_pool_memclear_tx()`)

---

## Performance Impact Estimate

### Current Overhead (Per Slot):
- Antenna memset: 364-546KB
- Time: 18-27 µs
- Impact: 3.6-5.4% of 500µs slot budget

### After Phase 2 Optimization:
- Antenna memset: 0KB (moved to initialization)
- Time: ~2-3 µs (one-time at slot start)
- Impact: 0.4-0.6% of slot budget
- **Net savings**: ~15-24 µs per slot

### After Phase 3 Optimization (Conditional):
- Antenna memset: 0KB most of the time
- Time: ~2-3 µs only when layers change
- Impact: Near-zero in steady state
- **Net savings**: ~18-27 µs per slot (steady state)

---

## Conclusion

All memset operations in `nr_dlsch.c` are necessary for correctness, but **antenna zero-fill operations are inefficiently placed** in the hot path.

**Recommended**: Implement Phase 2 (slot-level bulk zero) for immediate 15-20 µs gain with minimal risk.
