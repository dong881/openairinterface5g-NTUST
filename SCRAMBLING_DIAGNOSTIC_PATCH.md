# Scrambling Performance Diagnostic Patch

## Quick Start

```bash
# 1. Apply instrumentation patch
cd /home/kelvin/openairinterface5g

# 2. Rebuild
cd cmake_targets/
./build_oai --gNB --ninja -t oran_fhlib_5g --cmake-opt -Dxran_LOCATION=$HOME/RU/phy/fhi_lib/lib -P --build-lib "ldpc_aal"

# 3. Run and capture diagnostics
sudo ./cmake_targets/ran_build/build/nr-softmodem \
  -O ../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.273prb.fhi72.4x4-liteon.conf \
  --thread-pool 1,3 \
  --loader.ldpc.shlibversion _aal \
  --nrLDPC_coding_aal.dpdk_dev 0000:87:00.0 \
  --nrLDPC_coding_aal.dpdk_core_list 16-17 \
  --nrLDPC_coding_aal.vfio_vf_token c2d9f0a2-bc24-4a83-8126-9fbb22f3ce12 \
  2>&1 | tee scrambling_diagnostic.log

# 4. Analyze results
grep "SCRAMBLING_SLOW" scrambling_diagnostic.log
grep "GOLD_CACHE" scrambling_diagnostic.log
```

## Patch 1: Add Timing to nr_codeword_scrambling()

**File**: `openair1/PHY/NR_TRANSPORT/nr_scrambling.c`

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
  unsigned int avx512_iters = ((roundedSz >> 3) << 3);
  for (; i_32 < avx512_iters; i_32 += 8) {
    __m256i in_256 = _mm256_load_epi32(&((uint32_t *)in)[i_32]);
    __m256i seq_256 = _mm256_load_epi32(&seq[i_32]);
    _mm256_storeu_epi32(&out[i_32], _mm256_xor_si256(in_256, seq_256));
  }
  unsigned int avx512_count = i_32;
#else
  unsigned int avx512_count = 0;
  unsigned int avx512_iters = 0;
