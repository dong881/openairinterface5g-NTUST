/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file nr_dlsch.c
 * \brief Top-level routines for transmission of the PDSCH 38211 v 15.2.0
 * \author Guy De Souza
 * \date 2018
 * \version 0.1
 * \company Eurecom
 * \email: desouza@eurecom.fr
 * \note
 * \warning
 */

#include "nr_dlsch.h"
#include "nr_dci.h"
#include "nr_sch_dmrs.h"
#include "PHY/MODULATION/nr_modulation.h"
#include "PHY/NR_REFSIG/dmrs_nr.h"
#include "PHY/NR_REFSIG/ptrs_nr.h"
#include "PHY/NR_REFSIG/nr_refsig_common.h"  // for gold_cache
#include "PHY/ISIP_POOL/isip_pool.h"
#include "TaskScheduler_c.h"  // enkiTS C API for parallel RE mapping
#include "common/utils/LOG/vcd_signal_dumper.h"
#include "common/utils/nr/nr_common.h"
#include "executables/softmodem-common.h"
#include "SCHED_NR/sched_nr.h"
#include "SCHED_NR/nr_slot_timing.h"

// #define DEBUG_DLSCH
// #define DEBUG_DLSCH_MAPPING
#include <simde/x86/avx512.h>
#define USE128BIT

// ========== ISIP Parallel RE Mapping Support ==========
// Task arguments for parallel symbol×layer processing
typedef struct {
  NR_DL_FRAME_PARMS *frame_parms;
  int slot;
  nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15;
  int layer;
  c16_t *output;           // Output buffer (txdataF or txdataF_precoding)
  c16_t *txl_start;        // Input layer data (pre-calculated offset)
  int start_sc;
  int symbol_sz;
  int l_symbol;
  uint16_t dlPtrsSymPos;
  int n_ptrs;
  int amp;
  int16_t amp_dmrs;
  int l_prime;
  nfapi_nr_dmrs_type_e dmrs_Type;
  c16_t *dmrs_start;       // DMRS symbols array for this symbol
  int layer_sz_result;     // Output: number of REs processed
} re_mapping_task_args_t;

// Maximum number of symbols per slot (14 for normal CP)
#define MAX_SYMBOLS_PER_SLOT 14
// Maximum DMRS symbols per slot (typically 2-4)
#define MAX_DMRS_SYMBOLS 4

// Pre-calculate layer_sz for a symbol based on its type
// Returns the number of data samples consumed from tx_layers per layer
static inline int precalc_layer_sz(nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15,
                                   int l_symbol,
                                   uint16_t dmrs_symbol_map,
                                   uint16_t dlPtrsSymPos,
                                   nfapi_nr_dmrs_type_e dmrs_Type)
{
  const int total_res = rel15->rbSize * NR_NB_SC_PER_RB;  // 12 subcarriers per RB

  // Check if PTRS symbol
  if ((rel15->pduBitmap & 0x1) && is_ptrs_symbol(l_symbol, dlPtrsSymPos)) {
    // PTRS uses some REs - calculate exact count
    // For simplicity, approximate as (total_res - ptrs_count)
    // Actual count depends on PTRS density, but this is close enough for offset calculation
    int ptrs_per_rb = 1;  // Typical: 1 PTRS per RB for time density
    return total_res - rel15->rbSize * ptrs_per_rb;
  }

  // Check if DMRS symbol
  if (dmrs_symbol_map & (1 << l_symbol)) {
    if (dmrs_Type == NFAPI_NR_DMRS_TYPE1) {
      if (rel15->numDmrsCdmGrpsNoData == 2) {
        return 0;  // All REs are DMRS, no data
      } else {  // numDmrsCdmGrpsNoData == 1
        return total_res / 2;  // Half REs are data
      }
    } else {  // DMRS Type 2
      if (rel15->numDmrsCdmGrpsNoData == 3) {
        return 0;  // All REs are DMRS
      } else if (rel15->numDmrsCdmGrpsNoData == 2) {
        return total_res / 3;  // 1/3 REs are data
      } else {  // numDmrsCdmGrpsNoData == 1
        return total_res * 2 / 3;  // 2/3 REs are data
      }
    }
  }

  // Regular data symbol - all REs are data
  return total_res;
}

// Forward declaration for task function
static inline int do_onelayer(NR_DL_FRAME_PARMS *frame_parms, int slot,
                              nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15, int layer,
                              c16_t *output, c16_t *txl_start, int start_sc,
                              int symbol_sz, int l_symbol, uint16_t dlPtrsSymPos,
                              int n_ptrs, int amp, int16_t amp_dmrs, int l_prime,
                              nfapi_nr_dmrs_type_e dmrs_Type, c16_t *dmrs_start);

// ISIP task function for parallel layer RE mapping
static void re_mapping_layer_task(uint32_t start, uint32_t end, uint32_t threadNum, void* pArgs)
{
  re_mapping_task_args_t *args = (re_mapping_task_args_t*)pArgs;
  for (uint32_t i = start; i < end; i++) {
    args[i].layer_sz_result = do_onelayer(
        args[i].frame_parms, args[i].slot, args[i].rel15, args[i].layer,
        args[i].output, args[i].txl_start, args[i].start_sc, args[i].symbol_sz,
        args[i].l_symbol, args[i].dlPtrsSymPos, args[i].n_ptrs, args[i].amp,
        args[i].amp_dmrs, args[i].l_prime, args[i].dmrs_Type, args[i].dmrs_start);
  }
}

// Pre-created ISIP task set for RE mapping (initialized on first use)
static enkiTaskSet* g_re_mapping_task = NULL;

// ========== Symbol-Level Parallel RE Mapping ==========
// Task arguments for parallel symbol processing (processes all layers for one symbol)
typedef struct {
  // Common parameters (shared across all symbols)
  NR_DL_FRAME_PARMS *frame_parms;
  int slot;
  nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15;
  int start_sc;
  int symbol_sz;
  uint16_t dlPtrsSymPos;
  int n_ptrs;
  int amp;
  int16_t amp_dmrs;
  nfapi_nr_dmrs_type_e dmrs_Type;
  uint32_t txdataF_offset;  // Base offset for this slot
  c16_t **txdataF;          // Output array [antenna][sample]
  c16_t *tx_layers_base;    // Base pointer to tx_layers[0][0]
  int layerSz2;             // Aligned layer size for indexing
  int nrOfLayers;
  int nb_antennas_tx;

  // Per-symbol parameters
  int l_symbol;
  int l_prime;              // Pre-computed l_prime for DMRS
  c16_t *mod_dmrs;          // Pre-computed DMRS modulation for this symbol (NULL if not DMRS symbol)
  int re_offset;            // Pre-computed offset into tx_layers for this symbol
} symbol_task_args_t;

// Task function for parallel symbol RE mapping (PMI=0 fast path)
static void re_mapping_symbol_task(uint32_t start, uint32_t end, uint32_t threadNum, void* pArgs)
{
  symbol_task_args_t *all_args = (symbol_task_args_t*)pArgs;

  for (uint32_t sym_idx = start; sym_idx < end; sym_idx++) {
    symbol_task_args_t *args = &all_args[sym_idx];

    const int l_symbol = args->l_symbol;
    const size_t txdataF_offset_per_symbol = l_symbol * args->symbol_sz + args->txdataF_offset;
    const int nrOfLayers = args->nrOfLayers;

    // Calculate DMRS index for this symbol
    uint32_t dmrs_idx = args->rel15->rbStart;
    if (args->rel15->refPoint == 0)
      dmrs_idx += args->rel15->BWPStart;
    dmrs_idx *= args->dmrs_Type == NFAPI_NR_DMRS_TYPE1 ? 6 : 4;

    // Get DMRS pointer (NULL if not a DMRS symbol, already offset if DMRS)
    c16_t *dmrs_start = args->mod_dmrs ? args->mod_dmrs + dmrs_idx : NULL;

    // Process all layers for this symbol (sequential within symbol)
    for (int layer = 0; layer < nrOfLayers; layer++) {
      // Calculate tx_layer pointer: base + layer * layerSz2 + re_offset
      c16_t *tx_layer_ptr = args->tx_layers_base + layer * args->layerSz2 + args->re_offset;

      do_onelayer(args->frame_parms,
                  args->slot,
                  args->rel15,
                  layer,
                  &args->txdataF[layer][txdataF_offset_per_symbol],
                  tx_layer_ptr,
                  args->start_sc,
                  args->symbol_sz,
                  l_symbol,
                  args->dlPtrsSymPos,
                  args->n_ptrs,
                  args->amp,
                  args->amp_dmrs,
                  args->l_prime,
                  args->dmrs_Type,
                  dmrs_start);
    }

    // Zero-fill unused antennas (antennas beyond nrOfLayers for PMI=0)
    const int total_res = args->rel15->rbSize * NR_NB_SC_PER_RB;
    for (int ant = nrOfLayers; ant < args->nb_antennas_tx; ant++) {
      if (args->start_sc + total_res <= args->symbol_sz) {
        memset(&args->txdataF[ant][txdataF_offset_per_symbol + args->start_sc], 0, total_res * sizeof(c16_t));
      } else {
        const int neg_length = args->symbol_sz - args->start_sc;
        const int pos_length = total_res - neg_length;
        memset(&args->txdataF[ant][txdataF_offset_per_symbol + args->start_sc], 0, neg_length * sizeof(c16_t));
        memset(&args->txdataF[ant][txdataF_offset_per_symbol], 0, pos_length * sizeof(c16_t));
      }
    }
  }
}

// Pre-created ISIP task set for symbol-level parallelization
static enkiTaskSet* g_symbol_mapping_task = NULL;

// ========== Async Memory Clear Overlap Support ==========
// Static buffer to store encoded output between encoding and codeword phases
// This allows PDSCH encoding to run in parallel with async memory clear
// Max size: 273 PRBs * 14 symbols * 12 SC * 8 (256QAM) * 8 layers / 8 bits = 366KB per PDSCH
// Allocate 512KB to handle multiple PDSCHs per slot with alignment padding
#define PDSCH_ENCODED_OUTPUT_MAX_SIZE (512 * 1024)
static unsigned char __attribute__((aligned(64))) g_pdsch_encoded_output[PDSCH_ENCODED_OUTPUT_MAX_SIZE];
static size_t g_pdsch_encoded_size = 0;

