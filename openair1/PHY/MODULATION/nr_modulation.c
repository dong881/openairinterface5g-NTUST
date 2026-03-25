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

#include "nr_modulation.h"
#include "PHY/NR_REFSIG/nr_mod_table.h"
#include "executables/softmodem-common.h"
#include <simde/x86/avx512.h>

// ISIP support - only for gNB builds with ISIP pool available
// The ISIP pool is only built for gNB, so check if the header exists
#if __has_include("PHY/ISIP_POOL/isip_pool.h") && __has_include("external/enkiTS/src/TaskScheduler_c.h")
#define ISIP_AVAILABLE 1
#include "PHY/ISIP_POOL/isip_pool.h"
#include "external/enkiTS/src/TaskScheduler_c.h"
#else
#define ISIP_AVAILABLE 0
#endif
// Lacking declaration in present simde external package, will be detected as compilation error when they will add it
#define simde_mm512_extracti64x2_epi64(a...) _mm512_extracti64x2_epi64(a)

// #define DEBUG_DLSCH_PRECODING_PRINT_WITH_TRIVIAL // TODO: For debug, to be removed if want to merge to develop
// #define DEBUG_LAYER_MAPPING
#define USE_NEON
// #define USE_GATHER
//  Table 6.3.1.5-1 Precoding Matrix W 1 layer 2 antenna ports 'n' = -1 and 'o' = -j
const char nr_W_1l_2p[6][2][1] = {
    {{'1'}, {'0'}}, // pmi 0
    {{'0'}, {'1'}},
    {{'1'}, {'1'}},
    {{'1'}, {'n'}},
    {{'1'}, {'j'}},
    {{'1'}, {'o'}} // pmi 5
};

// Table 6.3.1.5-3 Precoding Matrix W 1 layer 4 antenna ports 'n' = -1 and 'o' = -j
const char nr_W_1l_4p[28][4][1] = {
    {{'1'}, {'0'}, {'0'}, {'0'}}, // pmi 0
    {{'0'}, {'1'}, {'0'}, {'0'}},
    {{'0'}, {'0'}, {'1'}, {'0'}},
    {{'0'}, {'0'}, {'0'}, {'1'}},
    {{'1'}, {'0'}, {'1'}, {'0'}},
    {{'1'}, {'0'}, {'n'}, {'0'}},
    {{'1'}, {'0'}, {'j'}, {'0'}},
    {{'1'}, {'0'}, {'o'}, {'0'}}, // pmi 7
    {{'0'}, {'1'}, {'0'}, {'1'}}, // pmi 8
    {{'0'}, {'1'}, {'0'}, {'n'}},
    {{'0'}, {'1'}, {'0'}, {'j'}},
    {{'0'}, {'1'}, {'0'}, {'o'}},
    {{'1'}, {'1'}, {'1'}, {'1'}},
    {{'1'}, {'1'}, {'j'}, {'j'}},
    {{'1'}, {'1'}, {'n'}, {'n'}},
    {{'1'}, {'1'}, {'o'}, {'o'}},
    {{'1'}, {'j'}, {'1'}, {'j'}}, // pmi
    // 16
    {{'1'}, {'j'}, {'j'}, {'n'}},
    {{'1'}, {'j'}, {'n'}, {'o'}},
    {{'1'}, {'j'}, {'o'}, {'1'}},
    {{'1'}, {'n'}, {'1'}, {'n'}},
    {{'1'}, {'n'}, {'j'}, {'o'}},
    {{'1'}, {'n'}, {'n'}, {'1'}},
    {{'1'}, {'n'}, {'o'}, {'j'}}, // pmi 23
    {{'1'}, {'o'}, {'1'}, {'o'}}, // pmi 24
    {{'1'}, {'o'}, {'j'}, {'1'}},
    {{'1'}, {'o'}, {'n'}, {'j'}},
    {{'1'}, {'o'}, {'o'}, {'n'}} // pmi 27
};

// Table 6.3.1.5-4 Precoding Matrix W 2 antenna ports layers 2  'n' = -1 and 'o' = -j
const char nr_W_2l_2p[3][2][2] = {
    {{'1', '0'}, {'0', '1'}}, // pmi 0
    {{'1', '1'}, {'1', 'n'}},
    {{'1', '1'}, {'j', 'o'}} // pmi 2
};

// Table 6.3.1.5-5 Precoding Matrix W 2 layers 4 antenna ports 'n' = -1 and 'o' = -j
const char nr_W_2l_4p[22][4][2] = {
    {{'1', '0'}, {'0', '1'}, {'0', '0'}, {'0', '0'}}, // pmi 0
    {{'1', '0'}, {'0', '0'}, {'0', '1'}, {'0', '0'}}, {{'1', '0'}, {'0', '0'}, {'0', '0'}, {'0', '1'}},
    {{'0', '0'}, {'1', '0'}, {'0', '1'}, {'0', '0'}}, // pmi 3
    {{'0', '0'}, {'1', '0'}, {'0', '0'}, {'0', '1'}}, // pmi 4
    {{'0', '0'}, {'0', '0'}, {'1', '0'}, {'0', '1'}}, {{'1', '0'}, {'0', '1'}, {'1', '0'}, {'0', 'o'}},
    {{'1', '0'}, {'0', '1'}, {'1', '0'}, {'0', 'j'}}, {{'1', '0'}, {'0', '1'}, {'o', '0'}, {'0', '1'}}, // pmi 8
    {{'1', '0'}, {'0', '1'}, {'o', '0'}, {'0', 'n'}}, {{'1', '0'}, {'0', '1'}, {'n', '0'}, {'0', 'o'}},
    {{'1', '0'}, {'0', '1'}, {'n', '0'}, {'0', 'j'}}, // pmi 11
    {{'1', '0'}, {'0', '1'}, {'j', '0'}, {'0', '1'}}, // pmi 12
    {{'1', '0'}, {'0', '1'}, {'j', '0'}, {'0', 'n'}}, {{'1', '1'}, {'1', '1'}, {'1', 'n'}, {'1', 'n'}},
    {{'1', '1'}, {'1', '1'}, {'j', 'o'}, {'j', 'o'}}, // pmi 15
    {{'1', '1'}, {'j', 'j'}, {'1', 'n'}, {'j', 'o'}}, // pmi 16
    {{'1', '1'}, {'j', 'j'}, {'j', 'o'}, {'n', '1'}}, {{'1', '1'}, {'n', 'n'}, {'1', 'n'}, {'n', '1'}},
    {{'1', '1'}, {'n', 'n'}, {'j', 'o'}, {'o', 'j'}}, // pmi 19
    {{'1', '1'}, {'o', 'o'}, {'1', 'n'}, {'o', 'j'}}, {{'1', '1'}, {'o', 'o'}, {'j', 'o'}, {'1', 'n'}} // pmi 21
};

// Table 6.3.1.5-6 Precoding Matrix W 3 layers 4 antenna ports 'n' = -1 and 'o' = -j
const char nr_W_3l_4p[7][4][3] = {{{'1', '0', '0'}, {'0', '1', '0'}, {'0', '0', '1'}, {'0', '0', '0'}}, // pmi 0
                                  {{'1', '0', '0'}, {'0', '1', '0'}, {'1', '0', '0'}, {'0', '0', '1'}},
                                  {{'1', '0', '0'}, {'0', '1', '0'}, {'n', '0', '0'}, {'0', '0', '1'}},
                                  {{'1', '1', '1'}, {'1', 'n', '1'}, {'1', '1', 'n'}, {'1', 'n', 'n'}}, // pmi 3
                                  {{'1', '1', '1'}, {'1', 'n', '1'}, {'j', 'j', 'o'}, {'j', 'o', 'o'}}, // pmi 4
                                  {{'1', '1', '1'}, {'n', '1', 'n'}, {'1', '1', 'n'}, {'n', '1', '1'}},
                                  {{'1', '1', '1'}, {'n', '1', 'n'}, {'j', 'j', 'o'}, {'o', 'j', 'j'}}};

// Table 6.3.1.5-7 Precoding Matrix W 4 layers 4 antenna ports 'n' = -1 and 'o' = -j
const char nr_W_4l_4p[5][4][4] = {
    {{'1', '0', '0', '0'}, {'0', '1', '0', '0'}, {'0', '0', '1', '0'}, {'0', '0', '0', '1'}}, // pmi 0
    {{'1', '1', '0', '0'}, {'0', '0', '1', '1'}, {'1', 'n', '0', '0'}, {'0', '0', '1', 'n'}},
    {{'1', '1', '0', '0'}, {'0', '0', '1', '1'}, {'j', 'o', '0', '0'}, {'0', '0', 'j', 'o'}},
    {{'1', '1', '1', '1'}, {'1', 'n', '1', 'n'}, {'1', '1', 'n', 'n'}, {'1', 'n', 'n', '1'}}, // pmi 3
    {{'1', '1', '1', '1'}, {'1', 'n', '1', 'n'}, {'j', 'j', 'o', 'o'}, {'j', 'o', 'o', 'j'}} // pmi 4
};

void nr_modulation(const uint32_t *in, uint32_t length, uint16_t mod_order, int16_t *out)
{
  const uint16_t mask = ((1 << mod_order) - 1);
  int32_t *out32 = (int32_t *)out;
  const uint8_t *in_bytes = (const uint8_t *)in;
  const uint64_t *in64 = (const uint64_t *)in;
  int64_t *out64 = (int64_t *)out;
  uint32_t i = 0;

  LOG_D(PHY, "nr_modulation: length %d, mod_order %d\n", length, mod_order);

  switch (mod_order) {
    case 2: {
      simde__m128i *nr_mod_table128 = (simde__m128i *)nr_qpsk_byte_mod_table;
      simde__m128i *out128 = (simde__m128i *)out;
      for (i = 0; i < length / 8; i++)
        out128[i] = nr_mod_table128[in_bytes[i]];
      // the bits that are left out
      i = i * 8 / 2;
      int32_t *nr_mod_table32 = (int32_t *)nr_qpsk_mod_table;
      while (i < length / 2) {
        const int idx = ((in_bytes[(i * 2) / 8] >> ((i * 2) & 0x7)) & mask);
        out32[i] = nr_mod_table32[idx];
        i++;
      }
    }
      return;

    case 4:
      for (i = 0; i < length / 8; i++)
        out64[i] = nr_16qam_byte_mod_table[in_bytes[i]];
      // the bits that are left out
      i = i * 8 / 4;
      while (i < length / 4) {
        const int idx = ((in_bytes[(i * 4) / 8] >> ((i * 4) & 0x7)) & mask);
        out32[i] = nr_16qam_mod_table[idx];
        i++;
      }
      return;

    case 6:
      if (length > (3 * 64))
        for (i = 0; i < length - 3 * 64; i += 3 * 64) {
          uint64_t x = *in64++;
          uint64_t x1 = x & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x >> 12) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x >> 24) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x >> 36) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x >> 48) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          uint64_t x2 = (x >> 60);
          x = *in64++;
          x2 |= x << 4;
          x1 = x2 & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x2 >> 12) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x2 >> 24) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x2 >> 36) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x2 >> 48) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x2 = ((x >> 56) & 0xf0) | (x2 >> 60);
          x = *in64++;
          x2 |= x << 8;
          x1 = x2 & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x2 >> 12) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x2 >> 24) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x2 >> 36) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x1 = (x2 >> 48) & 0xfff;
          *out64++ = nr_64qam_mod_table[x1];
          x2 = ((x >> 52) & 0xff0) | (x2 >> 60);
          *out64++ = nr_64qam_mod_table[x2];
        }

      while (i + 24 <= length) {
        uint32_t xx = 0;
        memcpy(&xx, in_bytes + i / 8, 3);
        uint64_t x1 = xx & 0xfff;
        *out64++ = nr_64qam_mod_table[x1];
        x1 = (xx >> 12) & 0xfff;
        *out64++ = nr_64qam_mod_table[x1];
        i += 24;
      }
      if (i != length) {
        uint32_t xx = 0;
        memcpy(&xx, in_bytes + i / 8, 2);
        uint64_t x1 = xx & 0xfff;
        *out64++ = nr_64qam_mod_table[x1];
      }
      return;

    case 8: {
      int32_t *nr_mod_table32 = (int32_t *)nr_256qam_mod_table;
      for (i = 0; i < length / 8; i++)
        out32[i] = nr_mod_table32[in_bytes[i]];
    }
      return;

    default:
      break;
  }
  AssertFatal(false, "Invalid or unsupported modulation order %d\n", mod_order);
}