#endif

  // Scalar remainder
  unsigned int scalar_start = i_32;
  for (; i_32 < roundedSz; i_32++) {
    unsigned int i = i_32;
    out[i] = ((uint32_t *)in)[i] ^ seq[i];
    DEBUG_SCRAMBLING(LOG_D(PHY, "in[%d] %x => %x\n", i, ((uint32_t*)in)[i], out[i]));
  }

  clock_gettime(CLOCK_MONOTONIC, &t_xor_end);
  long xor_ns = (t_xor_end.tv_sec - t_xor_start.tv_sec) * 1000000000L +
                (t_xor_end.tv_nsec - t_xor_start.tv_nsec);

  long total_ns = cache_ns + xor_ns;

  // Log if total time > 50μs (anomaly threshold)
  static __thread int slow_count = 0;
  static __thread long max_cache_ns = 0;
  static __thread long max_xor_ns = 0;

  if (total_ns > 50000) {
    slow_count++;
    if (cache_ns > max_cache_ns) max_cache_ns = cache_ns;
    if (xor_ns > max_xor_ns) max_xor_ns = xor_ns;

    LOG_W(PHY, "[SCRAMBLING_SLOW #%d] Total=%ld μs | cache=%ld μs, xor=%ld μs | "
               "size=%u bits, roundedSz=%u uint32s, avx_iters=%u, scalar=%u | "
               "RNTI=0x%04x, q=%u, Nid=%u | max_cache=%ld μs, max_xor=%ld μs\n",
          slow_count, total_ns/1000, cache_ns/1000, xor_ns/1000,
          size, roundedSz, avx512_iters, roundedSz - scalar_start,
          n_RNTI, q, Nid, max_cache_ns/1000, max_xor_ns/1000);
  }
}
```

## Patch 2: Add Gold Cache Hit/Miss Statistics

**File**: `openair1/PHY/NR_REFSIG/refsig.c`

Insert at line 103 in `gold_cache()`:

```c
uint32_t *gold_cache(uint32_t key, int length)
{
  struct timespec t_lookup_start, t_lookup_end;
  clock_gettime(CLOCK_MONOTONIC, &t_lookup_start);

  // Thread-local statistics
  static __thread int total_calls = 0;
  static __thread int cache_hits = 0;
  static __thread int cache_misses = 0;
  static __thread long max_lookup_ns = 0;
  static __thread int max_iterations = 0;

  total_calls++;

  (void)pthread_once(&gold_key_once, make_table_key);
  gold_cache_table_t *tableCache;
  if ((tableCache = pthread_getspecific(gold_table_key)) == NULL) {
    tableCache = calloc(1, sizeof(gold_cache_table_t));
    (void)pthread_setspecific(gold_table_key, tableCache);
  }

  // align for AVX512
  length = ((length + grain - 1) / grain) * grain;
  tableCache->calls++;

  // periodic refresh
  if (tableCache->calls > REFRESH_RATE)
    refresh_table(tableCache, 0);

  uint32_t *ptr = tableCache->table;
  int iterations_this_call = 0;
  bool found = false;

  // check if already cached
  for (; ptr < tableCache->table + tableCache->tblSz; ptr += roundedHeaderSz) {
    gold_cache_t *tbl = (gold_cache_t *)ptr;
    tableCache->iterate++;
    iterations_this_call++;

    if (tbl->length >= length && tbl->key == key) {
      tbl->usage++;
      found = true;
      cache_hits++;

      clock_gettime(CLOCK_MONOTONIC, &t_lookup_end);
      long lookup_ns = (t_lookup_end.tv_sec - t_lookup_start.tv_sec) * 1000000000L +
                       (t_lookup_end.tv_nsec - t_lookup_start.tv_nsec);

      if (lookup_ns > max_lookup_ns) max_lookup_ns = lookup_ns;
      if (iterations_this_call > max_iterations) max_iterations = iterations_this_call;

      if (lookup_ns > 10000) {  // >10μs for cache hit is slow
        LOG_W(PHY, "[GOLD_CACHE_HIT_SLOW] key=0x%08x, length=%d, lookup=%ld μs, iters=%d | "
                   "hit_rate=%.1f%%, max_lookup=%ld μs, max_iters=%d\n",
              key, length, lookup_ns/1000, iterations_this_call,
              (cache_hits * 100.0) / total_calls, max_lookup_ns/1000, max_iterations);
      }

      return ptr + roundedHeaderSz;
    }

    if (tbl->key == key) {
      // We use a longer sequence, same key
      // let's delete the shorter and force reorganize
      tbl->usage = 0;
      tableCache->calls += REFRESH_RATE;
    }
    if (!tbl->length)
      break;
    ptr += tbl->length;
  }

  // CACHE MISS - need to generate
  cache_misses++;

  clock_gettime(CLOCK_MONOTONIC, &t_lookup_end);
  long search_ns = (t_lookup_end.tv_sec - t_lookup_start.tv_sec) * 1000000000L +
                   (t_lookup_end.tv_nsec - t_lookup_start.tv_nsec);

  // not enough space in the table
  if (!ptr || ptr > tableCache->table + tableCache->tblSz - (2 * roundedHeaderSz + length))
    refresh_table(tableCache, 2 * roundedHeaderSz + length);

  // We will add a new entry
  uint32_t *firstFree;
  int size = 0;
  for (firstFree = tableCache->table; firstFree < tableCache->table + tableCache->tblSz; firstFree += roundedHeaderSz) {
    gold_cache_t *tbl = (gold_cache_t *)firstFree;
    if (!tbl->length)
      break;
    firstFree += tbl->length;
    size++;
  }

  AssertFatal(firstFree <= tableCache->table + tableCache->tblSz, "programming error in gold sequence cache");
  uint32_t x1 = 0, x2 = key;
  LOG_D(PHY, "generating gold sequence for key %x (N_RB_DL %d, Ns %d, %d bits)\n", key, 0, 0, length);

  struct timespec t_gen_start, t_gen_end;
  clock_gettime(CLOCK_MONOTONIC, &t_gen_start);

  lte_gold_generic(&x1, &x2, length);

  clock_gettime(CLOCK_MONOTONIC, &t_gen_end);
  long gen_ns = (t_gen_end.tv_sec - t_gen_start.tv_sec) * 1000000000L +
                (t_gen_end.tv_nsec - t_gen_start.tv_nsec);
  long total_miss_ns = search_ns + gen_ns;

  gold_cache_t *tbl = (gold_cache_t *)firstFree;
  tbl->key = key;
  tbl->length = length;
  tbl->usage = 1;
  uint32_t *newSeq = firstFree + roundedHeaderSz;
  for (int n = 0; n < length; n++) {
    uint32_t x1tmp, x2tmp;
    x1tmp = x1;
    x2tmp = x2;
    uint32_t b = 0;
    for (int i = 0; i < 32; i++) {
      b |= ((x1 ^ x2) & 1) << i;
      x1 = (x1 >> 1) ^ ((x1 & 1) ? 0x80200003 : 0);
      x2 = (x2 >> 1) ^ ((x2 & 1) ? 0x80600003 : 0);
    }
    newSeq[n] = b;
  }

  LOG_W(PHY, "[GOLD_CACHE_MISS] key=0x%08x, length=%d | search=%ld μs, gen=%ld μs, total=%ld μs | "
             "hit_rate=%.1f%%, cache_entries=%d, iters=%d\n",
        key, length, search_ns/1000, gen_ns/1000, total_miss_ns/1000,
        (cache_hits * 100.0) / total_calls, size, iterations_this_call);

  return newSeq;
}
```

## Expected Output Patterns

### Pattern A: Gold Cache Miss Bottleneck
```
[GOLD_CACHE_MISS] key=0x80123456, length=6250 | search=2 μs, gen=98 μs, total=100 μs | ...
[SCRAMBLING_SLOW #1] Total=105 μs | cache=102 μs, xor=3 μs | size=200000 bits, ...
```
**Diagnosis**: Cache miss causing regeneration (98μs). Gold sequence generation is the bottleneck.

### Pattern B: Linear Search in Cache
```
[GOLD_CACHE_HIT_SLOW] key=0x80123456, length=6250, lookup=45 μs, iters=2500 | ...
[SCRAMBLING_SLOW #2] Total=50 μs | cache=47 μs, xor=3 μs | ...
```
**Diagnosis**: Cache hit but slow lookup (45μs due to 2500 linear search iterations). Need hash table.

### Pattern C: XOR Performance Issue
```
[SCRAMBLING_SLOW #3] Total=80 μs | cache=3 μs, xor=77 μs | size=200000 bits, avx_iters=0, scalar=6250 | ...
```
**Diagnosis**: AVX not being used (avx_iters=0), falling back to scalar XOR. Compilation or alignment issue.

### Pattern D: Normal Operation
```
(No SCRAMBLING_SLOW logs)
```
**Diagnosis**: All scrambling operations <50μs. System working correctly.

## Analysis Commands

```bash
# Count slow scrambling events
grep -c "SCRAMBLING_SLOW" scrambling_diagnostic.log

# Show worst cases
grep "SCRAMBLING_SLOW" scrambling_diagnostic.log | sort -t'=' -k2 -nr | head -10

# Check cache miss rate
grep "GOLD_CACHE" scrambling_diagnostic.log | grep -oP "hit_rate=\K[0-9.]+" | tail -1

# Check if AVX is being used
grep "SCRAMBLING_SLOW" scrambling_diagnostic.log | grep "avx_iters=0"

# Breakdown: cache vs XOR time
awk -F'[| =]' '/SCRAMBLING_SLOW/ {
    cache+=$6; xor+=$10; count++
} END {
    print "Avg cache time:", cache/count/1000, "μs"
    print "Avg XOR time:", xor/count/1000, "μs"
    print "Cache is", (cache/(cache+xor))*100, "% of total"
}' scrambling_diagnostic.log
```

## Next Steps Based on Findings

### If Cache Misses are the problem:
1. Pre-generate common Gold sequences at initialization
2. Increase cache size
3. Implement hash table lookup instead of linear search

### If Linear Search is slow:
1. Replace linear search with hash table (O(1) lookup)
2. Or keep MRU (Most Recently Used) entries at front

### If AVX not being used:
1. Check alignment of `in` and `seq` buffers
2. Verify AVX512 compile flags
3. Check for CPU support at runtime

### If Gold Generation is slow:
1. Profile `lte_gold_generic()` function
2. Consider pre-computing common sequences
3. Use faster PRNG for Gold sequence generation