// ========== DMRS Precompute Buffer Pool ==========
// Static buffers to store pre-computed DMRS modulation data
// Allows DMRS computation to run in parallel with memclear and encoding
#define DMRS_MAX_PDSCH_PER_SLOT 8
#define DMRS_MAX_RES_PER_SYMBOL 1792  // 275 PRBs * 6 (type 2) + 64 alignment
#define DMRS_BUF_STRIDE DMRS_MAX_RES_PER_SYMBOL

// DMRS precompute result for one PDSCH
typedef struct {
    c16_t mod_dmrs[MAX_DMRS_SYMBOLS][DMRS_MAX_RES_PER_SYMBOL] __attribute__((aligned(64)));
    int symbol_indices[MAX_DMRS_SYMBOLS];
    int l_prime[MAX_DMRS_SYMBOLS];
    int num_precomputed;
    int valid;  // 1 if precomputed data is available
} dmrs_precompute_result_t;

// Pool of DMRS precompute results, one per PDSCH in slot
static dmrs_precompute_result_t g_dmrs_precompute[DMRS_MAX_PDSCH_PER_SLOT];
static int g_dmrs_precompute_enabled = 0;  // Set to 1 when async precompute is active

// Get pre-computed DMRS for a PDSCH (returns NULL if not available)
static inline dmrs_precompute_result_t* get_dmrs_precompute(int pdsch_idx) {
    if (g_dmrs_precompute_enabled && pdsch_idx >= 0 && pdsch_idx < DMRS_MAX_PDSCH_PER_SLOT
        && g_dmrs_precompute[pdsch_idx].valid) {
        return &g_dmrs_precompute[pdsch_idx];
    }
    return NULL;
}

// Reset DMRS precompute state (called at start of slot processing)
void nr_dlsch_dmrs_precompute_reset(void) {
    g_dmrs_precompute_enabled = 0;
    for (int i = 0; i < DMRS_MAX_PDSCH_PER_SLOT; i++) {
        g_dmrs_precompute[i].valid = 0;
    }
}

// Get DMRS buffer for async precompute (for ISIP task to fill)
c16_t* nr_dlsch_dmrs_get_buffer(int pdsch_idx, int *stride) {
    if (pdsch_idx < 0 || pdsch_idx >= DMRS_MAX_PDSCH_PER_SLOT) return NULL;
    *stride = DMRS_BUF_STRIDE;
    return (c16_t*)g_dmrs_precompute[pdsch_idx].mod_dmrs;
}

// Mark DMRS as precomputed for a PDSCH
void nr_dlsch_dmrs_set_precomputed(int pdsch_idx, int *symbol_indices, int *l_prime, int num_precomputed) {
    if (pdsch_idx < 0 || pdsch_idx >= DMRS_MAX_PDSCH_PER_SLOT) return;
    dmrs_precompute_result_t *res = &g_dmrs_precompute[pdsch_idx];
    for (int i = 0; i < num_precomputed && i < MAX_DMRS_SYMBOLS; i++) {
        res->symbol_indices[i] = symbol_indices[i];
        res->l_prime[i] = l_prime[i];
    }
    res->num_precomputed = num_precomputed;
    res->valid = 1;
}

// Enable DMRS precompute mode
void nr_dlsch_dmrs_precompute_enable(void) {
    g_dmrs_precompute_enabled = 1;
}

static void nr_pdsch_codeword_scrambling(uint8_t *in, uint32_t size, uint8_t q, uint32_t Nid, uint32_t n_RNTI, uint32_t *out)
{
  nr_codeword_scrambling(in, size, q, Nid, n_RNTI, out);
}

static int do_ptrs_symbol(nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15,
                          int start_sc,
                          int symbol_sz,
                          c16_t *txF,
                          c16_t *tx_layer,
                          int amp,
                          c16_t *mod_ptrs)
{
  int ptrs_idx = 0;
  int k = start_sc;
  c16_t *in = tx_layer;
  for (int i = 0; i < rel15->rbSize * NR_NB_SC_PER_RB; i++) {
    /* check for PTRS symbol and set flag for PTRS RE */
    bool is_ptrs_re =
        is_ptrs_subcarrier(k, rel15->rnti, rel15->PTRSFreqDensity, rel15->rbSize, rel15->PTRSReOffset, start_sc, symbol_sz);
    if (is_ptrs_re) {
      /* check if cuurent RE is PTRS RE*/
      uint16_t beta_ptrs = 1;
      txF[k] = c16mulRealShift(mod_ptrs[ptrs_idx], beta_ptrs * amp, 15);
#ifdef DEBUG_DLSCH_MAPPING
      printf("ptrs_idx %d\t \t k %d \t \t txdataF: %d %d, mod_ptrs: %d %d\n",
             ptrs_idx,
             k,
             txF[k].r,
             txF[k].i,
             mod_ptrs[ptrs_idx].r,
             mod_ptrs[ptrs_idx].i);
#endif
      ptrs_idx++;
    } else {
      txF[k] = c16mulRealShift(*in++, amp, 15);
#ifdef DEBUG_DLSCH_MAPPING
      printf("k %d \t txdataF: %d %d\n", k, txF[k].r, txF[k].i);
#endif
    }
    if (++k >= symbol_sz)
      k -= symbol_sz;
  }
  return in - tx_layer;
}

typedef union {
  uint64_t l;
  c16_t s[2];
} amp_t;

static inline int interleave_with_0_signal_first(c16_t *output, c16_t *mod_dmrs, const int16_t amp_dmrs, int sz)
{
#ifdef DEBUG_DLSCH_MAPPING
  printf("doing DMRS pattern for port 0 : d0 0 d1 0 ... dNm2 0 dNm1 0 (ul %d, rr %d)\n", upper_limit, remaining_re);
#endif
  // add filler to process all as SIMD
  c16_t *out = output;
  int i = 0;
  int end = sz / 2;
#if defined(__AVX512BW__)
  simde__m512i zeros512 = simde_mm512_setzero_si512(), amp_dmrs512 = simde_mm512_set1_epi16(amp_dmrs);
  simde__m512i perml = simde_mm512_set_epi32(23, 7, 22, 6, 21, 5, 20, 4, 19, 3, 18, 2, 17, 1, 16, 0);
  simde__m512i permh = simde_mm512_set_epi32(31, 15, 30, 14, 29, 13, 28, 12, 27, 11, 26, 10, 25, 9, 24, 8);
  for (; i < (end & ~15); i += 16) {
    simde__m512i d0 = simde_mm512_mulhrs_epi16(_mm512_loadu_si512((simde__m512i *)(mod_dmrs + i)), amp_dmrs512);
    simde_mm512_storeu_si512((simde__m512i *)out, simde_mm512_permutex2var_epi32(d0, perml, zeros512));
    out += 16;
    simde_mm512_storeu_si512((simde__m512i *)out, simde_mm512_permutex2var_epi32(d0, permh, zeros512));
    out += 16;
  }
#endif
#if defined(__AVX2__)
  simde__m256i zeros256 = simde_mm256_setzero_si256(), amp_dmrs256 = simde_mm256_set1_epi16(amp_dmrs);
  for (; i < (end & ~7); i += 8) {
    simde__m256i d0 = simde_mm256_mulhrs_epi16(simde_mm256_loadu_si256((simde__m256i *)(mod_dmrs + i)), amp_dmrs256);
    simde__m256i d2 = simde_mm256_unpacklo_epi32(d0, zeros256);
    simde__m256i d3 = simde_mm256_unpackhi_epi32(d0, zeros256);
    simde_mm256_storeu_si256((simde__m256i *)out, simde_mm256_permute2x128_si256(d2, d3, 32));
    out += 8;
    simde_mm256_storeu_si256((simde__m256i *)out, simde_mm256_permute2x128_si256(d2, d3, 49));
    out += 8;
  }
#endif
#if defined(USE128BIT)
  simde__m128i zeros = simde_mm_setzero_si128(), amp_dmrs128 = simde_mm_set1_epi16(amp_dmrs);
  for (; i < (end & ~3); i += 4) {
    simde__m128i d0 = simde_mm_mulhrs_epi16(simde_mm_loadu_si128((simde__m128i *)(mod_dmrs + i)), amp_dmrs128);
    simde__m128i d2 = simde_mm_unpacklo_epi32(d0, zeros);
    simde__m128i d3 = simde_mm_unpackhi_epi32(d0, zeros);
    simde_mm_storeu_si128((simde__m128i *)out, d2);
    out += 4;
    simde_mm_storeu_si128((simde__m128i *)out, d3);
    out += 4;
  }
#endif
  for (; i < end; i++) {
    *out++ = c16mulRealShift(mod_dmrs[i], amp_dmrs, 15);
    *out++ = (c16_t){};
  }
  return 0;
}

