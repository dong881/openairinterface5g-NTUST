# How to Investigate Scrambling Performance Bottleneck

## TL;DR - Quick Investigation

```bash
cd /home/kelvin/openairinterface5g

# 1. Read the full investigation guide
cat SCRAMBLING_INVESTIGATION_GUIDE.md

# 2. Apply diagnostic patches from
cat SCRAMBLING_DIAGNOSTIC_PATCH.md

# 3. The patches will add timing instrumentation to:
#    - nr_codeword_scrambling() - Track cache vs XOR time
#    - gold_cache() - Track cache hits/misses and generation time

# 4. Rebuild and run
cd cmake_targets/
./build_oai --gNB --ninja -t oran_fhlib_5g \
  --cmake-opt -Dxran_LOCATION=$HOME/RU/phy/fhi_lib/lib -P \
  --build-lib "ldpc_aal"

# 5. Run with diagnostics
sudo ./cmake_targets/ran_build/build/nr-softmodem \
  -O ../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.273prb.fhi72.4x4-liteon.conf \
  --thread-pool 1,3 \
  --loader.ldpc.shlibversion _aal \
  --nrLDPC_coding_aal.dpdk_dev 0000:87:00.0 \
  --nrLDPC_coding_aal.dpdk_core_list 16-17 \
  --nrLDPC_coding_aal.vfio_vf_token c2d9f0a2-bc24-4a83-8126-9fbb22f3ce12 \
  2>&1 | tee scrambling_diagnostic.log

# 6. Analyze results
grep "SCRAMBLING_SLOW" scrambling_diagnostic.log
grep "GOLD_CACHE" scrambling_diagnostic.log
```

## Problem Summary

**Current State**: Scrambling shows **10-50× slowdown** in high-PRB scenarios

| Metric | Normal | Anomaly | Expected |
|--------|--------|---------|----------|
| **Typical (44 PRBs)** | 1-10 μs | - | 2-5 μs |
| **High PRB (273 PRBs)** | - | 113-150 μs | 5-10 μs |
| **Slowdown Factor** | - | **10-50×** | 1× |

**Evidence from CSV**:
- Frame 231, Slot 6: **150 μs** scrambling (198 PRBs, MCS 21)
- Frame 560, Slot 2: **113 μs** scrambling (273 PRBs, MCS 21)
- Impact: 16-30% of total PDSCH generation time in worst cases

## Root Cause Theories (Ranked by Likelihood)

### Theory 1: Gold Sequence Cache Misses (HIGH PROBABILITY) 🔴
**Mechanism**: Linear cache search + expensive regeneration

**Evidence**:
```c
// openair1/PHY/NR_REFSIG/refsig.c:122-138
for (; ptr < tableCache->table + tableCache->tblSz; ptr += roundedHeaderSz) {
    gold_cache_t *tbl = (gold_cache_t *)ptr;
    tableCache->iterate++;  // O(n) linear search
    if (tbl->length >= length && tbl->key == key) {
        return ptr + roundedHeaderSz;  // Cache HIT
    }
    // ...
}
// Cache MISS → lte_gold_generic() regenerates entire sequence
```

**Why this causes 113μs**:
1. **Cache miss**: Different UE RNTIs → different keys → miss
2. **Expensive regeneration**: `lte_gold_generic()` generates 273 PRBs worth of pseudo-random bits
3. **Linear search overhead**: Large cache → many iterations before miss detected

**Test**: Check if `GOLD_CACHE_MISS` logs correlate with `SCRAMBLING_SLOW`

### Theory 2: Linear Search in Large Cache (MEDIUM PROBABILITY) 🟡
**Mechanism**: Even cache hits take long due to O(n) search

**Why this causes 113μs**:
- Thread-local cache grows over time
- No hash table - sequential scan
- 273 PRB sequences stored late in cache → many iterations

**Test**: Check if `GOLD_CACHE_HIT_SLOW` with high `iters` count

### Theory 3: Multi-UE Cache Thrashing (MEDIUM PROBABILITY) 🟡
**Mechanism**: Multiple UEs with different RNTIs evict each other's cache entries

**Key calculation**: `key = (n_RNTI << 15) + (q << 14) + Nid`
- Each UE has unique RNTI → unique cache entry
- Periodic refresh (every 100K calls) might evict useful entries
- High-PRB slots might have multiple UEs

**Test**: Check if cache miss rate increases with multiple UEs

### Theory 4: AVX Not Used (LOW PROBABILITY - VERIFIED UNLIKELY) 🟢
**Status**: **Objdump shows vpxor instructions** - AVX2 is being used

**Evidence**:
```asm
4c:	c5 f5 ef 04 03       	vpxor  (%rbx,%rax,1),%ymm1,%ymm0
b0:	c4 a1 45 ef 14 03    	vpxor  (%rbx,%r8,1),%ymm7,%ymm2
```

**Conclusion**: XOR operation is SIMD-optimized. Not the bottleneck.

## Investigation Files

| File | Purpose |
|------|---------|
| `SCRAMBLING_INVESTIGATION_GUIDE.md` | Detailed technical analysis and theories |
| `SCRAMBLING_DIAGNOSTIC_PATCH.md` | Code patches to add instrumentation |
| `HOW_TO_INVESTIGATE_SCRAMBLING.md` | This file - quick start guide |

