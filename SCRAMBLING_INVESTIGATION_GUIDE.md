# Scrambling Performance Investigation Guide

## Problem Summary

Scrambling shows **anomalous performance** in certain slots:
- **Normal**: 1-10μs for typical workloads
- **Anomaly**: 113-150μs in high-PRB scenarios (273 PRBs, Frame 231/Slot 6, Frame 560/Slot 2)
- **Expected operation**: Simple Gold sequence XOR (~2-5μs maximum)

## Root Cause Hypothesis

### Theory 1: Gold Sequence Cache Misses
**Evidence**:
```c
// openair1/PHY/NR_REFSIG/refsig.c:103-127
uint32_t *gold_cache(uint32_t key, int length) {
    // Linear search through cache
    for (; ptr < tableCache->table + tableCache->tblSz; ptr += roundedHeaderSz) {
        tableCache->iterate++;  // Search iteration counter
        if (tbl->length >= length && tbl->key == key) {
            tbl->usage++;
            return ptr + roundedHeaderSz;  // Cache HIT
        }
        // ...
    }
    // Cache MISS → regenerate sequence (EXPENSIVE!)
    lte_gold_generic(&x1, &x2, length);  // ← BOTTLENECK SUSPECT
}
```

**Why it might be slow**:
1. **Linear cache search** - No hash table, O(n) lookup
2. **Thread-local cache** - Each thread has separate cache (pthread_getspecific)
3. **Cache miss regeneration** - Generates entire Gold sequence from scratch
4. **Large sequence sizes** - 273 PRBs requires large Gold sequence buffers

### Theory 2: Gold Sequence Regeneration Per Symbol
**Scrambling call pattern**:
```c
// openair1/PHY/NR_TRANSPORT/nr_dlsch.c:780
nr_pdsch_codeword_scrambling(input_ptr, encoded_length, codeWord,
                              rel15->dataScramblingId, rel15->rnti, scrambled_output);
    ↓
// openair1/PHY/NR_TRANSPORT/nr_scrambling.c:35
uint32_t *seq = gold_cache((n_RNTI << 15) + (q << 14) + Nid, roundedSz);
```

**Key (c_init) calculation**:
- `key = (n_RNTI << 15) + (q << 14) + Nid`
- `n_RNTI`: UE RNTI (changes per UE)
- `q`: Codeword index (0 or 1)
- `Nid`: Scrambling ID (cell-specific)

**Potential issue**: Different keys for different UEs → cache thrashing

### Theory 3: AVX512 Code Path Not Used
**Check if AVX512 path is executing**:
```c
// openair1/PHY/NR_TRANSPORT/nr_scrambling.c:37-42
#if defined(__AVX512F__) && defined(__AVX512VL__)
  for (; i_32 < ((roundedSz >> 3) << 3); i_32 += 8) {
    __m256i in_256 = _mm256_load_epi32(&((uint32_t *)in)[i_32]);
    __m256i seq_256 = _mm256_load_epi32(&seq[i_32]);
    _mm256_storeu_epi32(&out[i_32], _mm256_xor_si256(in_256, seq_256));
  }
#endif
```

**Potential issue**:
- Compiler flags might not enable AVX512
- Misaligned data forcing scalar fallback
- CPU not supporting AVX512VL

## Investigation Steps

### Step 1: Add Detailed Timing Instrumentation

**Location**: `openair1/PHY/NR_TRANSPORT/nr_scrambling.c`