static inline int interleave_with_0_start_with_0(c16_t *output, c16_t *mod_dmrs, const int16_t amp_dmrs, int sz)
{
#ifdef DEBUG_DLSCH_MAPPING
  printf("doing DMRS pattern for port 2 : 0 d0 0 d1 ... 0 dNm2 0 dNm1\n");
#endif
  c16_t *out = output;
  int i = 0;
  int end = sz / 2;
#if defined(__AVX512BW__)
  simde__m512i zeros512 = simde_mm512_setzero_si512(), amp_dmrs512 = simde_mm512_set1_epi16(amp_dmrs);
  simde__m512i perml = simde_mm512_set_epi32(23, 7, 22, 6, 21, 5, 20, 4, 19, 3, 18, 2, 17, 1, 16, 0);
  simde__m512i permh = simde_mm512_set_epi32(31, 15, 30, 14, 29, 13, 28, 12, 27, 11, 26, 10, 25, 9, 24, 8);
  for (; i < (end & ~15); i += 16) {
    simde__m512i d0 = simde_mm512_mulhrs_epi16(_mm512_loadu_si512((simde__m512i *)(mod_dmrs + i)), amp_dmrs512);
    simde_mm512_storeu_si512((simde__m512i *)out, simde_mm512_permutex2var_epi32(zeros512, perml, d0));
    out += 16;
    simde_mm512_storeu_si512((simde__m512i *)out, simde_mm512_permutex2var_epi32(zeros512, permh, d0));
    out += 16;
  }
#endif
#if defined(__AVX2__)
  simde__m256i zeros256 = simde_mm256_setzero_si256(), amp_dmrs256 = simde_mm256_set1_epi16(amp_dmrs);
  for (; i < (end & ~7); i += 8) {
    simde__m256i d0 = simde_mm256_mulhrs_epi16(simde_mm256_loadu_si256((simde__m256i *)(mod_dmrs + i)), amp_dmrs256);
    simde__m256i d2 = simde_mm256_unpacklo_epi32(zeros256, d0);
    simde__m256i d3 = simde_mm256_unpackhi_epi32(zeros256, d0);
    simde_mm256_storeu_si256((simde__m256i *)out, simde_mm256_permute2x128_si256(d2, d3, 32));
    out += 8;
    simde_mm256_storeu_si256((simde__m256i *)out, simde_mm256_permute2x128_si256(d2, d3, 49));
    out += 8;
  }
#endif
#if defined(USE128BIT)
  simde__m128i zeros = simde_mm_setzero_si128(), amp_dmrs128 = simde_mm_set1_epi16(amp_dmrs);
  for (; i < (end & ~3); i += 4) {
    simde__m128i d0 = simde_mm_mulhrs_epi16(simde_mm_loadu_si128((simde__m128i *)(mod_dmrs + i)), amp_dmrs128);
    simde__m128i d2 = simde_mm_unpacklo_epi32(zeros, d0);
    simde__m128i d3 = simde_mm_unpackhi_epi32(zeros, d0);
    simde_mm_storeu_si128((simde__m128i *)out, d2);
    out += 4;
    simde_mm_storeu_si128((simde__m128i *)out, d3);
    out += 4;
  }
#endif
  for (; i < end; i++) {
    *out++ = (c16_t){};
    *out++ = c16mulRealShift(mod_dmrs[i], amp_dmrs, 15);
  }
  return 0;
}

static inline int interleave_signals(c16_t *output, c16_t *signal1, const int amp, c16_t *signal2, const int amp2, int sz)
{
#ifdef DEBUG_DLSCH_MAPPING
  printf("doing DMRS pattern for port 0 : d0 X0 d1 X1 ... dNm2 XNm2 dNm1 XNm1\n");
#endif
    // add filler to process all as SIMD
  c16_t *out = output;
  int i = 0;
  int end = sz / 2;
#if defined(__AVX512BW__)
  simde__m512i amp2512 = simde_mm512_set1_epi16(amp2), amp512 = simde_mm512_set1_epi16(amp);
  simde__m512i perml = simde_mm512_set_epi32(23, 7, 22, 6, 21, 5, 20, 4, 19, 3, 18, 2, 17, 1, 16, 0);
  simde__m512i permh = simde_mm512_set_epi32(31, 15, 30, 14, 29, 13, 28, 12, 27, 11, 26, 10, 25, 9, 24, 8);
  for (; i < (end & ~15); i += 16) {
    simde__m512i d0 = simde_mm512_mulhrs_epi16(_mm512_loadu_si512((simde__m512i *)(signal2 + i)), amp2512);
    simde__m512i d1 = simde_mm512_mulhrs_epi16(_mm512_loadu_si512((simde__m512i *)(signal1 + i)), amp512);
    simde_mm512_storeu_si512((simde__m512i *)out, simde_mm512_permutex2var_epi32(d0, perml, d1));
    out += 16;
    simde_mm512_storeu_si512((simde__m512i *)out, simde_mm512_permutex2var_epi32(d0, permh, d1));
    out += 16;
  }
#endif
#if defined(__AVX2__)
  simde__m256i amp2256 = simde_mm256_set1_epi16(amp2), amp256 = simde_mm256_set1_epi16(amp);
  for (; i < (end & ~7); i += 8) {
    simde__m256i d0 = simde_mm256_mulhrs_epi16(simde_mm256_loadu_si256((simde__m256i *)(signal2 + i)), amp2256);
    simde__m256i d1 = simde_mm256_mulhrs_epi16(simde_mm256_loadu_si256((simde__m256i *)(signal1 + i)), amp256);
    simde__m256i d2 = simde_mm256_unpacklo_epi32(d0, d1);
    simde__m256i d3 = simde_mm256_unpackhi_epi32(d0, d1);
    simde_mm256_storeu_si256((simde__m256i *)out, simde_mm256_permute2x128_si256(d2, d3, 32));
    out += 8;
    simde_mm256_storeu_si256((simde__m256i *)out, simde_mm256_permute2x128_si256(d2, d3, 49));
    out += 8;
  }
#endif
#if defined(USE128BIT)
  simde__m128i amp2128 = simde_mm_set1_epi16(amp2), amp128 = simde_mm_set1_epi16(amp);
  for (; i < (end & ~3); i += 4) {
    simde__m128i d0 = simde_mm_mulhrs_epi16(simde_mm_loadu_si128((simde__m128i *)(signal2 + i)), amp2128);
    simde__m128i d1 = simde_mm_mulhrs_epi16(simde_mm_loadu_si128((simde__m128i *)(signal1 + i)), amp128);
    simde__m128i d2 = simde_mm_unpacklo_epi32(d0, d1);
    simde__m128i d3 = simde_mm_unpackhi_epi32(d0, d1);
    simde_mm_storeu_si128((simde__m128i *)out, d2);
    out += 4;
    simde_mm_storeu_si128((simde__m128i *)out, d3);
    out += 4;
  }
#endif
  for (; i < end; i++) {
    *out++ = c16mulRealShift(signal2[i], amp2, 15);
    *out++ = c16mulRealShift(signal1[i], amp, 15);
  }
  return sz / 2;
}

static inline int dmrs_case00(c16_t *output,
                              c16_t *txl,
                              c16_t *mod_dmrs,
                              const int16_t amp_dmrs,
                              const int amp,
                              int sz,
                              int start_sc,
                              int remaining_re,
                              int dmrs_port,
                              const int dmrs_Type,
                              int symbol_sz,
                              int l_prime,
                              uint8_t numDmrsCdmGrpsNoData)
{
  // DMRS params for this dmrs port
  int Wt[2], Wf[2];
  get_Wt(Wt, dmrs_port, dmrs_Type);
  get_Wf(Wf, dmrs_port, dmrs_Type);
  const int8_t delta = get_delta(dmrs_port, dmrs_Type);
  int dmrs_idx = 0;
  int k = start_sc;
  c16_t *in = txl;
  uint8_t k_prime = 0;
  uint16_t n = 0;
  for (int i = 0; i < sz; i++) {
    if (k == ((start_sc + get_dmrs_freq_idx(n, k_prime, delta, dmrs_Type)) % (symbol_sz))) {
      output[k] = c16mulRealShift(mod_dmrs[dmrs_idx], Wt[l_prime] * Wf[k_prime] * amp_dmrs, 15);
      dmrs_idx++;
      k_prime = (k_prime + 1) & 1;
      n += (k_prime ? 0 : 1);
    }
    /* Map PTRS Symbol */
    /* Map DATA Symbol */
    else if (allowed_xlsch_re_in_dmrs_symbol(k, start_sc, symbol_sz, numDmrsCdmGrpsNoData, dmrs_Type)) {
      output[k] = c16mulRealShift(*in++, amp, 15);
    }
    /* mute RE */
    else {
      output[k] = (c16_t){0};
    }
    k = (k + 1) % symbol_sz;
  } // RE loop
  return in - txl;
}

static inline int no_ptrs_dmrs_case(c16_t *output, c16_t *txl, const int amp, const int sz)
{
  // Loop Over SCs:
  int i = 0;
#if defined(__AVX512BW__)
  simde__m512i amp512 = simde_mm512_set1_epi16(amp);
  for (; i < (sz & ~15); i += 16) {
    const simde__m512i txL = simde_mm512_loadu_si512((simde__m512i *)(txl + i));
    simde_mm512_storeu_si512((simde__m512i *)(output + i), simde_mm512_mulhrs_epi16(amp512, txL));
  }
#endif
#if defined(__AVX2__)
  simde__m256i amp256 = simde_mm256_set1_epi16(amp);
  for (; i < (sz & ~7); i += 8) {
    const simde__m256i txL = simde_mm256_loadu_si256((simde__m256i *)(txl + i));
    simde_mm256_storeu_si256((simde__m256i *)(output + i), _mm256_mulhrs_epi16(amp256, txL));
  }
#endif
#if defined(USE128BIT)
  simde__m128i amp128 = simde_mm_set1_epi16(amp);
  for (; i < (sz & ~3); i += 4) {
    const simde__m128i txL = simde_mm_loadu_si128((simde__m128i *)(txl + i));
    simde_mm_storeu_si128((simde__m128i *)(output + i), simde_mm_mulhrs_epi16(amp128, txL));
  }
#endif
  for (; i < sz; i++) {
    output[i] = c16mulRealShift(txl[i], amp, 15);
  }
  return sz;
}

static inline void neg_dmrs(c16_t *in, c16_t *out, int sz)
{
  for (int i = 0; i < sz; i++)
    *out++ = i % 2 ? (c16_t){-in[i].r, -in[i].i} : in[i];
}