## Expected Diagnostic Output

### Scenario A: Cache Miss Bottleneck
```
[GOLD_CACHE_MISS] key=0x80123456, length=6250 | search=2 μs, gen=98 μs, total=100 μs | hit_rate=45.2%
[SCRAMBLING_SLOW #1] Total=105 μs | cache=102 μs, xor=3 μs | RNTI=0x1234
```
**Solution**: Pre-generate common sequences, increase cache, use hash table

### Scenario B: Linear Search Bottleneck
```
[GOLD_CACHE_HIT_SLOW] key=0x80123456, lookup=45 μs, iters=2500 | hit_rate=98.5%
[SCRAMBLING_SLOW #2] Total=50 μs | cache=47 μs, xor=3 μs
```
**Solution**: Replace linear search with hash table (O(1) lookup)

### Scenario C: Both Issues Combined
```
[GOLD_CACHE_MISS] key=0x80456789, gen=80 μs | hit_rate=60.3%
[GOLD_CACHE_HIT_SLOW] key=0x80123456, lookup=30 μs, iters=1800
[SCRAMBLING_SLOW #3] Total=115 μs | cache=112 μs, xor=3 μs
```
**Solution**: Implement comprehensive cache optimization (hash + pre-gen)

## Analysis Commands

### Quick Health Check
```bash
# How many slow events?
grep -c "SCRAMBLING_SLOW" scrambling_diagnostic.log

# What's the cache hit rate?
grep "hit_rate" scrambling_diagnostic.log | tail -1

# Is cache or XOR the bottleneck?
grep "SCRAMBLING_SLOW" scrambling_diagnostic.log | \
  awk -F'[| =]' '{print $6, $10}' | \
  awk '{cache+=$1; xor+=$2; n++} END {print "Cache avg:", cache/n/1000, "μs\nXOR avg:", xor/n/1000, "μs"}'
```

### Detailed Analysis
```bash
# Show worst 10 cases
grep "SCRAMBLING_SLOW" scrambling_diagnostic.log | \
  sort -t'=' -k2 -nr | head -10

# Check for cache misses
grep "GOLD_CACHE_MISS" scrambling_diagnostic.log | wc -l

# Check for slow cache hits
grep "GOLD_CACHE_HIT_SLOW" scrambling_diagnostic.log | wc -l

# Verify AVX is being used (should be NO output if AVX works)
grep "SCRAMBLING_SLOW" scrambling_diagnostic.log | grep "avx_iters=0"
```

## Optimization Roadmap (Based on Findings)

### Phase 1: Quick Win (If Cache Miss is Problem)
**Time**: 1-2 hours
```c
// Pre-generate common Gold sequences at init
void init_gold_cache_preload() {
    // Common RNTI values and PRB sizes
    uint32_t common_rntis[] = {0x1234, 0x5678, 0xABCD};
    int common_prb_sizes[] = {44, 100, 150, 200, 273};

    for (int i = 0; i < sizeof(common_rntis)/sizeof(uint32_t); i++) {
        for (int j = 0; j < sizeof(common_prb_sizes)/sizeof(int); j++) {
            int length = (common_prb_sizes[j] * 12 * 6 + 31) / 32;  // 64QAM
            uint32_t key = (common_rntis[i] << 15) + Nid;
            gold_cache(key, length);  // Pre-load into cache
        }
    }
}
```

### Phase 2: Hash Table Optimization (If Linear Search is Problem)
**Time**: 4-6 hours
- Replace linear search with hash table
- Use `key % table_size` for O(1) lookup
- Keep LRU eviction policy

### Phase 3: Profile Gold Generation (If Gen Time is Issue)
**Time**: 2-4 hours
- Profile `lte_gold_generic()` to find hotspots
- Consider faster PRNG or lookup table approach
- Benchmark alternative implementations

## Success Criteria

| Metric | Current | Target | Status |
|--------|---------|--------|--------|
| Scrambling (44 PRBs) | 1-10 μs | <5 μs | ✓ Already good |
| Scrambling (273 PRBs) | 113-150 μs | <10 μs | ❌ Needs fix |
| Cache hit rate | Unknown | >95% | ? Measure first |
| Gold generation | Unknown | <20 μs | ? Measure first |

**Target**: All scrambling operations <10μs regardless of PRB count

## Related Files

- `openair1/PHY/NR_TRANSPORT/nr_scrambling.c` - Scrambling implementation
- `openair1/PHY/NR_REFSIG/refsig.c` - Gold cache implementation
- `openair1/PHY/LTE_TRANSPORT/transport_proto.h` - lte_gold_generic()
- `l1_downlink_timing.csv` - Performance measurements showing anomaly

## Contact & Next Steps

1. **Apply diagnostic patches** (see `SCRAMBLING_DIAGNOSTIC_PATCH.md`)
2. **Run test and collect logs**
3. **Analyze with provided commands**
4. **Choose optimization based on findings**
5. **Implement fix and verify with new timing CSV**

Expected timeline: **1-2 days** for complete investigation and fix.