```c
void nr_codeword_scrambling(uint8_t *in,
                            uint32_t size,
                            uint8_t q,
                            uint32_t Nid,
                            uint32_t n_RNTI,
                            uint32_t* out)
{
  struct timespec t_cache_start, t_cache_end, t_xor_start, t_xor_end;

  // TIME: Gold sequence cache lookup
  clock_gettime(CLOCK_MONOTONIC, &t_cache_start);
  const int roundedSz = (size + 31) / 32;
  uint32_t *seq = gold_cache((n_RNTI << 15) + (q << 14) + Nid, roundedSz);
  clock_gettime(CLOCK_MONOTONIC, &t_cache_end);
  long cache_ns = (t_cache_end.tv_sec - t_cache_start.tv_sec) * 1000000000L +
                  (t_cache_end.tv_nsec - t_cache_start.tv_nsec);

  // TIME: XOR operation
  clock_gettime(CLOCK_MONOTONIC, &t_xor_start);

  unsigned int i_32 = 0;
#if defined(__AVX512F__) && defined(__AVX512VL__)
  unsigned int avx512_count = ((roundedSz >> 3) << 3);
  for (; i_32 < avx512_count; i_32 += 8) {
    __m256i in_256 = _mm256_load_epi32(&((uint32_t *)in)[i_32]);
    __m256i seq_256 = _mm256_load_epi32(&seq[i_32]);
    _mm256_storeu_epi32(&out[i_32], _mm256_xor_si256(in_256, seq_256));
  }
#else
  unsigned int avx512_count = 0;
#endif

  // Scalar remainder
  unsigned int scalar_start = i_32;
  for (; i_32 < roundedSz; i_32++) {
    out[i_32] = ((uint32_t *)in)[i_32] ^ seq[i_32];
  }

  clock_gettime(CLOCK_MONOTONIC, &t_xor_end);
  long xor_ns = (t_xor_end.tv_sec - t_xor_start.tv_sec) * 1000000000L +
                (t_xor_end.tv_nsec - t_xor_start.tv_nsec);

  // Log only if slow (>50μs total)
  if (cache_ns + xor_ns > 50000) {
    LOG_W(PHY, "[SCRAMBLING SLOW] size=%u, roundedSz=%u, cache=%ld ns, xor=%ld ns, "
               "avx512_count=%u, scalar_count=%u, RNTI=%u, q=%u, Nid=%u\n",
          size, roundedSz, cache_ns, xor_ns, avx512_count, roundedSz - scalar_start,
          n_RNTI, q, Nid);
  }
}
```

### Step 2: Add Gold Cache Statistics

**Location**: `openair1/PHY/NR_REFSIG/refsig.c`

```c
uint32_t *gold_cache(uint32_t key, int length)
{
  static __thread int slow_lookup_count = 0;
  static __thread int total_lookups = 0;
  struct timespec t_start, t_end;
  clock_gettime(CLOCK_MONOTONIC, &t_start);

  // ... existing code ...

  // Check if cache hit or miss
  bool cache_hit = false;
  for (; ptr < tableCache->table + tableCache->tblSz; ptr += roundedHeaderSz) {
    gold_cache_t *tbl = (gold_cache_t *)ptr;
    tableCache->iterate++;
    if (tbl->length >= length && tbl->key == key) {
      tbl->usage++;
      cache_hit = true;

      clock_gettime(CLOCK_MONOTONIC, &t_end);
      long lookup_ns = (t_end.tv_sec - t_start.tv_sec) * 1000000000L +
                       (t_end.tv_nsec - t_start.tv_nsec);

      if (lookup_ns > 10000) {  // >10μs is slow for cache hit
        slow_lookup_count++;
        LOG_W(PHY, "[GOLD_CACHE SLOW HIT] key=%u, length=%d, lookup_ns=%ld, iterations=%d\n",
              key, length, lookup_ns, tableCache->iterate);
      }

      return ptr + roundedHeaderSz;
    }
    // ... rest of loop ...
  }

  // Cache miss - regenerate
  clock_gettime(CLOCK_MONOTONIC, &t_end);
  long miss_ns = (t_end.tv_sec - t_start.tv_sec) * 1000000000L +
                 (t_end.tv_nsec - t_start.tv_nsec);

  LOG_W(PHY, "[GOLD_CACHE MISS] key=%u, length=%d, regeneration took %ld ns\n",
        key, length, miss_ns);

  // ... generate new sequence ...
}
```

### Step 3: Check Compiler AVX512 Flags