static inline int do_onelayer(NR_DL_FRAME_PARMS *frame_parms,
                              int slot,
                              nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15,
                              int layer,
                              c16_t *output,
                              c16_t *txl_start,
                              int start_sc,
                              int symbol_sz,
                              int l_symbol,
                              uint16_t dlPtrsSymPos,
                              int n_ptrs,
                              int amp,
                              int16_t amp_dmrs,
                              int l_prime,
                              nfapi_nr_dmrs_type_e dmrs_Type,
                              c16_t *dmrs_start)
{
  c16_t *txl = txl_start;
  const uint sz = rel15->rbSize * NR_NB_SC_PER_RB;
  int upper_limit = sz;
  int remaining_re = 0;
  if (start_sc + upper_limit > symbol_sz) {
    upper_limit = symbol_sz - start_sc;
    remaining_re = sz - upper_limit;
  }

  /* calculate if current symbol is PTRS symbols */
  int ptrs_symbol = 0;
  if (rel15->pduBitmap & 0x1) {
    ptrs_symbol = is_ptrs_symbol(l_symbol, dlPtrsSymPos);
  }

  if (ptrs_symbol) {
    /* PTRS QPSK Modulation for each OFDM symbol in a slot */
    LOG_D(PHY, "Doing ptrs modulation for symbol %d, n_ptrs %d\n", l_symbol, n_ptrs);
    c16_t mod_ptrs[max(n_ptrs, 1)]
        __attribute__((aligned(64))); // max only to please sanitizer, that kills if 0 even if it is not a error
    const uint32_t *gold =
        nr_gold_pdsch(frame_parms->N_RB_DL, frame_parms->symbols_per_slot, rel15->dlDmrsScramblingId, rel15->SCID, slot, l_symbol);
    nr_modulation(gold, n_ptrs * DMRS_MOD_ORDER, DMRS_MOD_ORDER, (int16_t *)mod_ptrs);
    txl += do_ptrs_symbol(rel15, start_sc, symbol_sz, output, txl, amp, mod_ptrs);

  } else if (rel15->dlDmrsSymbPos & (1 << l_symbol)) {
    /* Map DMRS Symbol */
    int dmrs_port = get_dmrs_port(layer, rel15->dmrsPorts);
    if (l_prime == 0 && dmrs_Type == NFAPI_NR_DMRS_TYPE1) {
      if (rel15->numDmrsCdmGrpsNoData == 2) {
        switch (dmrs_port & 3) {
          case 0:
            txl += interleave_with_0_signal_first(output + start_sc, dmrs_start, amp_dmrs, upper_limit);
            txl += interleave_with_0_signal_first(output, dmrs_start + upper_limit / 2, amp_dmrs, remaining_re);
            break;
          case 1: {
            c16_t dmrs[sz / 2];
            neg_dmrs(dmrs_start, dmrs, sz / 2);
            txl += interleave_with_0_signal_first(output + start_sc, dmrs, amp_dmrs, upper_limit);
            txl += interleave_with_0_signal_first(output, dmrs + upper_limit / 2, amp_dmrs, remaining_re);
          } break;
          case 2:
            txl += interleave_with_0_start_with_0(output + start_sc, dmrs_start, amp_dmrs, upper_limit);
            txl += interleave_with_0_start_with_0(output, dmrs_start + upper_limit / 2, amp_dmrs, remaining_re);
            break;
          case 3: {
            c16_t dmrs[sz / 2];
            neg_dmrs(dmrs_start, dmrs, sz / 2);
            txl += interleave_with_0_start_with_0(output + start_sc, dmrs, amp_dmrs, upper_limit);
            txl += interleave_with_0_start_with_0(output, dmrs + upper_limit / 2, amp_dmrs, remaining_re);
          } break;
        }
      } else if (rel15->numDmrsCdmGrpsNoData == 1) {
        switch (dmrs_port & 3) {
          case 0:
            txl += interleave_signals(output + start_sc, txl, amp, dmrs_start, amp_dmrs, upper_limit);
            txl += interleave_signals(output, txl, amp, dmrs_start + upper_limit / 2, amp_dmrs, remaining_re);
            break;
          case 1: {
            c16_t dmrs[sz / 2];
            neg_dmrs(dmrs_start, dmrs, sz / 2);
            txl += interleave_signals(output + start_sc, txl, amp, dmrs, amp_dmrs, upper_limit);
            txl += interleave_signals(output, txl, amp, dmrs + upper_limit / 2, amp_dmrs, remaining_re);
          } break;
          case 2:
            txl += interleave_signals(output + start_sc, dmrs_start, amp_dmrs, txl, amp, upper_limit);
            txl += interleave_signals(output, dmrs_start + upper_limit / 2, amp_dmrs, txl, amp, remaining_re);
            break;
          case 3: {
            c16_t dmrs[sz / 2];
            neg_dmrs(dmrs_start, dmrs, sz / 2);
            txl += interleave_signals(output + start_sc, dmrs, amp_dmrs, txl, amp, upper_limit);
            txl += interleave_signals(output, dmrs + upper_limit / 2, amp_dmrs, txl, amp, remaining_re);
          } break;
        }
      } else
        AssertFatal(false, "rel15->numDmrsCdmGrpsNoData is %d\n", rel15->numDmrsCdmGrpsNoData);
    } else {
      txl += dmrs_case00(output,
                         txl,
                         dmrs_start,
                         amp_dmrs,
                         amp,
                         sz,
                         start_sc,
                         remaining_re,
                         dmrs_port,
                         dmrs_Type,
                         symbol_sz,
                         l_prime,
                         rel15->numDmrsCdmGrpsNoData);
    } // generic DMRS case
  } else { // no PTRS or DMRS in this symbol
    txl += no_ptrs_dmrs_case(output + start_sc, txl, amp, upper_limit);
    txl += no_ptrs_dmrs_case(output, txl, amp, remaining_re);
  } // no DMRS/PTRS in symbol
  return txl - txl_start;
}

// ========== Helper: Check if all PRGs use PMI=0 (identity matrix) ==========
// PMI=0 means no precoding needed: antenna[i] = layer[i] directly
static inline bool check_all_pmi_zero(nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15)
{
  nfapi_nr_tx_precoding_and_beamforming_t *pb = &rel15->precodingAndBeamforming;

  // prg_size==0 means no PRG grouping, implicitly all PMI=0
  if (pb->prg_size == 0)
    return true;

  // Check each PRG for non-zero PMI
  const int num_prgs = (rel15->rbSize + pb->prg_size - 1) / pb->prg_size;
  for (int i = 0; i < num_prgs; i++) {
    if (pb->prgs_list[i].pm_idx != 0)
      return false;
  }
  return true;
}

// ========== do_txdataF: Apply precoding matrix to map layers to antennas ==========
// This function is only called when direct mapping (PMI=0) is NOT used.
// For PMI=0 cases, the caller handles direct layer-to-antenna mapping.
static inline void do_txdataF(c16_t **txdataF,
                              int symbol_sz,
                              c16_t txdataF_precoding[][symbol_sz],
                              PHY_VARS_gNB *gNB,
                              nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15,
                              int ant,
                              int start_sc,
                              int txdataF_offset_per_symbol)
{
  NR_DL_FRAME_PARMS *frame_parms = &gNB->frame_parms;
  nfapi_nr_tx_precoding_and_beamforming_t *pb = &rel15->precodingAndBeamforming;

  // ========== RB-by-RB Precoding (PMI may vary per PRG) ==========
  int rb = 0;
  uint16_t subCarrier = start_sc;
  while (rb < rel15->rbSize) {
    // get pmi info
    const int pmi = (pb->prg_size > 0) ? (pb->prgs_list[(int)rb / pb->prg_size].pm_idx) : 0;
    const int pmi2 = (rb < (rel15->rbSize - 1) && pb->prg_size > 0) ? (pb->prgs_list[(int)(rb + 1) / pb->prg_size].pm_idx) : -1;

    // If pmi of next RB and pmi of current RB are the same, we do 2 RB in a row
    // if pmi differs, or current rb is the end (rel15->rbSize - 1), than we do 1 RB in a row
    const int rb_step = pmi == pmi2 ? 2 : 1;
    const int re_cnt = NR_NB_SC_PER_RB * rb_step;

    if (pmi == 0) { // unitary Precoding
      if (subCarrier + re_cnt <= symbol_sz) { // RB does not cross DC
        if (ant < rel15->nrOfLayers)
          memcpy(&txdataF[ant][txdataF_offset_per_symbol + subCarrier],
                 &txdataF_precoding[ant][subCarrier],
                 re_cnt * sizeof(**txdataF));
        else
          memset(&txdataF[ant][txdataF_offset_per_symbol + subCarrier], 0, re_cnt * sizeof(**txdataF));
      } else { // RB does cross DC
        const int neg_length = symbol_sz - subCarrier;
        const int pos_length = re_cnt - neg_length;
        if (ant < rel15->nrOfLayers) {
          memcpy(&txdataF[ant][txdataF_offset_per_symbol + subCarrier],
                 &txdataF_precoding[ant][subCarrier],
                 neg_length * sizeof(**txdataF));
          memcpy(&txdataF[ant][txdataF_offset_per_symbol], &txdataF_precoding[ant], pos_length * sizeof(**txdataF));
        } else {
          memset(&txdataF[ant][txdataF_offset_per_symbol + subCarrier], 0, neg_length * sizeof(**txdataF));
          memset(&txdataF[ant][txdataF_offset_per_symbol], 0, pos_length * sizeof(**txdataF));
        }
      }
      subCarrier += re_cnt;
      if (subCarrier >= symbol_sz) {
        subCarrier -= symbol_sz;
      }
    } else { // non-unitary Precoding
      AssertFatal(frame_parms->nb_antennas_tx > 1, "No precoding can be done with a single antenna port\n");
      // get the precoding matrix weights:
      nfapi_nr_pm_pdu_t *pmi_pdu = &gNB->gNB_config.pmi_list.pmi_pdu[pmi - 1]; // pmi 0 is identity matrix
      AssertFatal(pmi == pmi_pdu->pm_idx, "PMI %d doesn't match to the one in precoding matrix %d\n", pmi, pmi_pdu->pm_idx);
      AssertFatal(ant < pmi_pdu->num_ant_ports,
                  "Antenna port index %d exceeds precoding matrix AP size %d\n",
                  ant,
                  pmi_pdu->num_ant_ports);
      AssertFatal(rel15->nrOfLayers == pmi_pdu->numLayers,
                  "Number of layers %d doesn't match to the one in precoding matrix %d\n",
                  rel15->nrOfLayers,
                  pmi_pdu->numLayers);
      if ((subCarrier + re_cnt) < symbol_sz) { // within ofdm_symbol_size, use SIMDe
        nr_layer_precoder_simd(rel15->nrOfLayers,
                               symbol_sz,
                               txdataF_precoding,
                               ant,
                               pmi_pdu,
                               subCarrier,
                               re_cnt,
                               &txdataF[ant][txdataF_offset_per_symbol]);
        subCarrier += re_cnt;
      } else { // crossing ofdm_symbol_size, use simple arithmetic operations
        for (int i = 0; i < re_cnt; i++) {
          txdataF[ant][txdataF_offset_per_symbol + subCarrier] =
              nr_layer_precoder_cm(rel15->nrOfLayers, symbol_sz, txdataF_precoding, ant, pmi_pdu, subCarrier);
#ifdef DEBUG_DLSCH_MAPPING
          printf("antenna %d\t l %d \t subCarrier %d \t txdataF: %d %d\n",
                 ant,
                 l_symbol,
                 subCarrier,
                 txdataF[ant][l_symbol * symbol_sz + subCarrier + txdataF_offset].r,
                 txdataF[ant][l_symbol * symbol_sz + subCarrier + txdataF_offset].i);
#endif
          if (++subCarrier >= symbol_sz) {
            subCarrier -= symbol_sz;
          }
        }
      } // else{ // crossing ofdm_symbol_size, use simple arithmetic operations
    } // else { // non-unitary Precoding

    rb += rb_step;
  } // RB loop: while(rb < rel15->rbSize)
}