void nr_layer_mapping(int nbCodes,
                      int encoded_len,
                      c16_t mod_symbs[nbCodes][encoded_len],
                      uint8_t n_layers,
                      int layerSz,
                      uint32_t n_symbs,
                      c16_t tx_layers[][layerSz])
{
  LOG_D(PHY, "Doing layer mapping for %d layers, %d symbols\n", n_layers, n_symbs);
  c16_t *mod = mod_symbs[0];
  switch (n_layers) {
    case 1:
      memcpy(tx_layers[0], mod, n_symbs * sizeof(**mod_symbs));
      break;

    case 2: {
      int i = 0;
      c16_t *tx0 = tx_layers[0];
      c16_t *tx1 = tx_layers[1];
#if defined(__AVX512BW__)
      simde__m512i perm2a = simde_mm512_set_epi32(30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2, 0);
      simde__m512i perm2b = simde_mm512_set_epi32(31, 29, 27, 25, 23, 21, 19, 17, 15, 13, 11, 9, 7, 5, 3, 1);
      for (; i < (n_symbs & ~31); i += 32) {
        simde__m512i a = *(simde__m512i *)(mod + i);
        simde__m512i b = *(simde__m512i *)(mod + i + 16);
        *(simde__m512i *)tx0 = simde_mm512_permutex2var_epi32(a, perm2a, b);
        *(simde__m512i *)tx1 = simde_mm512_permutex2var_epi32(a, perm2b, b);
        tx0 += 16;
        tx1 += 16;
      }
#endif
#ifdef __AVX2__
      simde__m256i perm2 = simde_mm256_set_epi32(7, 5, 3, 1, 6, 4, 2, 0);
      for (; i < (n_symbs & ~7); i += 8) {
        simde__m256i d = simde_mm256_permutevar8x32_epi32(*(simde__m256i *)(mod + i), perm2);
        *(simde__m128i *)tx0 = simde_mm256_extractf128_si256(d, 0);
        *(simde__m128i *)tx1 = simde_mm256_extractf128_si256(d, 1);
        tx0 += 4;
        tx1 += 4;
      }
#endif
#if defined(__aarch64__) && defined(USE_NEON)
      // SIMDe doesn't handle this properly, gcc up to 14.2 neither
      uint8_t const perm0[16] = {0, 1, 2, 3, 8, 9, 10, 11, 4, 5, 6, 7, 12, 13, 14, 15};
      uint8x16_t perm = vld1q_u8(perm0);
      uint8x16_t d;
      for (; i < (n_symbs & (~3)); i += 4) {
        d = vqtbl1q_u8(*(uint8x16_t *)(mod + i), perm);
        *(int64_t *)tx0 = vgetq_lane_u64((uint64x2_t)d, 0);
        *(int64_t *)tx1 = vgetq_lane_u64((uint64x2_t)d, 1);
        tx0 += 2;
        tx1 += 2;
      }
#endif
      for (; i < n_symbs; i += 2) {
        *tx0++ = mod[i];
        *tx1++ = mod[i + 1];
      }
    } break;
    case 3: {
      int i = 0;
      c16_t *tx0 = tx_layers[0];
      c16_t *tx1 = tx_layers[1];
      c16_t *tx2 = tx_layers[2];
#if defined(__AVX512F) && defined(__AVX512VBMI__)
      simde__m512i perm3_0 = simde_mm512_set_epi32(13 + 16,
                                                   10 + 16,
                                                   7 + 16,
                                                   4 + 16,
                                                   1 + 16,
                                                   14 + 16,
                                                   11 + 16,
                                                   8 + 16,
                                                   5 + 16,
                                                   2 + 16,
                                                   15,
                                                   12,
                                                   9,
                                                   6,
                                                   3,
                                                   0);
      simde__m512i perm3_0b = simde_mm512_set_epi32(13 + 16, 10 + 16, 7 + 16, 4 + 16, 1 + 16, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
      simde__m512i perm3_1 = simde_mm512_set_epi32(14 + 16,
                                                   11 + 16,
                                                   8 + 16,
                                                   5 + 16,
                                                   2 + 16,
                                                   15 + 16,
                                                   12 + 16,
                                                   9 + 16,
                                                   6 + 16,
                                                   3 + 16,
                                                   0 + 16,
                                                   13,
                                                   10,
                                                   7,
                                                   4,
                                                   1);
      simde__m512i perm3_1b = simde_mm512_set_epi32(14 + 16, 11 + 16, 8 + 16, 5 + 16, 2 + 16, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
      simde__m512i perm3_2 = simde_mm512_set_epi32(15 + 16,
                                                   12 + 16,
                                                   9 + 16,
                                                   6 + 16,
                                                   3 + 16,
                                                   0 + 16,
                                                   13 + 16,
                                                   10 + 16,
                                                   7 + 16,
                                                   4 + 16,
                                                   1 + 16,
                                                   14,
                                                   11,
                                                   8,
                                                   5,
                                                   2);
      simde__m512i perm3_2b = simde_mm512_set_epi32(15 + 16, 12 + 16, 9 + 16, 6 + 16, 3 + 16, 0 + 16, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
      for (; i < (n_symbs & ~63); i += 48) {
        simde__m512i i0 = *(simde__m512i *)(mod + i);
        simde__m512i i1 = *(simde__m512i *)(mod + i + 16);
        simde__m512i i2 = *(simde__m512i *)(mod + i + 32);
        simde__m512i d0 = simde_mm512_permutex2var_epi32(i0, perm3_0, i1);
        *(simde__m512i *)tx0 = simde_mm512_permutex2var_epi32(d0, perm3_0b, i2); // 11000000
        tx0 += 16;
        d0 = simde_mm512_permutex2var_epi32(i0, perm3_1, i1);
        *(simde__m512i *)tx1 = simde_mm512_permutex2var_epi32(d0, perm3_1b, i2); // 11000000
        tx1 += 16;
        d0 = simde_mm512_permutex2var_epi32(i0, perm3_2, i1);
        *(simde__m512i *)tx2 = simde_mm512_permutex2var_epi32(d0, perm3_2b, i2); // 11000000
        tx2 += 16;
      }
#endif
#ifdef __AVX2__
      {
        simde__m256i perm3_0 = simde_mm256_set_epi32(5, 2, 7, 4, 1, 6, 3, 0);
        simde__m256i perm3_1 = simde_mm256_set_epi32(6, 3, 0, 5, 2, 7, 4, 1);
        simde__m256i perm3_2 = simde_mm256_set_epi32(7, 4, 1, 6, 3, 0, 5, 2);
        for (; i < (n_symbs & ~31); i += 24) {
          simde__m256i i0 = *(simde__m256i *)(mod + i);
          simde__m256i i1 = *(simde__m256i *)(mod + i + 8);
          simde__m256i i2 = *(simde__m256i *)(mod + i + 16);
          simde__m256i d0 = simde_mm256_permutevar8x32_epi32(i0, perm3_0);
          simde__m256i d1 = simde_mm256_permutevar8x32_epi32(i1, perm3_0);
          simde__m256i d2 = simde_mm256_permutevar8x32_epi32(i2, perm3_0);
          simde__m256i d3 = simde_mm256_blend_epi32(d0, d1, 0x38); // 00111000
          *(simde__m256i *)tx0 = simde_mm256_blend_epi32(d3, d2, 0xc0); // 11000000
          tx0 += 8;
          d0 = simde_mm256_permutevar8x32_epi32(i0, perm3_1);
          d1 = simde_mm256_permutevar8x32_epi32(i1, perm3_1);
          d2 = simde_mm256_permutevar8x32_epi32(i2, perm3_1);
          d3 = simde_mm256_blend_epi32(d0, d1, 0x18); // 00011000
          *(simde__m256i *)tx1 = simde_mm256_blend_epi32(d3, d2, 0xe0); // 11100000
          tx1 += 8;
          d0 = simde_mm256_permutevar8x32_epi32(i0, perm3_2);
          d1 = simde_mm256_permutevar8x32_epi32(i1, perm3_2);
          d2 = simde_mm256_permutevar8x32_epi32(i2, perm3_2);
          d3 = simde_mm256_blend_epi32(d0, d1, 0x1c); // 00011100
          *(simde__m256i *)tx2 = simde_mm256_blend_epi32(d3, d2, 0xe0); // 11100000
          tx2 += 8;
        }
      }
#endif
      for (; i < n_symbs; i += 3) {
        *tx0++ = mod[i];
        *tx1++ = mod[i + 1];
        *tx2++ = mod[i + 2];
      }

#ifdef DEBUG_LAYER_MAPPING
      printf("\nsymb %d/%u\n", i << 3, n_symbs);
      printf(" layer 0:\t");
      for (int j = 0; j < 8 * 6; j += 6) {
        printf("%d %d ", ((int16_t *)&mod[i << 3])[j], ((int16_t *)&mod[i << 3])[j + 1]);
      }
      printf("\n layer 1:\t");
      for (int j = 2; j < 8 * 6; j += 6) {
        printf("%d %d ", ((int16_t *)&mod[i << 3])[j], ((int16_t *)&mod[i << 3])[j + 1]);
      }
      printf("\n layer 2:\t");
      for (int j = 4; j < 8 * 6; j += 6) {
        printf("%d %d ", ((int16_t *)&mod[i << 3])[j], ((int16_t *)&mod[i << 3])[j + 1]);
      }
      printf("\n Mapping layer 0:\t");
      for (int j = 0; j < 16; j++) {
        printf("%d ", ((int16_t *)&tx_layers[0][n << 3])[j]);
      }
      printf("\n Mapping layer 1:\t");
      for (int j = 0; j < 16; j++) {
        printf("%d ", ((int16_t *)&tx_layers[1][n << 3])[j]);
      }
      printf("\n Mapping layer 2:\t");
      for (int j = 0; j < 16; j++) {
        printf("%d ", ((int16_t *)&tx_layers[2][n << 3])[j]);
      }
#endif
    } break;

    case 4: {
      int i = 0;
      c16_t *tx0 = tx_layers[0];
      c16_t *tx1 = tx_layers[1];
      c16_t *tx2 = tx_layers[2];
      c16_t *tx3 = tx_layers[3];
#if defined(__AVX512VBMI__)
      simde__m512i perm4 = simde_mm512_set_epi32(15, 11, 7, 3, 14, 10, 6, 2, 13, 9, 5, 1, 12, 8, 4, 0);
      for (; i < (n_symbs & ~15); i += 16) {
        simde__m512i e = simde_mm512_permutexvar_epi32(perm4, *(simde__m512i *)(mod + i));
        *(simde__m128i *)tx0 = simde_mm512_extracti64x2_epi64(e, 0);
        tx0 += 4;
        *(simde__m128i *)tx1 = simde_mm512_extracti64x2_epi64(e, 1);
        tx1 += 4;
        *(simde__m128i *)tx2 = simde_mm512_extracti64x2_epi64(e, 2);
        tx2 += 4;
        *(simde__m128i *)tx3 = simde_mm512_extracti64x2_epi64(e, 3);
        tx3 += 4;
      }
#endif
#ifdef __AVX2__
      {
        simde__m256i perm4 = simde_mm256_set_epi32(7, 3, 6, 2, 5, 1, 4, 0);
        for (; i < (n_symbs & ~7); i += 8) {
          simde__m256i e = simde_mm256_permutevar8x32_epi32(*(simde__m256i *)(mod + i), perm4);
          *(uint64_t *)tx0 = simde_mm256_extract_epi64(e, 0);
          tx0 += 2;
          *(uint64_t *)tx1 = simde_mm256_extract_epi64(e, 1);
          tx1 += 2;
          *(uint64_t *)tx2 = simde_mm256_extract_epi64(e, 2);
          tx2 += 2;
          *(uint64_t *)tx3 = simde_mm256_extract_epi64(e, 3);
          tx3 += 2;
        }
      }
#endif
#if defined(__aarch64__) && defined(USE_NEON)
      // SIMDe doesn't handle this properly, gcc up to 14.2 neither
      for (; i < (n_symbs & ~3); i += 4) {
        uint32x4_t d4 = *(uint32x4_t *)(mod + i);
        *(uint32_t *)tx0 = vgetq_lane_u32(d4, 0); 
	tx0++;
        *(uint32_t *)tx1 = vgetq_lane_u32(d4, 1); 
	tx1++;
        *(uint32_t *)tx2 = vgetq_lane_u32(d4, 0); 
	tx2++;
        *(uint32_t *)tx3 = vgetq_lane_u32(d4, 1); 
	tx3++;
      }
#endif
      for (; i < n_symbs; i += 4) {
        *tx0++ = mod[i];
        *tx1++ = mod[i + 1];
        *tx2++ = mod[i + 2];
        *tx3++ = mod[i + 3];
      }
    } break;

    case 5:
    case 6:
    case 7:
    case 8:
      /*
      // Layer 0,1
      for (int i = 0; i < n_symbs; i += 2) {
const int txIdx = i / 2;
tx_layer[0][txIdx] = mod_symbs[0][i];
tx_layer[1][txIdx] = mod_symbs[0][i + 1];
      }
      // layers 2,3,4
      else
for (int i = 0; i < n_symbs; i += 3) {
const int txIdx = i / 3;
tx_layer[2][txIdx] = mod_symbs[1][i + 2];
tx_layer[3][txIdx] = mod_symbs[1][i + 3];
tx_layer[4][txIdx] = mod_symbs[1][i + 4];
}
      break;

case 6:
      for (int q=0; q<2; q++)
for (int i = 0; i < n_symbs; i += 3) {
const int txIdx = i / 3;
tx_layer[0][txIdx] = mod_symbs[q][i + layer];
tx_layer[1][txIdx] = mod_symbs[q][i + layer];
tx_layer[2][txIdx] = mod_symbs[q][i + layer];
tx_layer[3][txIdx] = mod_symbs[q][i + layer];
tx_layer[4][txIdx] = mod_symbs[q][i + layer];
tx_layer[5][txIdx] = mod_symbs[q][i + layer];
}
      break;

case 7:
      if (layer < 3)
for (int i = 0; i < n_symbs; i += 3) {
const int txIdx = i / 3;
tx_layer[txIdx] = mod_symbs[1][i + layer];
}
      else
for (int i = 0; i < n_symbs; i += 4) {
const int txIdx = i / 4;
tx_layer[txIdx] = mod_symbs[0][i + layer];
}
      break;

case 8:
      for (int q=0; q<2; q++)
      for (int i = 0; i < n_symbs; i += 4) {
const int txIdx = i / 4;
tx_layer[txIdx] = mod_symbs[q][i + layer];
      }
      break;
*/
    default:
      AssertFatal(0, "Invalid number of layers %d\n", n_layers);
  }
}

// ========== Parallel Layer Mapping with Symbol Range Splitting ==========
// Process a range of modulated symbols for layer mapping
// Each task processes symbols from start_symb to end_symb (input indices)
// For n_layers, input index i maps to: tx_layers[i % n_layers][i / n_layers]

#if ISIP_AVAILABLE
typedef struct {
  c16_t *mod;           // Input modulated symbols
  c16_t *tx_layers[8];  // Output layer buffers (max 8 layers)
  uint32_t start_symb;  // Start symbol index (in mod array, must be aligned to n_layers)
  uint32_t end_symb;    // End symbol index (exclusive)
  uint8_t n_layers;     // Number of layers
} layer_mapping_task_args_t;

// Task function for parallel layer mapping
// IMPORTANT: start_symb must be properly aligned for SIMD operation:
//   - Case 1: any alignment (memcpy)
//   - Case 2: aligned to 32 (AVX512) or 8 (AVX2)
//   - Case 3: aligned to 48 (AVX512) or 24 (AVX2)
//   - Case 4: aligned to 16 (AVX512) or 8 (AVX2)
static void layer_mapping_range_task(uint32_t start, uint32_t end, uint32_t threadNum, void* pArgs)
{
  layer_mapping_task_args_t *all_args = (layer_mapping_task_args_t*)pArgs;

  for (uint32_t task_idx = start; task_idx < end; task_idx++) {
    layer_mapping_task_args_t *args = &all_args[task_idx];
    c16_t *mod = args->mod;
    const uint32_t start_symb = args->start_symb;
    const uint32_t end_symb = args->end_symb;
    const uint8_t n_layers = args->n_layers;

    // Output start index = start_symb / n_layers
    uint32_t out_idx = start_symb / n_layers;

    switch (n_layers) {
      case 1: {
        // Simple memcpy for single layer
        memcpy(args->tx_layers[0] + out_idx, mod + start_symb, (end_symb - start_symb) * sizeof(c16_t));
      } break;

      case 2: {
        c16_t *tx0 = args->tx_layers[0] + out_idx;
        c16_t *tx1 = args->tx_layers[1] + out_idx;
        uint32_t i = start_symb;
#if defined(__AVX512BW__)
        simde__m512i perm2a = simde_mm512_set_epi32(30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2, 0);
        simde__m512i perm2b = simde_mm512_set_epi32(31, 29, 27, 25, 23, 21, 19, 17, 15, 13, 11, 9, 7, 5, 3, 1);
        for (; i + 32 <= end_symb; i += 32) {
          simde__m512i a = *(simde__m512i *)(mod + i);
          simde__m512i b = *(simde__m512i *)(mod + i + 16);
          *(simde__m512i *)tx0 = simde_mm512_permutex2var_epi32(a, perm2a, b);
          *(simde__m512i *)tx1 = simde_mm512_permutex2var_epi32(a, perm2b, b);
          tx0 += 16;
          tx1 += 16;
        }
#endif
#ifdef __AVX2__
        simde__m256i perm2 = simde_mm256_set_epi32(7, 5, 3, 1, 6, 4, 2, 0);
        for (; i + 8 <= end_symb; i += 8) {
          simde__m256i d = simde_mm256_permutevar8x32_epi32(*(simde__m256i *)(mod + i), perm2);
          *(simde__m128i *)tx0 = simde_mm256_extractf128_si256(d, 0);
          *(simde__m128i *)tx1 = simde_mm256_extractf128_si256(d, 1);
          tx0 += 4;
          tx1 += 4;
        }
#endif
        for (; i < end_symb; i += 2) {
          *tx0++ = mod[i];
          *tx1++ = mod[i + 1];
        }
      } break;

      case 3: {
        c16_t *tx0 = args->tx_layers[0] + out_idx;
        c16_t *tx1 = args->tx_layers[1] + out_idx;
        c16_t *tx2 = args->tx_layers[2] + out_idx;
        uint32_t i = start_symb;
#if defined(__AVX512BW__)
        simde__m512i perm3_0 = simde_mm512_set_epi32(13 + 16, 10 + 16, 7 + 16, 4 + 16, 1 + 16,
                                                     14 + 16, 11 + 16, 8 + 16, 5 + 16, 2 + 16,
                                                     15, 12, 9, 6, 3, 0);
        simde__m512i perm3_0b = simde_mm512_set_epi32(13 + 16, 10 + 16, 7 + 16, 4 + 16, 1 + 16, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
        simde__m512i perm3_1 = simde_mm512_set_epi32(14 + 16, 11 + 16, 8 + 16, 5 + 16, 2 + 16,
                                                     15 + 16, 12 + 16, 9 + 16, 6 + 16, 3 + 16, 0 + 16,
                                                     13, 10, 7, 4, 1);
        simde__m512i perm3_1b = simde_mm512_set_epi32(14 + 16, 11 + 16, 8 + 16, 5 + 16, 2 + 16, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
        simde__m512i perm3_2 = simde_mm512_set_epi32(15 + 16, 12 + 16, 9 + 16, 6 + 16, 3 + 16, 0 + 16,
                                                     13 + 16, 10 + 16, 7 + 16, 4 + 16, 1 + 16,
                                                     14, 11, 8, 5, 2);
        simde__m512i perm3_2b = simde_mm512_set_epi32(15 + 16, 12 + 16, 9 + 16, 6 + 16, 3 + 16, 0 + 16, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
        for (; i + 48 <= end_symb; i += 48) {
          simde__m512i i0 = *(simde__m512i *)(mod + i);
          simde__m512i i1 = *(simde__m512i *)(mod + i + 16);
          simde__m512i i2 = *(simde__m512i *)(mod + i + 32);
          simde__m512i d0 = simde_mm512_permutex2var_epi32(i0, perm3_0, i1);
          *(simde__m512i *)tx0 = simde_mm512_permutex2var_epi32(d0, perm3_0b, i2);
          tx0 += 16;
          d0 = simde_mm512_permutex2var_epi32(i0, perm3_1, i1);
          *(simde__m512i *)tx1 = simde_mm512_permutex2var_epi32(d0, perm3_1b, i2);
          tx1 += 16;
          d0 = simde_mm512_permutex2var_epi32(i0, perm3_2, i1);
          *(simde__m512i *)tx2 = simde_mm512_permutex2var_epi32(d0, perm3_2b, i2);
          tx2 += 16;
        }
#endif
#ifdef __AVX2__
        {
          simde__m256i perm3_0_avx2 = simde_mm256_set_epi32(5, 2, 7, 4, 1, 6, 3, 0);
          simde__m256i perm3_1_avx2 = simde_mm256_set_epi32(6, 3, 0, 5, 2, 7, 4, 1);
          simde__m256i perm3_2_avx2 = simde_mm256_set_epi32(7, 4, 1, 6, 3, 0, 5, 2);
          for (; i + 24 <= end_symb; i += 24) {
            simde__m256i i0 = *(simde__m256i *)(mod + i);
            simde__m256i i1 = *(simde__m256i *)(mod + i + 8);
            simde__m256i i2 = *(simde__m256i *)(mod + i + 16);
            simde__m256i d0 = simde_mm256_permutevar8x32_epi32(i0, perm3_0_avx2);
            simde__m256i d1 = simde_mm256_permutevar8x32_epi32(i1, perm3_0_avx2);
            simde__m256i d2 = simde_mm256_permutevar8x32_epi32(i2, perm3_0_avx2);
            simde__m256i d3 = simde_mm256_blend_epi32(d0, d1, 0x38);
            *(simde__m256i *)tx0 = simde_mm256_blend_epi32(d3, d2, 0xc0);
            tx0 += 8;
            d0 = simde_mm256_permutevar8x32_epi32(i0, perm3_1_avx2);
            d1 = simde_mm256_permutevar8x32_epi32(i1, perm3_1_avx2);
            d2 = simde_mm256_permutevar8x32_epi32(i2, perm3_1_avx2);
            d3 = simde_mm256_blend_epi32(d0, d1, 0x18);
            *(simde__m256i *)tx1 = simde_mm256_blend_epi32(d3, d2, 0xe0);
            tx1 += 8;
            d0 = simde_mm256_permutevar8x32_epi32(i0, perm3_2_avx2);
            d1 = simde_mm256_permutevar8x32_epi32(i1, perm3_2_avx2);
            d2 = simde_mm256_permutevar8x32_epi32(i2, perm3_2_avx2);
            d3 = simde_mm256_blend_epi32(d0, d1, 0x1c);
            *(simde__m256i *)tx2 = simde_mm256_blend_epi32(d3, d2, 0xe0);
            tx2 += 8;
          }
        }
#endif
        for (; i < end_symb; i += 3) {
          *tx0++ = mod[i];
          *tx1++ = mod[i + 1];
          *tx2++ = mod[i + 2];
        }
      } break;

      case 4: {
        c16_t *tx0 = args->tx_layers[0] + out_idx;
        c16_t *tx1 = args->tx_layers[1] + out_idx;
        c16_t *tx2 = args->tx_layers[2] + out_idx;
        c16_t *tx3 = args->tx_layers[3] + out_idx;
        uint32_t i = start_symb;
#if defined(__AVX512BW__)
        simde__m512i perm4 = simde_mm512_set_epi32(15, 11, 7, 3, 14, 10, 6, 2, 13, 9, 5, 1, 12, 8, 4, 0);
        for (; i + 16 <= end_symb; i += 16) {
          simde__m512i e = simde_mm512_permutexvar_epi32(perm4, *(simde__m512i *)(mod + i));
          *(simde__m128i *)tx0 = simde_mm512_extracti64x2_epi64(e, 0);
          tx0 += 4;
          *(simde__m128i *)tx1 = simde_mm512_extracti64x2_epi64(e, 1);
          tx1 += 4;
          *(simde__m128i *)tx2 = simde_mm512_extracti64x2_epi64(e, 2);
          tx2 += 4;
          *(simde__m128i *)tx3 = simde_mm512_extracti64x2_epi64(e, 3);
          tx3 += 4;
        }
#endif
#ifdef __AVX2__
        {
          simde__m256i perm4_avx2 = simde_mm256_set_epi32(7, 3, 6, 2, 5, 1, 4, 0);
          for (; i + 8 <= end_symb; i += 8) {
            simde__m256i e = simde_mm256_permutevar8x32_epi32(*(simde__m256i *)(mod + i), perm4_avx2);
            *(uint64_t *)tx0 = simde_mm256_extract_epi64(e, 0);
            tx0 += 2;
            *(uint64_t *)tx1 = simde_mm256_extract_epi64(e, 1);
            tx1 += 2;
            *(uint64_t *)tx2 = simde_mm256_extract_epi64(e, 2);
            tx2 += 2;
            *(uint64_t *)tx3 = simde_mm256_extract_epi64(e, 3);
            tx3 += 2;
          }
        }
#endif
        for (; i < end_symb; i += 4) {
          *tx0++ = mod[i];
          *tx1++ = mod[i + 1];
          *tx2++ = mod[i + 2];
          *tx3++ = mod[i + 3];
        }
      } break;

      default:
        break;
    }
  }
}

// Pre-created task set for layer mapping (reused across calls)
static enkiTaskSet* g_layer_mapping_task = NULL;

// Parallel layer mapping: splits work into symbol ranges for 10 ISIP threads
void nr_layer_mapping_parallel(int nbCodes,
                               int encoded_len,
                               c16_t mod_symbs[nbCodes][encoded_len],
                               uint8_t n_layers,
                               int layerSz,
                               uint32_t n_symbs,
                               c16_t tx_layers[][layerSz])
{
  void *scheduler = isip_pool_get_scheduler();

  // Minimum symbols to benefit from parallelization (overhead ~3-4µs)
  // For 4 layers, 2048 symbols = 512 symbols per layer = ~4KB per layer
  const uint32_t MIN_SYMBS_FOR_PARALLEL = 2048;

  if (!scheduler || n_symbs < MIN_SYMBS_FOR_PARALLEL || n_layers < 1 || n_layers > 4) {
    // Fallback to sequential
    nr_layer_mapping(nbCodes, encoded_len, mod_symbs, n_layers, layerSz, n_symbs, tx_layers);
    return;
  }

  // Create task set on first use
  if (!g_layer_mapping_task) {
    g_layer_mapping_task = enkiCreateTaskSet(scheduler, layer_mapping_range_task);
  }

  // Determine alignment requirement for AVX512/AVX2 SIMD:
  // Must be lcm(n_layers, SIMD_width) to ensure correct processing
  // AVX512: 32 symbols (case 2), 48 symbols (case 3), 16 symbols (case 4)
  // AVX2:   8 symbols (case 2), 24 symbols (case 3), 8 symbols (case 4)
  uint32_t alignment;
#if defined(__AVX512BW__)
  switch (n_layers) {
    case 1: alignment = 32; break;  // Cache line alignment
    case 2: alignment = 32; break;  // AVX512 processes 32 symbols
    case 3: alignment = 48; break;  // lcm(3, 16) = 48
    case 4: alignment = 16; break;  // AVX512 processes 16 symbols
    default: alignment = 32; break;
  }
#else
  switch (n_layers) {
    case 1: alignment = 8; break;   // Cache efficiency
    case 2: alignment = 8; break;   // AVX2 processes 8 symbols
    case 3: alignment = 24; break;  // lcm(3, 8) = 24
    case 4: alignment = 8; break;   // AVX2 processes 8 symbols
    default: alignment = 8; break;
  }
#endif

  // Determine number of tasks (aim for ~2048 symbols per task, max 16 tasks)
  const uint32_t SYMBS_PER_TASK = 2048;
  uint32_t num_tasks = (n_symbs + SYMBS_PER_TASK - 1) / SYMBS_PER_TASK;
  if (num_tasks > 16) num_tasks = 16;
  if (num_tasks < 1) num_tasks = 1;

  // Calculate symbols per task (must be multiple of alignment for SIMD)
  uint32_t symbs_per_task = (n_symbs + num_tasks - 1) / num_tasks;
  // Round up to multiple of alignment
  symbs_per_task = ((symbs_per_task + alignment - 1) / alignment) * alignment;

  // Prepare task arguments (stack allocated, safe for single-threaded caller)
  layer_mapping_task_args_t task_args[16];
  c16_t *mod = mod_symbs[0];

  uint32_t symb_offset = 0;
  uint32_t actual_tasks = 0;
  for (uint32_t t = 0; t < num_tasks && symb_offset < n_symbs; t++) {
    task_args[t].mod = mod;
    task_args[t].start_symb = symb_offset;
    task_args[t].end_symb = (symb_offset + symbs_per_task > n_symbs) ? n_symbs : (symb_offset + symbs_per_task);
    task_args[t].n_layers = n_layers;
    for (int l = 0; l < n_layers; l++) {
      task_args[t].tx_layers[l] = tx_layers[l];
    }
    symb_offset = task_args[t].end_symb;
    actual_tasks++;
  }

  // Execute parallel tasks (minRange=1 so each task is processed by one thread)
  enkiAddTaskSetMinRange(scheduler, g_layer_mapping_task, task_args, actual_tasks, 1);
  enkiWaitForTaskSet(scheduler, g_layer_mapping_task);
}
#else
// Fallback when ISIP not available
void nr_layer_mapping_parallel(int nbCodes,
                               int encoded_len,
                               c16_t mod_symbs[nbCodes][encoded_len],
                               uint8_t n_layers,
                               int layerSz,
                               uint32_t n_symbs,
                               c16_t tx_layers[][layerSz])
{
  nr_layer_mapping(nbCodes, encoded_len, mod_symbs, n_layers, layerSz, n_symbs, tx_layers);
}
#endif

void nr_ue_layer_mapping(const c16_t *mod_symbs, const int n_layers, const int n_symbs, c16_t tx_layers[][n_symbs])
{
  for (int l = 0; l < n_layers; l++) {
    for (int i = 0; i < n_symbs; i++) {
      tx_layers[l][i] = c16mulRealShift(mod_symbs[n_layers * i + l], AMP, 15);
    }
  }
}

void nr_dft(c16_t *z, c16_t *d, uint32_t Msc_PUSCH)
{
  simde__m128i dft_in128[3240], dft_out128[3240];
  c16_t *dft_in0 = (c16_t *)dft_in128, *dft_out0 = (c16_t *)dft_out128;

  uint32_t i, ip;

  simde__m128i norm128;

  if ((Msc_PUSCH % 1536) > 0) {
    for (i = 0, ip = 0; i < Msc_PUSCH; i++, ip += 4) {
      dft_in0[ip] = d[i];
    }
  }
  dft_size_idx_t dftsize = get_dft(Msc_PUSCH);
  switch (Msc_PUSCH) {
    case 12:
      dft(dftsize, (int16_t *)dft_in0, (int16_t *)dft_out0, 0);
      norm128 = simde_mm_set1_epi16(9459);
      for (i = 0; i < 12; i++) {
        ((simde__m128i *)dft_out0)[i] = simde_mm_slli_epi16(simde_mm_mulhi_epi16(((simde__m128i *)dft_out0)[i], norm128), 1);
      }

      break;
    default:
      dft(dftsize, (int16_t *)dft_in0, (int16_t *)dft_out0, 1);
      break;
  }

  if ((Msc_PUSCH % 1536) > 0) {
    for (i = 0, ip = 0; i < Msc_PUSCH; i++, ip += 4)
      z[i] = dft_out0[ip];
  }
}

void perform_symbol_rotation(NR_DL_FRAME_PARMS *fp, double f0, c16_t *symbol_rotation)
{
  const int nsymb = fp->symbols_per_slot * fp->slots_per_frame / 10;
  const double Tc = (1 / 480e3 / 4096);
  const double Nu = 2048 * 64 * (1 / (float)(1 << fp->numerology_index));
  const double Ncp0 = 16 * 64 + (144 * 64 * (1 / (float)(1 << fp->numerology_index)));
  const double Ncp1 = (144 * 64 * (1 / (float)(1 << fp->numerology_index)));

  LOG_D(PHY, "Doing symbol rotation calculation for TX/RX, f0 %f Hz, Nsymb %d\n", f0, nsymb);

  double tl = 0.0;
  double poff = 0.0;
  double exp_re = 0.0;
  double exp_im = 0.0;

  for (int l = 0; l < nsymb; l++) {
    double Ncp;
    if (l == 0 || l == (7 * (1 << fp->numerology_index))) {
      Ncp = Ncp0;
    } else {
      Ncp = Ncp1;
    }

    poff = 2 * M_PI * (tl + (Ncp * Tc)) * f0;
    exp_re = cos(poff);
    exp_im = sin(-poff);
    symbol_rotation[l].r = (int16_t)floor(exp_re * 32767);
    symbol_rotation[l].i = (int16_t)floor(exp_im * 32767);

    LOG_D(PHY,
          "Symbol rotation %d/%d => tl %f (%d,%d) (%f)\n",
          l,
          nsymb,
          tl,
          symbol_rotation[l].r,
          symbol_rotation[l].i,
          (poff / 2 / M_PI) - floor(poff / 2 / M_PI));

    tl += (Nu + Ncp) * Tc;
  }
}

void init_symbol_rotation(NR_DL_FRAME_PARMS *fp)
{
  double f[2] = {(double)fp->dl_CarrierFreq, (double)fp->ul_CarrierFreq};

  for (int ll = 0; ll < 2; ll++) {
    double f0 = f[ll];
    if (f0 == 0)
      continue;
    c16_t *rot = fp->symbol_rotation[ll];

    perform_symbol_rotation(fp, f0, rot);
  }
}

void init_timeshift_rotation(NR_DL_FRAME_PARMS *fp)
{
  const int sample_offset = fp->nb_prefix_samples / fp->ofdm_offset_divisor;
  for (int i = 0; i < fp->ofdm_symbol_size; i++) {
    double poff = -i * 2.0 * M_PI * sample_offset / fp->ofdm_symbol_size;
    double exp_re = cos(poff);
    double exp_im = sin(-poff);
    fp->timeshift_symbol_rotation[i].r = (int16_t)round(exp_re * 32767);
    fp->timeshift_symbol_rotation[i].i = (int16_t)round(exp_im * 32767);

    if (i < 10)
      LOG_D(PHY,
            "Timeshift symbol rotation %d => (%d,%d) %f\n",
            i,
            fp->timeshift_symbol_rotation[i].r,
            fp->timeshift_symbol_rotation[i].i,
            poff);
  }
}

c16_t nr_layer_precoder(int sz, c16_t datatx_F_precoding[][sz], const char *prec_matrix, uint8_t n_layers, int32_t re_offset)
{
  c16_t precodatatx_F = {0};

  for (int al = 0; al < n_layers; al++) {
    c16_t antenna = datatx_F_precoding[al][re_offset];
    switch (prec_matrix[al]) {
      case '0': // multiply by zero
        break;

      case '1': // multiply by 1
        precodatatx_F = c16add(precodatatx_F, antenna);
        break;

      case 'n': // multiply by -1
        precodatatx_F = c16sub(precodatatx_F, antenna);
        break;

      case 'j': //
        precodatatx_F.r -= antenna.i;
        precodatatx_F.i += antenna.r;
        break;

      case 'o': // -j
        precodatatx_F.r += antenna.i;
        precodatatx_F.i -= antenna.r;
        break;
    }
  }

  return precodatatx_F;
  // normalize
  /*  ((int16_t *)precodatatx_F)[0] = (int16_t)((((int16_t *)precodatatx_F)[0]*ONE_OVER_SQRT2_Q15)>>15);
      ((int16_t *)precodatatx_F)[1] = (int16_t)((((int16_t *)precodatatx_F)[1]*ONE_OVER_SQRT2_Q15)>>15);*/
}

c16_t nr_layer_precoder_cm(int n_layers,
                           int symSz,
                           c16_t datatx_F_precoding[n_layers][symSz],
                           int ap,
                           nfapi_nr_pm_pdu_t *pmi_pdu,
                           int offset)
{
  c16_t precodatatx_F = {0};
  for (int al = 0; al < n_layers; al++) {
    c16_t prec_weight = pmi_pdu->weights[al][ap];
    precodatatx_F = c16maddShift(datatx_F_precoding[al][offset], prec_weight, precodatatx_F, 15);
  }
  return precodatatx_F;
}

#if defined(__AVX512F__) && defined(__AVX512BW__)

static inline __attribute__((always_inline)) __m512i cmac0_prec512(__m512i x, __m512i w_c, __m512i w_s) {

      // Multiplication and shift
      const __m512i reals =
          _mm512_srai_epi32(_mm512_madd_epi16(x, w_c), 15); // (int32_t) .r = (x.r * w.r - x.i * w.i) >> 15
      const __m512i imags =
          _mm512_slli_epi32(_mm512_madd_epi16(x, w_s),  1); // (int32_t) .i = (x.r * w.i + x.i * w.r) << 1, since higher 16 bit of each 32 bit is taken by blend_epi16

      // Re-arrange to match c16_t format
      return _mm512_mask_blend_epi16(0xAAAAAAAA,reals, imags);

}
static inline __attribute__((always_inline)) __m512i cmac_prec512(__m512i y, __m512i x, __m512i w_c, __m512i w_s) {
  const __m512i produ = cmac0_prec512(x, w_c, w_s);
  // Accumulate the product
  return _mm512_adds_epi16(y, produ);
}
#endif
#ifdef __AVX2__
static inline __attribute__((always_inline)) __m256i cmac0_prec256(__m256i x, __m256i w_c, __m256i w_s) {

      // Multiplication and shift
      const __m256i reals =
          _mm256_srai_epi32(_mm256_madd_epi16(x, w_c), 15); // (int32_t) .r = (x.r * w.r - x.i * w.i) >> 15
      const __m256i imags =
          _mm256_slli_epi32(_mm256_madd_epi16(x, w_s),  1); // (int32_t) .i = (x.r * w.i + x.i * w.r) << 1, since higher 16 bit of each 32 bit is taken by blend_epi16

      // Re-arrange to match c16_t format
      return _mm256_blend_epi16(reals, imags,0xAA);

}
static inline __attribute__((always_inline)) __m256i cmac_prec256(__m256i y, __m256i x, __m256i w_c, __m256i w_s) {
  const __m256i produ = cmac0_prec256(x, w_c, w_s);
  // Accumulate the product
  return _mm256_adds_epi16(y, produ);
}
#endif
#ifdef __aarch64__
static inline __attribute__((always_inline)) int16x8_t cmac0_prec128(int16x8_t x, int16x8_t wr, int16x8_t wi) {
    //
    int16x8_t xr = vuzp1q_s16(x, x);  // even lanes
    int16x8_t xi = vuzp2q_s16(x, x);  // odd  lanes
    // real = ar*br - ai*bi  (Q15 scaling via high-half doubling muls)
    int16x8_t real = vqdmulhq_s16(xr, wr);      // ≈ round((2*xr*wr)/2^16)
    real = vqrdmlshq_s16(real, xi, wi);         // real -= round((2*xi*wi)/2^16)
    //
    // imag = ar*bi + ai*br
    int16x8_t imag = vqdmulhq_s16(xr, wi);
    imag = vqrdmlahq_s16(imag, xi, wr);         // imag += round((2*xi*wr)/2^16)
    //
    // Re-interleave [real, imag]
    int16x8x2_t produ = vzipq_s16(real, imag);
    return produ.val[0];        
}
static inline __attribute__((always_inline)) int16x8_t cmac_prec128(int16x8_t y, int16x8_t x, int16x8_t wr, int16x8_t wi) {
  int16x8_t produ = cmac0_prec128(x, wr, wi);
  return vaddq_s16(y, produ);
}
#else
static inline __attribute__((always_inline)) simde__m128i cmac0_prec128(simde__m128i x, simde__m128i w_c, simde__m128i w_s)
{
  // Multiplication and shift
  const simde__m128i reals = simde_mm_srai_epi32(simde_mm_madd_epi16(x, w_c), 15); // (int32_t) .r = (x.r * w.r - x.i * w.i) >> 15
  const simde__m128i imags = simde_mm_slli_epi32(
      simde_mm_madd_epi16(x, w_s),
      1); // (int32_t) .i = (x.r * w.i + x.i * w.r) << 1, since higher 16 bit of each 32 bit is taken by blend_epi16

  /* Re-arrange to match c16_t format
     bit index: 0            | 16              | 32           | 48              | 64           | 80              | 96 | 112
     reals =   {R0.r[15..30] | R0.r[31] (0)*15 | R1.r[15..30] | R1.r[31] (0)*15 | R2.r[15..30] | R2.r[31] (0)*15 | R3.r[15..30]
     | R3.r[31] (0)*15} imags =   {0 R0.i[0..14]| R0.i[15..30]    | 0 R1.i[0..14]| R1.i[15..30]    | 0 R2.i[0..14]| R2.i[15..30]
     | 0 R3.i[0..14]| R3.i[15..30]   } 16b from  {reals        | imags           | reals        | imags | reals | imags | reals
     | imags          } produ =   {R0.r[15..30] | R0.i[15..30]    | R1.r[15..30] | R1.i[15..30] | R2.r[15..30] | R2.i[15..30] |
     R3.r[15..30] | R3.i[15..30]   }
  */
  return simde_mm_blend_epi16(reals, imags, 0xAA);
}
static inline __attribute__((always_inline)) __m128i cmac_prec128(__m128i y, __m128i x, __m128i w_c, __m128i w_s)
{
  const __m128i produ = cmac0_prec128(x, w_c, w_s);
  // Accumulate the product
  return simde_mm_adds_epi16(y, produ);
}
#endif

#define load_consts(Type, Instruct, Rank)                                          \
  const Type w_c##Rank = Instruct(c16toI32(c16conj(pmi_pdu->weights[Rank][ant]))); \
  const Type w_s##Rank = Instruct(c16toI32(c16swap(pmi_pdu->weights[Rank][ant]))); \
  const Type *in##Rank = (Type *)(txdataF_res_mapped[Rank] + sc_offset + (out-beginning));

void nr_layer_precoder_simd(const int n_layers,
                            const int symSz,
                            const c16_t txdataF_res_mapped[n_layers][symSz],
                            const int ant,
                            const nfapi_nr_pm_pdu_t *pmi_pdu,
                            const int sc_offset,
                            const int re_cnt,
                            c16_t *txdataF_precoded)
{
  // For x86, use 256 SIMD for every 8 RE and 128 SIMD for last 4 RE
  // For aarch64, use 128 SIMD for every 4 RE
  AssertFatal(n_layers > 0 && n_layers <= 4, "Shouldn't get here, n_layers %d\n", n_layers);

  // 512/256 SIMD: Do 16/8 RE in one iteration, 3 iterations for 2 RB
  c16_t *beginning = txdataF_precoded + sc_offset;
  c16_t *out=beginning;
#if defined(__AVX512F__) && defined(__AVX512BW__)
  {
    c16_t *end = out + (re_cnt & ~15);
    load_consts(__m512i, _mm512_set1_epi32, 0);
    if (n_layers == 1) {
      for (; out < end; out += sizeof(__m512i) / sizeof(*out)) {
        const __m512i x = _mm512_loadu_si512(in0++);
        // Matrix multiplication for 4 elements of the result (sizeof(simde__m256i) / sizeof(*prec_matrix) = 8)
        __m512i y = cmac0_prec512(x, w_c0, w_s0);
        _mm512_storeu_si512(out, y);
      }
    } else if (n_layers == 2) {
      load_consts(__m512i, _mm512_set1_epi32, 1);
      for (; out < end; out += sizeof(__m512i) / sizeof(*out)) {
        const __m512i x = _mm512_loadu_si512(in0++);
        const __m512i x1 = _mm512_loadu_si512(in1++);
        // Matrix multiplication for 4 elements of the result (sizeof(simde__m256i) / sizeof(*prec_matrix) = 8)
        __m512i y = cmac0_prec512(x, w_c0, w_s0);
        y = cmac_prec512(y, x1, w_c1, w_s1);
        _mm512_storeu_si512(out, y);
      }
    } else if (n_layers == 3) {
      load_consts(__m512i, _mm512_set1_epi32, 1);
      load_consts(__m512i, _mm512_set1_epi32, 2);
      for (; out < end; out += sizeof(__m512i) / sizeof(*out)) {
        const __m512i x = _mm512_loadu_si512(in0++);
        const __m512i x1 = _mm512_loadu_si512(in1++);
        const __m512i x2 = _mm512_loadu_si512(in2++);
        // Matrix multiplication for 4 elements of the result (sizeof(simde__m256i) / sizeof(*prec_matrix) = 8)
        __m512i y = cmac0_prec512(x, w_c0, w_s0);
        y = cmac_prec512(y, x1, w_c1, w_s1);
        y = cmac_prec512(y, x2, w_c2, w_s2);
        _mm512_storeu_si512(out, y);
      }
    } else if (n_layers == 4) {
      load_consts(__m512i, _mm512_set1_epi32, 1);
      load_consts(__m512i, _mm512_set1_epi32, 2);
      load_consts(__m512i, _mm512_set1_epi32, 3);
      for (; out < end; out += sizeof(__m512i) / sizeof(*out)) {
        const __m512i x = _mm512_loadu_si512(in0++);
        const __m512i x1 = _mm512_loadu_si512(in1++);
        const __m512i x2 = _mm512_loadu_si512(in2++);
        const __m512i x3 = _mm512_loadu_si512(in3++);
        // Matrix multiplication for 4 elements of the result (sizeof(simde__m256i) / sizeof(*prec_matrix) = 8)
        __m512i y = cmac0_prec512(x, w_c0, w_s0);
        y = cmac_prec512(y, x1, w_c1, w_s1);
        y = cmac_prec512(y, x2, w_c2, w_s2);
        y = cmac_prec512(y, x3, w_c3, w_s3);
        _mm512_storeu_si512(out, y);
      }
    }
  }
#endif
#ifdef __AVX2__
  {
    c16_t *end = beginning + (re_cnt & ~7);
    load_consts(simde__m256i, simde_mm256_set1_epi32, 0);
    if (n_layers == 1) {
      for (; out < end; out += sizeof(simde__m256i) / sizeof(*out)) {
        const simde__m256i x0 = simde_mm256_loadu_si256(in0++);
        // Accumulate the product
        simde__m256i y = cmac0_prec256(x0, w_c0, w_s0);
        // Store the result to txdataF
        simde_mm256_storeu_si256(out, y);
      }
    } else if (n_layers == 2) {
      load_consts(simde__m256i, simde_mm256_set1_epi32, 1);
      for (; out < end; out += sizeof(simde__m256i) / sizeof(*out)) {
        const simde__m256i x0 = simde_mm256_loadu_si256(in0++);
        const simde__m256i x1 = simde_mm256_loadu_si256(in1++);
        // Accumulate the product
        simde__m256i y = cmac0_prec256(x0, w_c0, w_s0);
        y = cmac_prec256(y, x1, w_c1, w_s1);
        // Store the result to txdataF
        simde_mm256_storeu_si256(out, y);
      }
    } else if (n_layers == 3) {
      load_consts(simde__m256i, simde_mm256_set1_epi32, 1);
      load_consts(simde__m256i, simde_mm256_set1_epi32, 2);
      for (; out < end; out += sizeof(simde__m256i) / sizeof(*out)) {
        const simde__m256i x0 = simde_mm256_loadu_si256(in0++);
        const simde__m256i x1 = simde_mm256_loadu_si256(in1++);
        const simde__m256i x2 = simde_mm256_loadu_si256(in2++);
        simde__m256i y = cmac0_prec256(x0, w_c0, w_s0);
        y = cmac_prec256(y, x1, w_c1, w_s1);
        y = cmac_prec256(y, x2, w_c2, w_s2);
        // Store the result to txdataF
        simde_mm256_storeu_si256(out, y);
      }
    } else if (n_layers == 4) {
      load_consts(simde__m256i, simde_mm256_set1_epi32, 1);
      load_consts(simde__m256i, simde_mm256_set1_epi32, 2);
      load_consts(simde__m256i, simde_mm256_set1_epi32, 3);
      for (; out < end; out += sizeof(simde__m256i) / sizeof(*out)) {
        const simde__m256i x0 = simde_mm256_loadu_si256(in0++);
        const simde__m256i x1 = simde_mm256_loadu_si256(in1++);
        const simde__m256i x2 = simde_mm256_loadu_si256(in2++);
        const simde__m256i x3 = simde_mm256_loadu_si256(in3++);
        simde__m256i y = cmac0_prec256(x0, w_c0, w_s0);
        y = cmac_prec256(y, x1, w_c1, w_s1);
        y = cmac_prec256(y, x2, w_c2, w_s2);
        y = cmac_prec256(y, x3, w_c3, w_s3);
        // Store the result to txdataF
        simde_mm256_storeu_si256(out, y);
      }
    }
  }
#endif
  c16_t *end = beginning + (re_cnt & ~3);
#ifdef DEBUG_DLSCH_PRECODING_PRINT_WITH_TRIVIAL // Get result with trivial solution, TODO: To be removed
  // 128 SIMD: Do 4 RE in one iteration, 3 iterations for 1 RB
  for (; out < end; out += sizeof(simde__m128i) / sizeof(*out)) {
    c16_t y_triv[4];
    for (int i = 0; i < 4; i++)
      y_triv[i] = nr_layer_precoder_cm(n_layers, symSz, txdataF_res_mapped, ant, pmi_pdu, sc + i);
    memcpy(out, y_triv, sizeof(y_triv));
  }
#endif
#ifdef __aarch64__
  load_consts(int16x8_t, vdupq_n_s16, 0);
  if (n_layers == 1) {
    for (; out < end; out += sizeof(int16x8_t) / sizeof(*out)) {
      const int16x8_t x0 = vld1q_s16((const int16_t *)in0++);
      // Accumulate the product
      int16x8_t y = cmac0_prec128(x0, w_c0, w_s0);
      // Store the result to txdataF
      *(int16x8_t *)out = y;
    }
  }
  if (n_layers == 2) {
    load_consts(int16x8_t, vdupq_n_s16, 1);
    for (; out < end; out += sizeof(int16x8_t) / sizeof(*out)) {
      const int16x8_t x0 = vld1q_s16((const int16_t *)in0++);
      const int16x8_t x1 = vld1q_s16((const int16_t *)in1++);
      // Accumulate the product
      int16x8_t y = cmac0_prec128(x0, w_c0, w_s0);
      y = cmac_prec128(y, x1, w_c1, w_s1);
      // Store the result to txdataF
      *(int16x8_t *)out = y;
    }
  }
  if (n_layers == 3) {
    load_consts(int16x8_t, vdupq_n_s16, 1);
    load_consts(int16x8_t, vdupq_n_s16, 2);
    for (; out < end; out += sizeof(int16x8_t) / sizeof(*out)) {
      const int16x8_t x0 = vld1q_s16((const int16_t *)in0++);
      const int16x8_t x1 = vld1q_s16((const int16_t *)in1++);
      const int16x8_t x2 = vld1q_s16((const int16_t *)in2++);
      // Accumulate the product
      int16x8_t y = cmac0_prec128(x0, w_c0, w_s0);
      ;
      y = cmac_prec128(y, x1, w_c1, w_s1);
      y = cmac_prec128(y, x2, w_c2, w_s2);
      // Store the result to txdataF
      *(int16x8_t *)out = y;
    }
  }
  if (n_layers == 4) {
    load_consts(int16x8_t, vdupq_n_s16, 1);
    load_consts(int16x8_t, vdupq_n_s16, 2);
    load_consts(int16x8_t, vdupq_n_s16, 3);
    for (; out < end; out += sizeof(int16x8_t) / sizeof(*out)) {
      const int16x8_t x0 = vld1q_s16((const int16_t *)in0++);
      const int16x8_t x1 = vld1q_s16((const int16_t *)in1++);
      const int16x8_t x2 = vld1q_s16((const int16_t *)in2++);
      const int16x8_t x3 = vld1q_s16((const int16_t *)in3++);
      // Accumulate the product
      int16x8_t y = cmac0_prec128(x0, w_c0, w_s0);
      ;
      y = cmac_prec128(y, x1, w_c1, w_s1);
      y = cmac_prec128(y, x2, w_c2, w_s2);
      y = cmac_prec128(y, x3, w_c3, w_s3);
      // Store the result to txdataF
      *(int16x8_t *)out = y;
    }
  }
#else
  load_consts(simde__m128i, simde_mm_set1_epi32, 0);
  if (n_layers == 1) {
    for (; out < end; out += sizeof(simde__m128i) / sizeof(*out)) {
      const simde__m128i x0 = simde_mm_loadu_si128(in0++);
      // Accumulate the product
      simde__m128i y = cmac0_prec128(x0, w_c0, w_s0);
      // Store the result to txdataF
      simde_mm_storeu_si128(out, y);
    }
  } else if (n_layers == 2) {
    load_consts(simde__m128i, simde_mm_set1_epi32, 1);
    for (; out < end; out += sizeof(simde__m128i) / sizeof(*out)) {
      const simde__m128i x0 = simde_mm_loadu_si128(in0++);
      const simde__m128i x1 = simde_mm_loadu_si128(in1++);
      // Accumulate the product
      simde__m128i y = cmac0_prec128(x0, w_c0, w_s0);
      y = cmac_prec128(y, x1, w_c1, w_s1);
      // Store the result to txdataF
      simde_mm_storeu_si128(out, y);
    }
  } else if (n_layers == 3) {
    load_consts(simde__m128i, simde_mm_set1_epi32, 1);
    load_consts(simde__m128i, simde_mm_set1_epi32, 2);
    for (; out < end; out += sizeof(simde__m128i) / sizeof(*out)) {
      const simde__m128i x0 = simde_mm_loadu_si128(in0++);
      const simde__m128i x1 = simde_mm_loadu_si128(in1++);
      const simde__m128i x2 = simde_mm_loadu_si128(in2++);
      simde__m128i y = cmac0_prec128(x0, w_c0, w_s0);
      y = cmac_prec128(y, x1, w_c1, w_s1);
      y = cmac_prec128(y, x2, w_c2, w_s2);
      // Store the result to txdataF
      simde_mm_storeu_si128(out, y);
    }
  } else if (n_layers == 4) {
    load_consts(simde__m128i, simde_mm_set1_epi32, 1);
    load_consts(simde__m128i, simde_mm_set1_epi32, 2);
    load_consts(simde__m128i, simde_mm_set1_epi32, 3);
    for (; out < end; out += sizeof(simde__m128i) / sizeof(*out)) {
      const simde__m128i x0 = simde_mm_loadu_si128(in0++);
      const simde__m128i x1 = simde_mm_loadu_si128(in1++);
      const simde__m128i x2 = simde_mm_loadu_si128(in2++);
      const simde__m128i x3 = simde_mm_loadu_si128(in3++);
      simde__m128i y = cmac0_prec128(x0, w_c0, w_s0);
      y = cmac_prec128(y, x1, w_c1, w_s1);
      y = cmac_prec128(y, x2, w_c2, w_s2);
      y = cmac_prec128(y, x3, w_c3, w_s3);
      // Store the result to txdataF
      simde_mm_storeu_si128(out, y);
    }
  }
#endif
#ifdef DEBUG_DLSCH_PRECODING_PRINT_WITH_TRIVIAL // Print simd and trivial result, TODO: To be removed
  c16_t *y_simd = (c16_t *)&y;
  printf("debug_to_be_removed re_cnt=%d, sc=%u, y_simd=(%+4d,%+4d), (%+4d,%+4d), (%+4d,%+4d), (%+4d,%+4d)\n",
         re_cnt,
         sc,
         y_simd[0].r,
         y_simd[0].i,
         y_simd[1].r,
         y_simd[1].i,
         y_simd[2].r,
         y_simd[2].i,
         y_simd[3].r,
         y_simd[3].i);
  printf("debug_to_be_removed re_cnt=%d, sc=%u, y_triv=(%+4d,%+4d), (%+4d,%+4d), (%+4d,%+4d), (%+4d,%+4d)\n",
         re_cnt,
         sc,
         y_triv[0].r,
         y_triv[0].i,
         y_triv[1].r,
         y_triv[1].i,
         y_triv[2].r,
         y_triv[2].i,
         y_triv[3].r,
         y_triv[3].i);
#endif
}

/*! \brief Fused modulation + layer mapping for 256QAM with AVX2 gather optimization
 *  Takes already-scrambled data and performs modulation + layer mapping in one pass.
 *  Eliminates the intermediate mod_symbs buffer (~98KB for 273 RB).
 *  Uses AVX2 gather instructions for vectorized table lookups.
 *
 *  @param[in]  scrambled_data Already scrambled bytes (after nr_codeword_scrambling)
 *  @param[in]  n_symbols      Number of modulated symbols to produce
 *  @param[in]  n_layers       Number of layers (1, 2, 3, or 4)
 *  @param[in]  layerSz        Size of each layer buffer
 *  @param[out] tx_layers      Output layer buffers [n_layers][layerSz]
 */
void nr_modulate_layer_map_256qam(const uint8_t *scrambled_data,
                                   uint32_t n_symbols,
                                   uint8_t n_layers,
                                   int layerSz,
                                   c16_t tx_layers[][layerSz])
{
  const int32_t *table = nr_256qam_mod_table;
  int32_t *out0 = (int32_t *)tx_layers[0];
  uint32_t i = 0;
  uint32_t out_idx = 0;

  switch (n_layers) {
    case 1: {
      // Single layer: direct table lookup with AVX2 gather
#ifdef __AVX2__
      // AVX2: gather 8 table entries at once
      for (; i + 8 <= n_symbols; i += 8) {
        // Load 8 bytes and expand to 32-bit indices
        simde__m128i bytes = simde_mm_loadl_epi64((simde__m128i *)(scrambled_data + i));
        simde__m256i indices = simde_mm256_cvtepu8_epi32(bytes);
        // Gather 8 table entries
        simde__m256i results = simde_mm256_i32gather_epi32(table, indices, 4);
        // Store 8 results
        simde_mm256_storeu_si256((simde__m256i *)(out0 + i), results);
      }
#endif
      // Scalar cleanup
      for (; i < n_symbols; i++) {
        out0[i] = table[scrambled_data[i]];
      }
    } break;

    case 2: {
      // 2 layers: gather then deinterleave
      int32_t *out1 = (int32_t *)tx_layers[1];
#ifdef __AVX2__
      // Process 16 input bytes (8 outputs per layer) at a time
      for (; i + 16 <= n_symbols; i += 16) {
        // Load 16 bytes and expand to indices
        simde__m128i bytes0 = simde_mm_loadl_epi64((simde__m128i *)(scrambled_data + i));
        simde__m128i bytes1 = simde_mm_loadl_epi64((simde__m128i *)(scrambled_data + i + 8));
        simde__m256i idx0 = simde_mm256_cvtepu8_epi32(bytes0);
        simde__m256i idx1 = simde_mm256_cvtepu8_epi32(bytes1);
        // Gather 16 table entries
        simde__m256i res0 = simde_mm256_i32gather_epi32(table, idx0, 4);
        simde__m256i res1 = simde_mm256_i32gather_epi32(table, idx1, 4);
        // Deinterleave: res0 has [0,1,2,3,4,5,6,7], res1 has [8,9,10,11,12,13,14,15]
        // We want layer0: [0,2,4,6,8,10,12,14], layer1: [1,3,5,7,9,11,13,15]
        // Use shuffle to separate even/odd
        simde__m256i shuf_even = simde_mm256_permutevar8x32_epi32(res0, simde_mm256_set_epi32(6, 4, 2, 0, 6, 4, 2, 0));
        simde__m256i shuf_odd  = simde_mm256_permutevar8x32_epi32(res0, simde_mm256_set_epi32(7, 5, 3, 1, 7, 5, 3, 1));
        simde__m256i shuf_even1 = simde_mm256_permutevar8x32_epi32(res1, simde_mm256_set_epi32(6, 4, 2, 0, 6, 4, 2, 0));
        simde__m256i shuf_odd1  = simde_mm256_permutevar8x32_epi32(res1, simde_mm256_set_epi32(7, 5, 3, 1, 7, 5, 3, 1));
        // Combine: low 128-bits from first, high 128-bits from second
        simde__m256i layer0 = simde_mm256_permute2x128_si256(shuf_even, shuf_even1, 0x20);
        simde__m256i layer1 = simde_mm256_permute2x128_si256(shuf_odd, shuf_odd1, 0x20);
        // Store
        simde_mm256_storeu_si256((simde__m256i *)(out0 + out_idx), layer0);
        simde_mm256_storeu_si256((simde__m256i *)(out1 + out_idx), layer1);
        out_idx += 8;
      }
#endif
      // Scalar cleanup
      for (; i < n_symbols; i += 2) {
        out0[out_idx] = table[scrambled_data[i]];
        out1[out_idx] = table[scrambled_data[i + 1]];
        out_idx++;
      }
    } break;

    case 3: {
      // 3 layers: symbols 0,1,2 → layers 0,1,2
      // AVX2 gather is less efficient for 3-layer due to non-power-of-2 stride
      int32_t *out1 = (int32_t *)tx_layers[1];
      int32_t *out2 = (int32_t *)tx_layers[2];
#ifdef __AVX2__
      // Process 24 symbols (8 outputs per layer) at a time
      for (; i + 24 <= n_symbols; i += 24) {
        // Load indices for each layer using strided access
        simde__m256i idx0 = simde_mm256_set_epi32(
            scrambled_data[i + 21], scrambled_data[i + 18], scrambled_data[i + 15], scrambled_data[i + 12],
            scrambled_data[i + 9],  scrambled_data[i + 6],  scrambled_data[i + 3],  scrambled_data[i]);
        simde__m256i idx1 = simde_mm256_set_epi32(
            scrambled_data[i + 22], scrambled_data[i + 19], scrambled_data[i + 16], scrambled_data[i + 13],
            scrambled_data[i + 10], scrambled_data[i + 7],  scrambled_data[i + 4],  scrambled_data[i + 1]);
        simde__m256i idx2 = simde_mm256_set_epi32(
            scrambled_data[i + 23], scrambled_data[i + 20], scrambled_data[i + 17], scrambled_data[i + 14],
            scrambled_data[i + 11], scrambled_data[i + 8],  scrambled_data[i + 5],  scrambled_data[i + 2]);
        // Gather
        simde__m256i res0 = simde_mm256_i32gather_epi32(table, idx0, 4);
        simde__m256i res1 = simde_mm256_i32gather_epi32(table, idx1, 4);
        simde__m256i res2 = simde_mm256_i32gather_epi32(table, idx2, 4);
        // Store
        simde_mm256_storeu_si256((simde__m256i *)(out0 + out_idx), res0);
        simde_mm256_storeu_si256((simde__m256i *)(out1 + out_idx), res1);
        simde_mm256_storeu_si256((simde__m256i *)(out2 + out_idx), res2);
        out_idx += 8;
      }
#endif
      // Scalar cleanup
      for (; i < n_symbols; i += 3) {
        out0[out_idx] = table[scrambled_data[i]];
        out1[out_idx] = table[scrambled_data[i + 1]];
        out2[out_idx] = table[scrambled_data[i + 2]];
        out_idx++;
      }
    } break;

    case 4: {
      // 4 layers: gather then deinterleave 4-way
      int32_t *out1 = (int32_t *)tx_layers[1];
      int32_t *out2 = (int32_t *)tx_layers[2];
      int32_t *out3 = (int32_t *)tx_layers[3];
#ifdef __AVX2__
      // Process 32 symbols (8 outputs per layer) at a time
      for (; i + 32 <= n_symbols; i += 32) {
        // Load 32 bytes as 4 groups of 8
        simde__m128i b0 = simde_mm_loadl_epi64((simde__m128i *)(scrambled_data + i));
        simde__m128i b1 = simde_mm_loadl_epi64((simde__m128i *)(scrambled_data + i + 8));
        simde__m128i b2 = simde_mm_loadl_epi64((simde__m128i *)(scrambled_data + i + 16));
        simde__m128i b3 = simde_mm_loadl_epi64((simde__m128i *)(scrambled_data + i + 24));
        simde__m256i idx0 = simde_mm256_cvtepu8_epi32(b0);
        simde__m256i idx1 = simde_mm256_cvtepu8_epi32(b1);
        simde__m256i idx2 = simde_mm256_cvtepu8_epi32(b2);
        simde__m256i idx3 = simde_mm256_cvtepu8_epi32(b3);
        // Gather
        simde__m256i r0 = simde_mm256_i32gather_epi32(table, idx0, 4);
        simde__m256i r1 = simde_mm256_i32gather_epi32(table, idx1, 4);
        simde__m256i r2 = simde_mm256_i32gather_epi32(table, idx2, 4);
        simde__m256i r3 = simde_mm256_i32gather_epi32(table, idx3, 4);
        // r0=[0,1,2,3,4,5,6,7], r1=[8,9,10,11,12,13,14,15], etc.
        // Layer 0 wants: [0,4,8,12,16,20,24,28]
        // Layer 1 wants: [1,5,9,13,17,21,25,29]
        // etc.
        // Transpose 4x8 -> 8x4 using unpack and permute
        simde__m256i t0 = simde_mm256_unpacklo_epi32(r0, r1);  // [0,8,1,9,4,12,5,13]
        simde__m256i t1 = simde_mm256_unpackhi_epi32(r0, r1);  // [2,10,3,11,6,14,7,15]
        simde__m256i t2 = simde_mm256_unpacklo_epi32(r2, r3);  // [16,24,17,25,20,28,21,29]
        simde__m256i t3 = simde_mm256_unpackhi_epi32(r2, r3);  // [18,26,19,27,22,30,23,31]
        simde__m256i u0 = simde_mm256_unpacklo_epi64(t0, t2);  // [0,8,16,24,4,12,20,28]
        simde__m256i u1 = simde_mm256_unpackhi_epi64(t0, t2);  // [1,9,17,25,5,13,21,29]
        simde__m256i u2 = simde_mm256_unpacklo_epi64(t1, t3);  // [2,10,18,26,6,14,22,30]
        simde__m256i u3 = simde_mm256_unpackhi_epi64(t1, t3);  // [3,11,19,27,7,15,23,31]
        // Final permute to get consecutive order
        simde__m256i perm = simde_mm256_set_epi32(7, 3, 6, 2, 5, 1, 4, 0);
        simde__m256i layer0_v = simde_mm256_permutevar8x32_epi32(u0, perm);
        simde__m256i layer1_v = simde_mm256_permutevar8x32_epi32(u1, perm);
        simde__m256i layer2_v = simde_mm256_permutevar8x32_epi32(u2, perm);
        simde__m256i layer3_v = simde_mm256_permutevar8x32_epi32(u3, perm);
        // Store
        simde_mm256_storeu_si256((simde__m256i *)(out0 + out_idx), layer0_v);
        simde_mm256_storeu_si256((simde__m256i *)(out1 + out_idx), layer1_v);
        simde_mm256_storeu_si256((simde__m256i *)(out2 + out_idx), layer2_v);
        simde_mm256_storeu_si256((simde__m256i *)(out3 + out_idx), layer3_v);
        out_idx += 8;
      }
#endif
      // Scalar cleanup
      for (; i < n_symbols; i += 4) {
        out0[out_idx] = table[scrambled_data[i]];
        out1[out_idx] = table[scrambled_data[i + 1]];
        out2[out_idx] = table[scrambled_data[i + 2]];
        out3[out_idx] = table[scrambled_data[i + 3]];
        out_idx++;
      }
    } break;

    default:
      AssertFatal(false, "Unsupported number of layers %d for fused 256QAM mod+layer\n", n_layers);
  }
}

// ========== Parallel 256QAM Modulation + Layer Mapping using ISIP ==========

#if ISIP_AVAILABLE

// Task arguments for parallel modulation
typedef struct {
  const uint8_t *scrambled_data;  // Input data
  const int32_t *table;           // Modulation table
  int32_t *out[4];                // Output layer pointers (max 4 layers)
  uint8_t n_layers;               // Number of layers
} mod_256qam_task_args_t;

// ISIP task function: process a range of output indices with AVX2 gather
static void mod_256qam_task(uint32_t start, uint32_t end, uint32_t threadNum, void* pArgs)
{
  mod_256qam_task_args_t *args = (mod_256qam_task_args_t*)pArgs;
  const uint8_t *in = args->scrambled_data;
  const int32_t *table = args->table;
  const uint8_t n_layers = args->n_layers;

  switch (n_layers) {
    case 1: {
      int32_t *out0 = args->out[0];
      uint32_t i = start;
#ifdef __AVX2__
      // AVX2: gather 8 table entries at once
      for (; i + 8 <= end; i += 8) {
        simde__m128i bytes = simde_mm_loadl_epi64((simde__m128i *)(in + i));
        simde__m256i indices = simde_mm256_cvtepu8_epi32(bytes);
        simde__m256i results = simde_mm256_i32gather_epi32(table, indices, 4);
        simde_mm256_storeu_si256((simde__m256i *)(out0 + i), results);
      }
#endif
      for (; i < end; i++) {
        out0[i] = table[in[i]];
      }
    } break;

    case 2: {
      int32_t *out0 = args->out[0];
      int32_t *out1 = args->out[1];
      uint32_t out_idx = start;
      uint32_t in_idx = start * 2;
#ifdef __AVX2__
      // Process 8 outputs at a time (16 inputs)
      for (; out_idx + 8 <= end; out_idx += 8, in_idx += 16) {
        simde__m128i bytes0 = simde_mm_loadl_epi64((simde__m128i *)(in + in_idx));
        simde__m128i bytes1 = simde_mm_loadl_epi64((simde__m128i *)(in + in_idx + 8));
        simde__m256i idx0 = simde_mm256_cvtepu8_epi32(bytes0);
        simde__m256i idx1 = simde_mm256_cvtepu8_epi32(bytes1);
        simde__m256i res0 = simde_mm256_i32gather_epi32(table, idx0, 4);
        simde__m256i res1 = simde_mm256_i32gather_epi32(table, idx1, 4);
        // Deinterleave even/odd
        simde__m256i shuf_even = simde_mm256_permutevar8x32_epi32(res0, simde_mm256_set_epi32(6, 4, 2, 0, 6, 4, 2, 0));
        simde__m256i shuf_odd  = simde_mm256_permutevar8x32_epi32(res0, simde_mm256_set_epi32(7, 5, 3, 1, 7, 5, 3, 1));
        simde__m256i shuf_even1 = simde_mm256_permutevar8x32_epi32(res1, simde_mm256_set_epi32(6, 4, 2, 0, 6, 4, 2, 0));
        simde__m256i shuf_odd1  = simde_mm256_permutevar8x32_epi32(res1, simde_mm256_set_epi32(7, 5, 3, 1, 7, 5, 3, 1));
        simde__m256i layer0 = simde_mm256_permute2x128_si256(shuf_even, shuf_even1, 0x20);
        simde__m256i layer1 = simde_mm256_permute2x128_si256(shuf_odd, shuf_odd1, 0x20);
        simde_mm256_storeu_si256((simde__m256i *)(out0 + out_idx), layer0);
        simde_mm256_storeu_si256((simde__m256i *)(out1 + out_idx), layer1);
      }
#endif
      for (; out_idx < end; out_idx++, in_idx += 2) {
        out0[out_idx] = table[in[in_idx]];
        out1[out_idx] = table[in[in_idx + 1]];
      }
    } break;

    case 3: {
      int32_t *out0 = args->out[0];
      int32_t *out1 = args->out[1];
      int32_t *out2 = args->out[2];
      uint32_t out_idx = start;
      uint32_t in_idx = start * 3;
#ifdef __AVX2__
      // Process 8 outputs at a time (24 inputs)
      for (; out_idx + 8 <= end; out_idx += 8, in_idx += 24) {
        simde__m256i idx0 = simde_mm256_set_epi32(
            in[in_idx + 21], in[in_idx + 18], in[in_idx + 15], in[in_idx + 12],
            in[in_idx + 9],  in[in_idx + 6],  in[in_idx + 3],  in[in_idx]);
        simde__m256i idx1 = simde_mm256_set_epi32(
            in[in_idx + 22], in[in_idx + 19], in[in_idx + 16], in[in_idx + 13],
            in[in_idx + 10], in[in_idx + 7],  in[in_idx + 4],  in[in_idx + 1]);
        simde__m256i idx2 = simde_mm256_set_epi32(
            in[in_idx + 23], in[in_idx + 20], in[in_idx + 17], in[in_idx + 14],
            in[in_idx + 11], in[in_idx + 8],  in[in_idx + 5],  in[in_idx + 2]);
        simde__m256i res0 = simde_mm256_i32gather_epi32(table, idx0, 4);
        simde__m256i res1 = simde_mm256_i32gather_epi32(table, idx1, 4);
        simde__m256i res2 = simde_mm256_i32gather_epi32(table, idx2, 4);
        simde_mm256_storeu_si256((simde__m256i *)(out0 + out_idx), res0);
        simde_mm256_storeu_si256((simde__m256i *)(out1 + out_idx), res1);
        simde_mm256_storeu_si256((simde__m256i *)(out2 + out_idx), res2);
      }
#endif
      for (; out_idx < end; out_idx++, in_idx += 3) {
        out0[out_idx] = table[in[in_idx]];
        out1[out_idx] = table[in[in_idx + 1]];
        out2[out_idx] = table[in[in_idx + 2]];
      }
    } break;

    case 4: {
      int32_t *out0 = args->out[0];
      int32_t *out1 = args->out[1];
      int32_t *out2 = args->out[2];
      int32_t *out3 = args->out[3];
      uint32_t out_idx = start;
      uint32_t in_idx = start * 4;
#ifdef __AVX2__
      // Process 8 outputs at a time (32 inputs)
      for (; out_idx + 8 <= end; out_idx += 8, in_idx += 32) {
        simde__m128i b0 = simde_mm_loadl_epi64((simde__m128i *)(in + in_idx));
        simde__m128i b1 = simde_mm_loadl_epi64((simde__m128i *)(in + in_idx + 8));
        simde__m128i b2 = simde_mm_loadl_epi64((simde__m128i *)(in + in_idx + 16));
        simde__m128i b3 = simde_mm_loadl_epi64((simde__m128i *)(in + in_idx + 24));
        simde__m256i idx0 = simde_mm256_cvtepu8_epi32(b0);
        simde__m256i idx1 = simde_mm256_cvtepu8_epi32(b1);
        simde__m256i idx2 = simde_mm256_cvtepu8_epi32(b2);
        simde__m256i idx3 = simde_mm256_cvtepu8_epi32(b3);
        simde__m256i r0 = simde_mm256_i32gather_epi32(table, idx0, 4);
        simde__m256i r1 = simde_mm256_i32gather_epi32(table, idx1, 4);
        simde__m256i r2 = simde_mm256_i32gather_epi32(table, idx2, 4);
        simde__m256i r3 = simde_mm256_i32gather_epi32(table, idx3, 4);
        // Transpose 4x8 -> 8x4
        simde__m256i t0 = simde_mm256_unpacklo_epi32(r0, r1);
        simde__m256i t1 = simde_mm256_unpackhi_epi32(r0, r1);
        simde__m256i t2 = simde_mm256_unpacklo_epi32(r2, r3);
        simde__m256i t3 = simde_mm256_unpackhi_epi32(r2, r3);
        simde__m256i u0 = simde_mm256_unpacklo_epi64(t0, t2);
        simde__m256i u1 = simde_mm256_unpackhi_epi64(t0, t2);
        simde__m256i u2 = simde_mm256_unpacklo_epi64(t1, t3);
        simde__m256i u3 = simde_mm256_unpackhi_epi64(t1, t3);
        simde__m256i perm = simde_mm256_set_epi32(7, 3, 6, 2, 5, 1, 4, 0);
        simde_mm256_storeu_si256((simde__m256i *)(out0 + out_idx), simde_mm256_permutevar8x32_epi32(u0, perm));
        simde_mm256_storeu_si256((simde__m256i *)(out1 + out_idx), simde_mm256_permutevar8x32_epi32(u1, perm));
        simde_mm256_storeu_si256((simde__m256i *)(out2 + out_idx), simde_mm256_permutevar8x32_epi32(u2, perm));
        simde_mm256_storeu_si256((simde__m256i *)(out3 + out_idx), simde_mm256_permutevar8x32_epi32(u3, perm));
      }
#endif
      for (; out_idx < end; out_idx++, in_idx += 4) {
        out0[out_idx] = table[in[in_idx]];
        out1[out_idx] = table[in[in_idx + 1]];
        out2[out_idx] = table[in[in_idx + 2]];
        out3[out_idx] = table[in[in_idx + 3]];
      }
    } break;
  }
}

// Pre-created ISIP task set for modulation (initialized on first use)
static enkiTaskSet* g_mod_256qam_task = NULL;

void nr_modulate_layer_map_256qam_parallel(const uint8_t *scrambled_data,
                                            uint32_t n_symbols,
                                            uint8_t n_layers,
                                            int layerSz,
                                            c16_t tx_layers[][layerSz])
{
  // Check if ISIP is available
  if (!isip_pool_is_initialized()) {
    // Fallback to sequential version
    nr_modulate_layer_map_256qam(scrambled_data, n_symbols, n_layers, layerSz, tx_layers);
    return;
  }

  // For small workloads, use sequential (parallel overhead not worth it)
  // Threshold: ~4K symbols = ~4K table lookups, ~8µs at 2ns/lookup
  const uint32_t n_outputs = n_symbols / n_layers;
  if (n_outputs < 4096) {
    nr_modulate_layer_map_256qam(scrambled_data, n_symbols, n_layers, layerSz, tx_layers);
    return;
  }

  enkiTaskScheduler *scheduler = (enkiTaskScheduler*)isip_pool_get_scheduler();
  if (!scheduler) {
    nr_modulate_layer_map_256qam(scrambled_data, n_symbols, n_layers, layerSz, tx_layers);
    return;
  }

  // Create task set on first use
  if (!g_mod_256qam_task) {
    g_mod_256qam_task = enkiCreateTaskSet(scheduler, mod_256qam_task);
    if (!g_mod_256qam_task) {
      nr_modulate_layer_map_256qam(scrambled_data, n_symbols, n_layers, layerSz, tx_layers);
      return;
    }
  }

  // Setup task arguments
  mod_256qam_task_args_t args;
  args.scrambled_data = scrambled_data;
  args.table = nr_256qam_mod_table;
  args.n_layers = n_layers;
  args.out[0] = (int32_t *)tx_layers[0];
  args.out[1] = (n_layers > 1) ? (int32_t *)tx_layers[1] : NULL;
  args.out[2] = (n_layers > 2) ? (int32_t *)tx_layers[2] : NULL;
  args.out[3] = (n_layers > 3) ? (int32_t *)tx_layers[3] : NULL;

  // Set task parameters
  struct enkiParamsTaskSet params = enkiGetParamsTaskSet(g_mod_256qam_task);
  params.pArgs = &args;
  params.setSize = n_outputs;
  // minRange: each worker gets at least 512 outputs (~1µs work)
  // This balances parallelism vs overhead
  params.minRange = 512;
  enkiSetParamsTaskSet(g_mod_256qam_task, params);

  // Launch parallel tasks and wait
  enkiAddTaskSet(scheduler, g_mod_256qam_task);
  enkiWaitForTaskSet(scheduler, g_mod_256qam_task);
}

#else  // !ISIP_AVAILABLE

// Fallback: just use the sequential version
void nr_modulate_layer_map_256qam_parallel(const uint8_t *scrambled_data,
                                            uint32_t n_symbols,
                                            uint8_t n_layers,
                                            int layerSz,
                                            c16_t tx_layers[][layerSz])
{
  nr_modulate_layer_map_256qam(scrambled_data, n_symbols, n_layers, layerSz, tx_layers);
}

#endif  // ISIP_AVAILABLE