```bash
# Check if code was compiled with AVX512 support
cd cmake_targets/ran_build/build
grep -r "AVX512" CMakeCache.txt

# Check object file for AVX512 instructions
objdump -d openair1/PHY/NR_TRANSPORT/CMakeFiles/NR_TRANSPORT.dir/nr_scrambling.c.o | grep -E "vxor|vpxor|vmov"

# Check if CPU supports AVX512
lscpu | grep -i avx512
cat /proc/cpuinfo | grep -i avx512
```

### Step 4: Profile Slow Slot Scenario

**Create test to reproduce Frame 560, Slot 2 scenario**:

```bash
# Run with L1 timing and capture logs
sudo ./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.273prb.fhi72.4x4-liteon.conf \
  --thread-pool 1,3 \
  --loader.ldpc.shlibversion _aal \
  --nrLDPC_coding_aal.dpdk_dev 0000:87:00.0 \
  --nrLDPC_coding_aal.dpdk_core_list 16-17 \
  --nrLDPC_coding_aal.vfio_vf_token c2d9f0a2-bc24-4a83-8126-9fbb22f3ce12 \
  --log_config.phy_log_level debug 2>&1 | tee scrambling_profile.log

# Filter for slow scrambling events
grep "SCRAMBLING SLOW" scrambling_profile.log
grep "GOLD_CACHE" scrambling_profile.log
```

### Step 5: Analyze CSV for Scrambling Patterns

```bash
# Extract slots with scrambling >50μs
awk -F',' 'NR>1 && $11 > 50000 {
    printf "Frame=%s Slot=%s RBs=%s Scrambling=%0.f μs\n", $1, $2, $3, $11/1000
}' l1_downlink_timing.csv | sort -t'=' -k4 -n
```

## Expected Findings

### Scenario A: Cache Miss Problem
**Symptoms**:
- High `cache_ns` time (>50μs)
- Frequent "GOLD_CACHE MISS" logs
- High iteration count in cache search

**Solution**:
- Implement hash table for O(1) lookup
- Pre-generate common Gold sequences at init
- Increase cache size for high-PRB scenarios

### Scenario B: AVX512 Not Used
**Symptoms**:
- `avx512_count=0` in all logs
- High `xor_ns` relative to data size
- No AVX512 instructions in objdump

**Solution**:
- Add `-mavx512f -mavx512vl` to CMake flags
- Fix data alignment issues
- Verify CPU support

### Scenario C: Large Data Volume
**Symptoms**:
- Linear correlation between scrambling time and PRB count
- Cache hits but still slow (>10μs for lookup)
- Normal XOR performance

**Analysis**:
273 PRBs × 12 subcarriers × 64QAM (6 bits/symbol) × 14 symbols = **large bit array**
- Encoded bits: ~200,000 bits = 6,250 uint32_t
- XOR operations: 6,250 × 32-bit XOR
- Expected time with AVX512: ~5-10μs
- Actual time: 113μs → **10-20× slower than expected**

**Solution**: Likely combination of cache miss + no AVX512

## Next Steps After Investigation

1. **Implement fixes based on findings**
2. **Re-run timing measurement**
3. **Verify scrambling time <10μs for 273 PRBs**
4. **Update CLAUDE.md with findings**

## Files to Modify

- `openair1/PHY/NR_TRANSPORT/nr_scrambling.c` - Add instrumentation
- `openair1/PHY/NR_REFSIG/refsig.c` - Add cache statistics
- `cmake_targets/CMakeLists.txt` - Add AVX512 flags if needed
- `openair1/PHY/CMakeLists.txt` - Verify SIMD optimization flags

## Reference Data

From CSV analysis:
- **Normal scrambling**: 1-10μs (most slots)
- **Anomaly cases**:
  - Frame 231, Slot 6: 150μs scrambling (198 PRBs, MCS 21)
  - Frame 560, Slot 2: 113μs scrambling (273 PRBs, MCS 21)
  - Expected for 273 PRBs: ~5-10μs with AVX512

**Bottleneck impact**: Scrambling alone accounts for 16-30% of PDSCH generation time in worst cases.