// Use shared slot timing from nr_slot_timing.h (slot_timing_enabled, current_slot_timing)

static inline long timespec_diff_ns_local(struct timespec *start, struct timespec *end) {
  return (end->tv_sec - start->tv_sec) * 1000000000L + (end->tv_nsec - start->tv_nsec);
}

static int do_one_dlsch(unsigned char *input_ptr, PHY_VARS_gNB *gNB, NR_gNB_DLSCH_t *dlsch, int slot, int pdsch_idx)
{
  const int16_t amp = gNB->TX_AMP;
  NR_DL_FRAME_PARMS *frame_parms = &gNB->frame_parms;

  time_stats_t *dlsch_scrambling_stats = &gNB->dlsch_scrambling_stats;
  time_stats_t *dlsch_modulation_stats = &gNB->dlsch_modulation_stats;

  // Timing measurements
  struct timespec t_start, t_end;
  NR_DL_gNB_HARQ_t *harq = &dlsch->harq_process;
  nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15 = &harq->pdsch_pdu.pdsch_pdu_rel15;
  const int layerSz = frame_parms->N_RB_DL * NR_SYMBOLS_PER_SLOT * NR_NB_SC_PER_RB;
  const int symbol_sz=frame_parms->ofdm_symbol_size;
  const int dmrs_Type = rel15->dmrsConfigType;
  const int nb_re_dmrs = rel15->numDmrsCdmGrpsNoData * (rel15->dmrsConfigType == NFAPI_NR_DMRS_TYPE1 ? 6 : 4);
  const int16_t amp_dmrs = min((double)amp * sqrt(rel15->numDmrsCdmGrpsNoData), INT16_MAX); // 3GPP TS 38.214 Section 4.1: Table 4.1-1
  LOG_D(PHY,
        "pdsch: BWPStart %d, BWPSize %d, rbStart %d, rbsize %d\n",
        rel15->BWPStart,
        rel15->BWPSize,
        rel15->rbStart,
        rel15->rbSize);

  // Accumulate RB count and record MCS for this PDSCH (use shared slot timing)
  if (slot_timing_enabled) {
    current_slot_timing.num_rbs += rel15->rbSize;
    // Record the first PDSCH MCS (or highest if multiple PDSCHs)
    if (current_slot_timing.mcs == -1)
      current_slot_timing.mcs = rel15->mcsIndex[0];
    else if (rel15->mcsIndex[0] > current_slot_timing.mcs)
      current_slot_timing.mcs = rel15->mcsIndex[0];  // Record highest MCS
    current_slot_timing.num_layers = rel15->nrOfLayers;
  }

  const int n_dmrs = (rel15->BWPStart + rel15->rbStart + rel15->rbSize) * nb_re_dmrs;

  const int dmrs_symbol_map = rel15->dlDmrsSymbPos; // single DMRS: 010000100 Double DMRS 110001100
  const int xOverhead = 0;
  const int nb_re =
      (12 * rel15->NrOfSymbols - nb_re_dmrs * get_num_dmrs(rel15->dlDmrsSymbPos) - xOverhead) * rel15->rbSize * rel15->nrOfLayers;
  const int Qm = rel15->qamModOrder[0];
  const int encoded_length = nb_re * Qm;

  /* PTRS */
  uint16_t dlPtrsSymPos = 0;
  int n_ptrs = 0;
  uint32_t ptrsSymbPerSlot = 0;
  if (rel15->pduBitmap & 0x1) {
    set_ptrs_symb_idx(&dlPtrsSymPos,
                      rel15->NrOfSymbols,
                      rel15->StartSymbolIndex,
                      1 << rel15->PTRSTimeDensity,
                      rel15->dlDmrsSymbPos);
    n_ptrs = (rel15->rbSize + rel15->PTRSFreqDensity - 1) / rel15->PTRSFreqDensity;
    ptrsSymbPerSlot = get_ptrs_symbols_in_slot(dlPtrsSymPos, rel15->StartSymbolIndex, rel15->NrOfSymbols);
  }
  harq->unav_res = ptrsSymbPerSlot * n_ptrs;

#ifdef DEBUG_DLSCH
  printf("PDSCH encoding:\nPayload:\n");
  for (int i = 0; i < (harq->B >> 3); i += 16) {
    for (int j = 0; j < 16; j++)
      printf("0x%02x\t", harq->pdu[i + j]);
    printf("\n");
  }
  printf("\nEncoded payload:\n");
  for (int i = 0; i < encoded_length; i += 8) {
    for (int j = 0; j < 8; j++)
      printf("%d", (input_ptr[i >> 3] >> j) & 1);
    printf("\t");
  }
  printf("\n");
#endif

  if (IS_SOFTMODEM_DLSIM)
    memcpy(harq->f, input_ptr, (encoded_length + 7) >> 3);

  /// Resource mapping
  // Non interleaved VRB to PRB mapping
  uint16_t start_sc = frame_parms->first_carrier_offset + (rel15->rbStart + rel15->BWPStart) * NR_NB_SC_PER_RB;
  if (start_sc >= symbol_sz)
    start_sc -= symbol_sz;

  const uint32_t txdataF_offset = slot * frame_parms->samples_per_slot_wCP;
#ifdef DEBUG_DLSCH_MAPPING
  printf("PDSCH resource mapping started (start SC %d\tstart symbol %d\tN_PRB %d\tnb_re %d,nb_layers %d)\n",
         start_sc,
         rel15->StartSymbolIndex,
         rel15->rbSize,
         nb_re,
         rel15->nrOfLayers);
#endif

  AssertFatal(n_dmrs, "n_dmrs can't be 0\n");
  // DMRS pre-computation: compute all DMRS modulations before the symbol loop
  // This removes DMRS computation from the critical path and improves cache locality

  // Check if DMRS was pre-computed asynchronously
  dmrs_precompute_result_t *dmrs_precomp = get_dmrs_precompute(pdsch_idx);

  // Local DMRS buffers (used when async precompute is not available)
  // Use fixed stride DMRS_MAX_RES_PER_SYMBOL to match async precompute buffer layout
  c16_t mod_dmrs_local[MAX_DMRS_SYMBOLS][DMRS_MAX_RES_PER_SYMBOL] __attribute__((aligned(64)));
  int dmrs_symbol_indices_local[MAX_DMRS_SYMBOLS];
  int dmrs_l_prime_local[MAX_DMRS_SYMBOLS];
  int num_precomputed_local = 0;

  // Pointers to DMRS data (either precomputed or local)
  c16_t (*mod_dmrs_precomputed)[DMRS_MAX_RES_PER_SYMBOL];
  int *dmrs_symbol_indices;
  int *dmrs_l_prime;
  int num_precomputed;

  if (dmrs_precomp) {
    // Use async-precomputed DMRS
    mod_dmrs_precomputed = dmrs_precomp->mod_dmrs;
    dmrs_symbol_indices = dmrs_precomp->symbol_indices;
    dmrs_l_prime = dmrs_precomp->l_prime;
    num_precomputed = dmrs_precomp->num_precomputed;
    LOG_D(PHY, "Using async precomputed DMRS for PDSCH %d (%d symbols)\n", pdsch_idx, num_precomputed);
  } else {
    // Compute DMRS inline (fallback path)
    mod_dmrs_precomputed = mod_dmrs_local;  // Same stride, no cast needed
    dmrs_symbol_indices = dmrs_symbol_indices_local;
    dmrs_l_prime = dmrs_l_prime_local;

    // Pre-compute DMRS for all DMRS symbols
    int l_overline = get_l0(dmrs_symbol_map);
    for (int l_symbol = rel15->StartSymbolIndex;
         l_symbol < rel15->StartSymbolIndex + rel15->NrOfSymbols && num_precomputed_local < MAX_DMRS_SYMBOLS;
         l_symbol++) {
      if (dmrs_symbol_map & (1 << l_symbol)) {
        // Compute l_prime for this symbol
        int l_prime = 0;
        if (l_symbol == (l_overline + 1)) {
          l_prime = 1;
        } else if (l_symbol > (l_overline + 1)) {
          l_overline = l_symbol;
          l_prime = 0;
        }

        // Get gold sequence and modulate
        const uint32_t *gold = nr_gold_pdsch(frame_parms->N_RB_DL,
                                             frame_parms->symbols_per_slot,
                                             rel15->dlDmrsScramblingId,
                                             rel15->SCID,
                                             slot,
                                             l_symbol);
        nr_modulation(gold, n_dmrs * DMRS_MOD_ORDER, DMRS_MOD_ORDER,
                      (int16_t *)mod_dmrs_local[num_precomputed_local]);

        dmrs_symbol_indices_local[num_precomputed_local] = l_symbol;
        dmrs_l_prime_local[num_precomputed_local] = l_prime;
        num_precomputed_local++;
      }
    }
    num_precomputed = num_precomputed_local;
  }

  // Pointer to current DMRS buffer (updated in symbol loop)
  c16_t *mod_dmrs = mod_dmrs_precomputed[0];
  int current_dmrs_idx = 0;
  unsigned int re_beginning_of_symbol = 0;

  // Allocate layer buffers
  int layerSz2 = (layerSz + 63) & ~63;
  c16_t tx_layers[rel15->nrOfLayers][layerSz2] __attribute__((aligned(64)));

  // ========== Fused Modulation + Layer Mapping for 256QAM ==========
  // For 256QAM, use fused modulation + layer mapping to eliminate mod_symbs buffer (~98KB).
  // Keep AVX512 scrambling separate (it's memory-bandwidth bound and very fast).
  const bool use_fused_mod_layer = (Qm == 8);

  if (use_fused_mod_layer) {
    // 256QAM path: AVX512 scrambling → fused modulation + layer mapping
    // Eliminates mod_symbs intermediate buffer

    // Scrambling (AVX512 optimized)
    // Note: No memset needed - nr_codeword_scrambling fully overwrites the output buffer
    // and we only read n_symbols bytes (encoded_length / 8) in modulation
    start_meas(dlsch_scrambling_stats);
    if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
    uint32_t scrambled_output[(encoded_length >> 5) + 4] __attribute__((aligned(64)));
    nr_pdsch_codeword_scrambling(input_ptr, encoded_length, 0, rel15->dataScramblingId, rel15->rnti, scrambled_output);
    if (slot_timing_enabled) {
      clock_gettime(CLOCK_MONOTONIC, &t_end);
      current_slot_timing.pdsch_scrambling_ns += timespec_diff_ns_local(&t_start, &t_end);
    }
    stop_meas(dlsch_scrambling_stats);

    // Fused modulation + layer mapping (parallel version using ISIP)
    start_meas(dlsch_modulation_stats);
    if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
    const uint32_t n_symbols = encoded_length / 8;  // 256QAM: 8 bits per symbol
    nr_modulate_layer_map_256qam_parallel((const uint8_t *)scrambled_output,
                                           n_symbols,
                                           rel15->nrOfLayers,
                                           layerSz2,
                                           tx_layers);
    if (slot_timing_enabled) {
      clock_gettime(CLOCK_MONOTONIC, &t_end);
      // Record in modulation timing (layer mapping is fused)
      current_slot_timing.pdsch_modulation_ns += timespec_diff_ns_local(&t_start, &t_end);
      current_slot_timing.pdsch_layer_mapping_ns += 0;  // Included in modulation
    }
    stop_meas(dlsch_modulation_stats);

#ifdef DEBUG_DLSCH
    printf("PDSCH Fused Mod+Layer 256QAM: Qm %d, n_symbols %d, n_layers %d\n", Qm, n_symbols, rel15->nrOfLayers);
    for (int l = 0; l < rel15->nrOfLayers; l++) {
      printf("Layer %d:\n", l);
      for (int i = 0; i < 8 && i < n_symbols / rel15->nrOfLayers; i++) {
        printf("%d %d\t", tx_layers[l][i].r, tx_layers[l][i].i);
      }
      printf("\n");
    }
#endif

  } else {
    // Original path for QPSK/16QAM/64QAM: separate scrambling, modulation, layer mapping
    c16_t mod_symbs[rel15->NrOfCodewords][encoded_length] __attribute__((aligned(64)));

    for (int codeWord = 0; codeWord < rel15->NrOfCodewords; codeWord++) {
      /// scrambling
      // Note: No memset needed - nr_codeword_scrambling fully overwrites the required bits
      start_meas(dlsch_scrambling_stats);
      if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
      uint32_t scrambled_output[(encoded_length >> 5) + 4] __attribute__((aligned(64)));
      nr_pdsch_codeword_scrambling(input_ptr, encoded_length, codeWord, rel15->dataScramblingId, rel15->rnti, scrambled_output);

#ifdef DEBUG_DLSCH
      printf("PDSCH scrambling:\n");
      for (int i = 0; i < encoded_length >> 8; i++) {
        for (int j = 0; j < 8; j++)
          printf("0x%08x\t", scrambled_output[(i << 3) + j]);
        printf("\n");
      }
#endif

      if (slot_timing_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        current_slot_timing.pdsch_scrambling_ns += timespec_diff_ns_local(&t_start, &t_end);
      }
      stop_meas(dlsch_scrambling_stats);

      /// Modulation
      start_meas(dlsch_modulation_stats);
      if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
      nr_modulation(scrambled_output, encoded_length, Qm, (int16_t *)mod_symbs[codeWord]);
      VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_gNB_PDSCH_MODULATION, 0);
      if (slot_timing_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        current_slot_timing.pdsch_modulation_ns += timespec_diff_ns_local(&t_start, &t_end);
      }
      stop_meas(dlsch_modulation_stats);
#ifdef DEBUG_DLSCH
      printf("PDSCH Modulation: Qm %d(%d)\n", Qm, nb_re);
      for (int i = 0; i < nb_re; i += 8) {
        for (int j = 0; j < 8; j++) {
          printf("%d %d\t", mod_symbs[codeWord][i + j].r, mod_symbs[codeWord][i + j].i);
        }
        printf("\n");
      }
#endif
    }

    // Layer mapping (parallel version using ISIP symbol-range splitting)
    if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
    nr_layer_mapping_parallel(rel15->NrOfCodewords, encoded_length, mod_symbs, rel15->nrOfLayers, layerSz2, nb_re, tx_layers);
    if (slot_timing_enabled) {
      clock_gettime(CLOCK_MONOTONIC, &t_end);
      current_slot_timing.pdsch_layer_mapping_ns += timespec_diff_ns_local(&t_start, &t_end);
    }
  }

  /// Layer Precoding and Antenna port mapping
  // tx_layers 1-8 are mapped on antenna ports 1000-1007
  // The precoding info is supported by nfapi such as num_prgs, prg_size, prgs_list and pm_idx
  // The same precoding matrix is applied on prg_size RBs, Thus
  //        pmi = prgs_list[rbidx/prg_size].pm_idx, rbidx =0,...,rbSize-1
  // The Precoding matrix:
  // The Codebook Type I
  start_meas(&gNB->dlsch_resource_mapping_stats);
  start_meas(&gNB->dlsch_precoding_stats);
  nfapi_nr_tx_precoding_and_beamforming_t *pb = &rel15->precodingAndBeamforming;
  // beam number in multi-beam scenario (concurrent beams)
  int bitmap = SL_to_bitmap(rel15->StartSymbolIndex, rel15->NrOfSymbols);
  int beam_nb = beam_index_allocation(gNB->enable_analog_das,
                                      pb->prgs_list[0].dig_bf_interface_list[0].beam_idx,
                                      &gNB->gNB_config.analog_beamforming_ve,
                                      &gNB->common_vars,
                                      slot,
                                      frame_parms->symbols_per_slot,
                                      bitmap);

  c16_t **txdataF = gNB->common_vars.txdataF[beam_nb];

  // Timing accumulators for RE mapping and precoding (measured per symbol)
  long total_mapping_ns = 0;
  long total_precoding_ns = 0;

  // ========== Symbol-Level Parallelization (PMI=0 Fast Path) ==========
  // When all PRGs use PMI=0, symbols can be processed in parallel since:
  // 1. Each symbol writes to different txdataF offsets (no conflict)
  // 2. Each symbol reads from different tx_layers offsets (pre-computed)
  // 3. DMRS modulation is pre-computed for all symbols
  const bool use_direct_mapping_global = check_all_pmi_zero(rel15);
  void* scheduler = isip_pool_get_scheduler();

  // Only use symbol parallelization when:
  // - PMI=0 (direct mapping, no precoding buffer)
  // - ISIP available
  // - Multiple symbols to process (>= 2 for parallel benefit)
  if (use_direct_mapping_global && scheduler && rel15->NrOfSymbols >= 2) {
    if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);

    // Pre-compute RE offsets for all symbols
    int symbol_re_offset[MAX_SYMBOLS_PER_SLOT];
    c16_t* symbol_dmrs_ptr[MAX_SYMBOLS_PER_SLOT];
    int symbol_l_prime[MAX_SYMBOLS_PER_SLOT];
    int cumulative_re = 0;
    int dmrs_precompute_idx = 0;

    for (int s = 0; s < rel15->NrOfSymbols; s++) {
      int l_sym = rel15->StartSymbolIndex + s;
      symbol_re_offset[s] = cumulative_re;
      symbol_l_prime[s] = 0;
      symbol_dmrs_ptr[s] = NULL;

      // Check if this is a DMRS symbol
      if (dmrs_symbol_map & (1 << l_sym)) {
        // Find matching pre-computed DMRS
        if (dmrs_precompute_idx < num_precomputed && dmrs_symbol_indices[dmrs_precompute_idx] == l_sym) {
          symbol_dmrs_ptr[s] = mod_dmrs_precomputed[dmrs_precompute_idx];
          symbol_l_prime[s] = dmrs_l_prime[dmrs_precompute_idx];
          dmrs_precompute_idx++;
        }
      }

      // Accumulate RE offset for next symbol
      cumulative_re += precalc_layer_sz(rel15, l_sym, dmrs_symbol_map, dlPtrsSymPos, dmrs_Type);
    }

    // Create task set on first use
    if (!g_symbol_mapping_task) {
      g_symbol_mapping_task = enkiCreateTaskSet(scheduler, re_mapping_symbol_task);
    }

    // Prepare task arguments for all symbols
    static symbol_task_args_t sym_args[MAX_SYMBOLS_PER_SLOT];
    for (int s = 0; s < rel15->NrOfSymbols; s++) {
      sym_args[s].frame_parms = frame_parms;
      sym_args[s].slot = slot;
      sym_args[s].rel15 = rel15;
      sym_args[s].start_sc = start_sc;
      sym_args[s].symbol_sz = symbol_sz;
      sym_args[s].dlPtrsSymPos = dlPtrsSymPos;
      sym_args[s].n_ptrs = n_ptrs;
      sym_args[s].amp = amp;
      sym_args[s].amp_dmrs = amp_dmrs;
      sym_args[s].dmrs_Type = dmrs_Type;
      sym_args[s].txdataF_offset = txdataF_offset;
      sym_args[s].txdataF = txdataF;
      sym_args[s].tx_layers_base = (c16_t*)tx_layers;  // Base of tx_layers[0][0]
      sym_args[s].layerSz2 = layerSz2;
      sym_args[s].nrOfLayers = rel15->nrOfLayers;
      sym_args[s].nb_antennas_tx = frame_parms->nb_antennas_tx;
      sym_args[s].l_symbol = rel15->StartSymbolIndex + s;
      sym_args[s].l_prime = symbol_l_prime[s];
      sym_args[s].mod_dmrs = symbol_dmrs_ptr[s];
      sym_args[s].re_offset = symbol_re_offset[s];
    }

    // Execute parallel symbol processing
    enkiAddTaskSetMinRange(scheduler, g_symbol_mapping_task, sym_args, rel15->NrOfSymbols, 1);
    enkiWaitForTaskSet(scheduler, g_symbol_mapping_task);

    if (slot_timing_enabled) {
      clock_gettime(CLOCK_MONOTONIC, &t_end);
      total_mapping_ns = timespec_diff_ns_local(&t_start, &t_end);
    }

    // Skip the sequential symbol loop - we're done
    goto symbol_loop_done;
  }

  // ========== Sequential Symbol Loop (Fallback) ==========
  // Loop Over OFDM symbols:
  for (int l_symbol = rel15->StartSymbolIndex; l_symbol < rel15->StartSymbolIndex + rel15->NrOfSymbols; l_symbol++) {
    int l_prime = 0; // Will be set from pre-computed value if DMRS symbol

#ifdef DEBUG_DLSCH_MAPPING
    printf("PDSCH resource mapping symbol %d\n", l_symbol);
#endif
    /// DMRS: Use pre-computed modulation (computed before symbol loop)
    if ((dmrs_symbol_map & (1 << l_symbol))) { // DMRS time occasion
      // Find pre-computed DMRS for this symbol
      if (current_dmrs_idx < num_precomputed && dmrs_symbol_indices[current_dmrs_idx] == l_symbol) {
        mod_dmrs = mod_dmrs_precomputed[current_dmrs_idx];
        l_prime = dmrs_l_prime[current_dmrs_idx];
        current_dmrs_idx++;
      }
#ifdef DEBUG_DLSCH_MAPPING
      printf("dlDmrsScramblingId %d, SCID %d slot %d l_symbol %d (pre-computed)\n",
             rel15->dlDmrsScramblingId, rel15->SCID, slot, l_symbol);
      printf("DMRS modulation (symbol %d, %d symbols, type %d):\n", l_symbol, n_dmrs, dmrs_Type);
      for (int i = 0; i < n_dmrs / 2; i += 8) {
        for (int j = 0; j < 8; j++) {
          printf("%d %d\t", mod_dmrs[i + j].r, mod_dmrs[i + j].i);
        }
        printf("\n");
      }
#endif
    }
    uint32_t dmrs_idx = rel15->rbStart;
    if (rel15->refPoint == 0)
      dmrs_idx += rel15->BWPStart;
    dmrs_idx *= dmrs_Type == NFAPI_NR_DMRS_TYPE1 ? 6 : 4;

    // ========== PMI=0 FAST PATH: Direct RE mapping to antenna buffers ==========
    // When all PRGs use PMI=0 (identity matrix), layer[i] maps directly to antenna[i].
    // This eliminates the intermediate txdataF_precoding buffer and precoding step.
    const bool use_direct_mapping = check_all_pmi_zero(rel15);
    const size_t txdataF_offset_per_symbol = l_symbol * symbol_sz + txdataF_offset;
    const int nrOfLayers = rel15->nrOfLayers;

    int layer_sz = 0;
    if (use_direct_mapping) {
      // FAST PATH: Map layers directly to antenna buffers (no precoding needed)
      if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);

      // Use ISIP for parallel layer processing if available and beneficial
      // Enable for 2+ layers to parallelize layer RE mapping
      void* scheduler = isip_pool_get_scheduler();
      if (scheduler && nrOfLayers >= 2) {
        // Create task set on first use
        if (!g_re_mapping_task) {
          g_re_mapping_task = enkiCreateTaskSet(scheduler, re_mapping_layer_task);
        }

        // Prepare task arguments for each layer
        static re_mapping_task_args_t layer_args[8];  // Max 8 layers
        for (int layer = 0; layer < nrOfLayers; layer++) {
          layer_args[layer].frame_parms = frame_parms;
          layer_args[layer].slot = slot;
          layer_args[layer].rel15 = rel15;
          layer_args[layer].layer = layer;
          layer_args[layer].output = &txdataF[layer][txdataF_offset_per_symbol];
          layer_args[layer].txl_start = tx_layers[layer] + re_beginning_of_symbol;
          layer_args[layer].start_sc = start_sc;
          layer_args[layer].symbol_sz = symbol_sz;
          layer_args[layer].l_symbol = l_symbol;
          layer_args[layer].dlPtrsSymPos = dlPtrsSymPos;
          layer_args[layer].n_ptrs = n_ptrs;
          layer_args[layer].amp = amp;
          layer_args[layer].amp_dmrs = amp_dmrs;
          layer_args[layer].l_prime = l_prime;
          layer_args[layer].dmrs_Type = dmrs_Type;
          layer_args[layer].dmrs_start = mod_dmrs + dmrs_idx;
        }

        // Execute parallel layer processing
        enkiAddTaskSetMinRange(scheduler, g_re_mapping_task, layer_args, nrOfLayers, 1);
        enkiWaitForTaskSet(scheduler, g_re_mapping_task);

        // Get layer_sz from any layer (all same)
        layer_sz = layer_args[0].layer_sz_result;
      } else {
        // Fallback to sequential processing (single layer or no ISIP)
        for (int layer = 0; layer < nrOfLayers; layer++) {
          layer_sz = do_onelayer(frame_parms,
                                 slot,
                                 rel15,
                                 layer,
                                 &txdataF[layer][txdataF_offset_per_symbol],
                                 tx_layers[layer] + re_beginning_of_symbol,
                                 start_sc,
                                 symbol_sz,
                                 l_symbol,
                                 dlPtrsSymPos,
                                 n_ptrs,
                                 amp,
                                 amp_dmrs,
                                 l_prime,
                                 dmrs_Type,
                                 mod_dmrs + dmrs_idx);
        }
      }

      // Zero-fill unused antennas (antennas beyond nrOfLayers)
      // Include in RE mapping timing since this replaces the precoding step
      const int total_res = rel15->rbSize * NR_NB_SC_PER_RB;
      for (int ant = nrOfLayers; ant < frame_parms->nb_antennas_tx; ant++) {
        if (start_sc + total_res <= symbol_sz) {
          memset(&txdataF[ant][txdataF_offset_per_symbol + start_sc], 0, total_res * sizeof(c16_t));
        } else {
          const int neg_length = symbol_sz - start_sc;
          const int pos_length = total_res - neg_length;
          memset(&txdataF[ant][txdataF_offset_per_symbol + start_sc], 0, neg_length * sizeof(c16_t));
          memset(&txdataF[ant][txdataF_offset_per_symbol], 0, pos_length * sizeof(c16_t));
        }
      }

      if (slot_timing_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        total_mapping_ns += timespec_diff_ns_local(&t_start, &t_end);
      }

      re_beginning_of_symbol += layer_sz;
      // No precoding step needed - data already in final position (precoding_ns = 0)

    } else {
      // STANDARD PATH: RE mapping to temp buffer, then apply precoding matrix
      c16_t txdataF_precoding[nrOfLayers][symbol_sz] __attribute__((aligned(64)));

      // RE mapping to intermediate buffer
      if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
      for (int layer = 0; layer < nrOfLayers; layer++) {
        layer_sz = do_onelayer(frame_parms,
                               slot,
                               rel15,
                               layer,
                               txdataF_precoding[layer],
                               tx_layers[layer] + re_beginning_of_symbol,
                               start_sc,
                               symbol_sz,
                               l_symbol,
                               dlPtrsSymPos,
                               n_ptrs,
                               amp,
                               amp_dmrs,
                               l_prime,
                               dmrs_Type,
                               mod_dmrs + dmrs_idx);
      }
      if (slot_timing_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        total_mapping_ns += timespec_diff_ns_local(&t_start, &t_end);
      }
      re_beginning_of_symbol += layer_sz;

      // Apply precoding matrix to map layers to antennas
      if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
      for (int ant = 0; ant < frame_parms->nb_antennas_tx; ant++) {
        do_txdataF(txdataF, symbol_sz, txdataF_precoding, gNB, rel15, ant, start_sc, txdataF_offset_per_symbol);
      }
      if (slot_timing_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        total_precoding_ns += timespec_diff_ns_local(&t_start, &t_end);
      }
    }
  }

symbol_loop_done:
  stop_meas(&gNB->dlsch_resource_mapping_stats);
  stop_meas(&gNB->dlsch_precoding_stats);

  // Accumulate timings for this PDSCH
  if (slot_timing_enabled) {
    current_slot_timing.pdsch_re_mapping_ns += total_mapping_ns;
    current_slot_timing.pdsch_precoding_ns += total_precoding_ns;
  }

  /* output and its parts for each dlsch should be aligned on 64 bytes (or 8 * 64 bits)
   * should remain a multiple of 8 * 64 with enough offset to fit each dlsch
   */
  uint32_t size_output_tb = rel15->rbSize * NR_SYMBOLS_PER_SLOT * NR_NB_SC_PER_RB * Qm * rel15->nrOfLayers;
  return ((size_output_tb + 511) >> 9) << 6;
}

void nr_generate_pdsch(processingData_L1tx_t *msgTx, int frame, int slot)
{
  PHY_VARS_gNB *gNB = msgTx->gNB;
  NR_DL_FRAME_PARMS *frame_parms = &gNB->frame_parms;
  time_stats_t *dlsch_encoding_stats = &gNB->dlsch_encoding_stats;
  time_stats_t *tinput = &gNB->tinput;
  time_stats_t *tprep = &gNB->tprep;
  time_stats_t *tparity = &gNB->tparity;
  time_stats_t *toutput = &gNB->toutput;
  time_stats_t *dlsch_rate_matching_stats = &gNB->dlsch_rate_matching_stats;
  time_stats_t *dlsch_interleaving_stats = &gNB->dlsch_interleaving_stats;
  time_stats_t *dlsch_segmentation_stats = &gNB->dlsch_segmentation_stats;

  struct timespec t_enc_start, t_enc_end;
  size_t size_output = 0;

  for (int dlsch_id = 0; dlsch_id < msgTx->num_pdsch_slot; dlsch_id++) {
    NR_gNB_DLSCH_t *dlsch = msgTx->dlsch[dlsch_id];
    NR_DL_gNB_HARQ_t *harq = &dlsch->harq_process;
    nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15 = &harq->pdsch_pdu.pdsch_pdu_rel15;

    LOG_D(PHY,
          "pdsch: BWPStart %d, BWPSize %d, rbStart %d, rbsize %d\n",
          rel15->BWPStart,
          rel15->BWPSize,
          rel15->rbStart,
          rel15->rbSize);

    const int Qm = rel15->qamModOrder[0];

    /* PTRS */
    uint16_t dlPtrsSymPos = 0;
    int n_ptrs = 0;
    uint32_t ptrsSymbPerSlot = 0;
    if (rel15->pduBitmap & 0x1) {
      set_ptrs_symb_idx(&dlPtrsSymPos,
                        rel15->NrOfSymbols,
                        rel15->StartSymbolIndex,
                        1 << rel15->PTRSTimeDensity,
                        rel15->dlDmrsSymbPos);
      n_ptrs = (rel15->rbSize + rel15->PTRSFreqDensity - 1) / rel15->PTRSFreqDensity;
      ptrsSymbPerSlot = get_ptrs_symbols_in_slot(dlPtrsSymPos, rel15->StartSymbolIndex, rel15->NrOfSymbols);
    }
    harq->unav_res = ptrsSymbPerSlot * n_ptrs;

    /// CRC, coding, interleaving and rate matching
    AssertFatal(harq->pdu != NULL, "%4d.%2d no HARQ PDU for PDSCH generation\n", msgTx->frame, msgTx->slot);

    /* output and its parts for each dlsch should be aligned on 64 bytes (or 8 * 64 bits)
     * => size_output is a sum of parts sizes rounded up to a multiple of 8 * 64
     */
    size_t size_output_tb = rel15->rbSize * NR_SYMBOLS_PER_SLOT * NR_NB_SC_PER_RB * Qm * rel15->nrOfLayers;
    size_output += ceil_mod(size_output_tb, 8 * 64);
  }

  unsigned char output[size_output >> 3] __attribute__((aligned(64)));
  bzero(output, sizeof(output));

  // Measure encoding time (CRC, LDPC, rate matching)
  start_meas(dlsch_encoding_stats);
  if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_enc_start);
  if (nr_dlsch_encoding(gNB,
                        msgTx,
                        frame,
                        slot,
                        frame_parms,
                        output,
                        tinput,
                        tprep,
                        tparity,
                        toutput,
                        dlsch_rate_matching_stats,
                        dlsch_interleaving_stats,
                        dlsch_segmentation_stats)
      == -1) {
    return;
  }
  if (slot_timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_enc_end);
    current_slot_timing.pdsch_encoding_ns += timespec_diff_ns_local(&t_enc_start, &t_enc_end);
  }
  stop_meas(dlsch_encoding_stats);

  unsigned char *output_ptr = output;
  for (int dlsch_id = 0; dlsch_id < msgTx->num_pdsch_slot; dlsch_id++) {
    // Pass -1 as pdsch_idx to disable precomputed DMRS (not available in legacy path)
    output_ptr += do_one_dlsch(output_ptr, gNB, msgTx->dlsch[dlsch_id], slot, -1);
  }
}

void dump_pdsch_stats(FILE *fd, PHY_VARS_gNB *gNB)
{
  for (int i = 0; i < MAX_MOBILES_PER_GNB; i++) {
    NR_gNB_PHY_STATS_t *stats = &gNB->phy_stats[i];
    if (stats->active && stats->frame != stats->dlsch_stats.dump_frame) {
      stats->dlsch_stats.dump_frame = stats->frame;
      fprintf(fd,
              "DLSCH RNTI %x: current_Qm %d, current_RI %d, total_bytes TX %d\n",
              stats->rnti,
              stats->dlsch_stats.current_Qm,
              stats->dlsch_stats.current_RI,
              stats->dlsch_stats.total_bytes_tx);
    }
  }
}

// ========== Split PDSCH Processing for Async Memory Clear Overlap ==========
// Phase 1: Encoding only (CRC, LDPC, rate matching) - can run in parallel with memory clear
// Phase 2: Codeword processing (scrambling, modulation, layer mapping, RE mapping) - needs clear txdataF

int nr_pdsch_encoding_phase(processingData_L1tx_t *msgTx, int frame, int slot)
{
  PHY_VARS_gNB *gNB = msgTx->gNB;
  NR_DL_FRAME_PARMS *frame_parms = &gNB->frame_parms;
  time_stats_t *dlsch_encoding_stats = &gNB->dlsch_encoding_stats;
  time_stats_t *tinput = &gNB->tinput;
  time_stats_t *tprep = &gNB->tprep;
  time_stats_t *tparity = &gNB->tparity;
  time_stats_t *toutput = &gNB->toutput;
  time_stats_t *dlsch_rate_matching_stats = &gNB->dlsch_rate_matching_stats;
  time_stats_t *dlsch_interleaving_stats = &gNB->dlsch_interleaving_stats;
  time_stats_t *dlsch_segmentation_stats = &gNB->dlsch_segmentation_stats;

  struct timespec t_enc_start, t_enc_end;
  size_t size_output = 0;

  // Calculate total output size for all PDSCHs
  for (int dlsch_id = 0; dlsch_id < msgTx->num_pdsch_slot; dlsch_id++) {
    NR_gNB_DLSCH_t *dlsch = msgTx->dlsch[dlsch_id];
    NR_DL_gNB_HARQ_t *harq = &dlsch->harq_process;
    nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15 = &harq->pdsch_pdu.pdsch_pdu_rel15;
    const int Qm = rel15->qamModOrder[0];

    // PTRS handling
    uint16_t dlPtrsSymPos = 0;
    int n_ptrs = 0;
    uint32_t ptrsSymbPerSlot = 0;
    if (rel15->pduBitmap & 0x1) {
      set_ptrs_symb_idx(&dlPtrsSymPos, rel15->NrOfSymbols, rel15->StartSymbolIndex,
                        1 << rel15->PTRSTimeDensity, rel15->dlDmrsSymbPos);
      n_ptrs = (rel15->rbSize + rel15->PTRSFreqDensity - 1) / rel15->PTRSFreqDensity;
      ptrsSymbPerSlot = get_ptrs_symbols_in_slot(dlPtrsSymPos, rel15->StartSymbolIndex, rel15->NrOfSymbols);
    }
    harq->unav_res = ptrsSymbPerSlot * n_ptrs;

    AssertFatal(harq->pdu != NULL, "%4d.%2d no HARQ PDU for PDSCH generation\n", msgTx->frame, msgTx->slot);

    size_t size_output_tb = rel15->rbSize * NR_SYMBOLS_PER_SLOT * NR_NB_SC_PER_RB * Qm * rel15->nrOfLayers;
    size_output += ceil_mod(size_output_tb, 8 * 64);
  }

  // Verify buffer size
  AssertFatal((size_output >> 3) <= PDSCH_ENCODED_OUTPUT_MAX_SIZE,
              "PDSCH encoded output size %zu exceeds buffer %d\n",
              size_output >> 3, PDSCH_ENCODED_OUTPUT_MAX_SIZE);

  // Store size for codeword phase
  g_pdsch_encoded_size = size_output;
  bzero(g_pdsch_encoded_output, size_output >> 3);

  // Perform encoding (CRC, LDPC, rate matching)
  start_meas(dlsch_encoding_stats);
  if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_enc_start);
  if (nr_dlsch_encoding(gNB, msgTx, frame, slot, frame_parms, g_pdsch_encoded_output,
                        tinput, tprep, tparity, toutput,
                        dlsch_rate_matching_stats, dlsch_interleaving_stats,
                        dlsch_segmentation_stats) == -1) {
    return -1;
  }
  if (slot_timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_enc_end);
    current_slot_timing.pdsch_encoding_ns += timespec_diff_ns_local(&t_enc_start, &t_enc_end);
  }
  stop_meas(dlsch_encoding_stats);

  return 0;
}

void nr_pdsch_codeword_phase(processingData_L1tx_t *msgTx, int frame, int slot)
{
  PHY_VARS_gNB *gNB = msgTx->gNB;

  LOG_D(PHY, "PDSCH codeword phase started (%d) in frame %d.%d\n", msgTx->num_pdsch_slot, frame, slot);

  // Process each PDSCH using the encoded output from phase 1
  unsigned char *output_ptr = g_pdsch_encoded_output;
  for (int dlsch_id = 0; dlsch_id < msgTx->num_pdsch_slot; dlsch_id++) {
    output_ptr += do_one_dlsch(output_ptr, gNB, msgTx->dlsch[dlsch_id], slot, dlsch_id);
  }
}
