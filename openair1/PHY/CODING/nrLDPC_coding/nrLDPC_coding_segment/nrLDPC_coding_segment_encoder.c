/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.0  (the "License"); you may not use this file
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

/*! \file PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nrLDPC_coding_segment_encoder.c
 * \brief Top-level routines for implementing LDPC encoding of transport channels
 */

#include "nr_rate_matching.h"
#include "PHY/defs_gNB.h"
#include "PHY/CODING/coding_extern.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/lte_interleaver_inline.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/CODING/nrLDPC_extern.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/NR_TRANSPORT/nr_dlsch.h"
#include "SCHED_NR/sched_nr.h"
#include "common/utils/LOG/vcd_signal_dumper.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"
#include <openair2/UTIL/OPT/opt.h>

#include <syscall.h>

#define DEBUG_LDPC_ENCODING
//#define DEBUG_LDPC_ENCODING_FREE 1

/**
 * \typedef group_tail_t
 * \struct group_tail_s
 * \brief index and content of the last word written by a task to the encoding library output
 * It is returned after task completion to fix the borders of segment groups
 * \var index index of the last word written by the task
 * \var content content of the last word written by the task
 */
typedef struct group_tail_s
{
  uint32_t index;
#if defined(__AVX512VBMI__)
  uint64_t content;
#elif defined(__aarch64__)
  uint16_t content;
#else
  uint32_t content;
#endif
} group_tail_t;

/**
 * \brief fix segment groups first and last words
 * The last word of a segment group is not written by encoding tasks in order to avoid race conditions
 * This function fixes the segment group borders after encoding tasks completion
 * \param group_tail store segment groups tail to fix segment borders
 * \param nbGroups number of groups
 * \params output encoding library output to write to
 */
static void fix_group_borders(group_tail_t *group_tail, int nbGroups, uint8_t *output)
{
  for(int i = 0; i < nbGroups; i++) {
#if defined(__AVX512VBMI__)
    ((uint64_t *)output)[group_tail[i].index] = group_tail[i].content | ((uint64_t *)output)[group_tail[i].index];
#elif defined(__aarch64__)
    ((uint16_t *)output)[group_tail[i].index] = group_tail[i].content | ((uint16_t *)output)[group_tail[i].index];
#else
    ((uint32_t *)output)[group_tail[i].index] = group_tail[i].content | ((uint32_t *)output)[group_tail[i].index];
#endif
  }
}

/**
 * \brief determine the value of E for the current segment
 * \param E number of bits per segment in f
 * \param E2 number of bits per segment in f2
 * \param Eshift flag indicating if there was a E shift within the segment group
 * \param segment index of the segment
 * \param E2_first_segment index of the first segment of size E2 if there is a E shift in the segment group
 */
static inline uint32_t select_E(uint32_t E, uint32_t E2, bool Eshift, uint32_t segment, uint32_t E2_first_segment)
{
  return (Eshift && (segment >= E2_first_segment)) ? E2 : E;
}

/**
 * \brief write nrLDPC_coding_segment_encoder output with interleaver output
 * the interleaver output has 8 bits from 8 different segments per byte
 * nrLDPC_coding_segment_encoder is made of concatenated packed segments
 * \param f input interleaved segment, 8 bits from 8 segments per byte
 * \param E number of bits per segment in f
 * \param f2 input interleaved segment, 8 bits from 8 segments per byte
 * interleaved with E2 if there was a E shift in the segment group
 * \param E2 number of bits per segment in f2
 * \param Eshift flag indicating if there was a E shift within the segment group
 * \param E2_first_segment index of the first segment of size E2 if there is a E shift in the segment group
 * \param nb_segments number of segments in the group
 * \param output nrLDPC_coding_segment_encoder with concatenated segments and packed bits
 * \param Eoffset offset in number of bits of the first segment of the segment group within output
 * \param group_tail store segment group last words to fix segment group borders
 */
static void write_task_output(uint8_t *f,
                              uint32_t E,
                              uint8_t *f2,
                              uint32_t E2,
                              bool Eshift,
                              uint32_t E2_first_segment,
                              uint32_t nb_segments,
                              uint8_t *output,
                              uint32_t Eoffset,
                              group_tail_t *group_tail)
{

#if defined(__AVX512VBMI__)
  uint64_t *output_p = (uint64_t*)output;
  int i=0;
  __m512i bitperm[8];
  uint32_t Eoffset2[8];
  uint32_t Eoffset2_bit[8];
  bitperm[0] = _mm512_set1_epi64(0x3830282018100800);
  __m512i inc = _mm512_set1_epi8(0x1);
  Eoffset2[0] = Eoffset >> 6;
  Eoffset2_bit[0] = Eoffset & 63;
  Eoffset += select_E(E, E2, Eshift, 0, E2_first_segment);
  for (int n = 1; n < nb_segments; n++) {
    bitperm[n] = _mm512_add_epi8(bitperm[n-1] ,inc);
    Eoffset2[n] = Eoffset >> 6;
    Eoffset2_bit[n] = Eoffset & 63;
    Eoffset += select_E(E, E2, Eshift, n, E2_first_segment);
  } 
  group_tail->index = Eoffset2[nb_segments - 1];
  int i2=0;
  __m64 tmp;
  for (i=0;i<E2;i+=64,i2++) {
    if (i<E) {
      for (int j=0; j < E2_first_segment && (j < (nb_segments - 1) || i < (E2 - 64)); j++) {
        // Note: Here and below for AVX2, we are using the 64-bit SIMD instruction
        // instead of C >>/<< because when the Eoffset2_bit is 64 or 0, the <<
        // and >> operations are undefined and in fact don't give "0" which is
        // what we want here. The SIMD version do give 0 when the shift is 64
        tmp = (__m64)_mm512_bitshuffle_epi64_mask(((__m512i *)f)[i2],bitperm[j]);

        *(__m64*)(output_p + Eoffset2[j]) = _mm_or_si64(*(__m64*)(output_p + Eoffset2[j]),_mm_slli_si64(tmp,Eoffset2_bit[j]));

        *(__m64*)(output_p + Eoffset2[j] + 1) = _mm_or_si64(*(__m64*)(output_p + Eoffset2[j]+1),_mm_srli_si64(tmp,(64-Eoffset2_bit[j])));

      } //for (int j=0;
    } // if (i < E)
    for (int j=E2_first_segment; j < nb_segments && (j < (nb_segments - 1) || i < (E2 - 64)); j++) {
      tmp = (__m64)_mm512_bitshuffle_epi64_mask(((__m512i *)f2)[i2],bitperm[j]);

      *(__m64*)(output_p + Eoffset2[j]) = _mm_or_si64(*(__m64*)(output_p + Eoffset2[j]),_mm_slli_si64(tmp,Eoffset2_bit[j]));

      *(__m64*)(output_p + Eoffset2[j]+1) = _mm_or_si64(*(__m64*)(output_p + Eoffset2[j]+1),_mm_srli_si64(tmp,(64-Eoffset2_bit[j])));
    }
    output_p++;
    group_tail->index++;
  }

  // Do not write the last word of the segment group but keep it for fixing segment borders
  if (Eshift) {
    tmp = (__m64)_mm512_bitshuffle_epi64_mask(((__m512i *)f2)[(E2 - 1) >> 6], bitperm[nb_segments - 1]);
  } else {
    tmp = (__m64)_mm512_bitshuffle_epi64_mask(((__m512i *)f)[(E2 - 1) >> 6], bitperm[nb_segments - 1]);
  }
  *(__m64*)(output_p - 1 + Eoffset2[nb_segments - 1]) = _mm_or_si64(*(__m64*)(output_p - 1 + Eoffset2[nb_segments - 1]), _mm_slli_si64(tmp, Eoffset2_bit[nb_segments - 1]));
  group_tail->content = (uint64_t)_mm_srli_si64(tmp, (64 - Eoffset2_bit[nb_segments - 1]));

#elif defined(__aarch64__)
  uint16_t *output_p = (uint16_t*)output;
  int i=0;
  uint32_t Eoffset2[8];
  uint32_t Eoffset2_bit[8];
  for (int n = 0; n < nb_segments; n++) {
    Eoffset2[n]  = Eoffset >> 4;
    Eoffset2_bit[n] = Eoffset & 15;
    Eoffset += select_E(E, E2, Eshift, n, E2_first_segment);
  } 
  group_tail->index = Eoffset2[nb_segments - 1];
  int i2=0;
  const int8_t __attribute__ ((aligned (16))) ucShift[8][16] = {
    {0,1,2,3,4,5,6,7,0,1,2,3,4,5,6,7},     // segment 0
    {-1,0,1,2,3,4,5,6,-1,0,1,2,3,4,5,6},   // segment 1
    {-2,-1,0,1,2,3,4,5,-2,-1,0,1,2,3,4,5}, // segment 2
    {-3,-2,-1,0,1,2,3,4,-3,-2,-1,0,1,2,3,4}, // segment 3
    {-4,-3,-2,-1,0,1,2,3,-4,-3,-2,-1,0,1,2,3}, // segment 4
    {-5,-4,-3,-2,-1,0,1,2,-5,-4,-3,-2,-1,0,1,2}, // segment 5
    {-6,-5,-4,-3,-2,-1,0,1,-6,-5,-4,-3,-2,-1,0,1}, // segment 6
    {-7,-6,-5,-4,-3,-2,-1,0,-7,-6,-5,-4,-3,-2,-1,0}}; // segment 7
  const uint8_t __attribute__ ((aligned (16))) masks[16] = 
      {0x1,0x2,0x4,0x8,0x10,0x20,0x40,0x80,0x1,0x2,0x4,0x8,0x10,0x20,0x40,0x80};
  int8x16_t vshift[8];
  for (int n=0;n<8;n++) vshift[n] = vld1q_s8(ucShift[n]);
  uint8x16_t vmask  = vld1q_u8(masks);

  int32_t tmp;
  uint8x16_t cshift;
  for (i=0;i<E2;i+=16,i2++) {
    if (i<E) {	
      for (int j=0; j < E2_first_segment && (j < (nb_segments - 1) || i < (E2 - 16)); j++) {
        cshift = vandq_u8(vshlq_u8(((uint8x16_t*)f)[i2],vshift[j]),vmask);
        tmp = (int)vaddv_u8(vget_low_u8(cshift));
        tmp += (int)(vaddv_u8(vget_high_u8(cshift))<<8);
        *(output_p + Eoffset2[j]) |= (uint16_t)(tmp<<Eoffset2_bit[j]);
        *(output_p + Eoffset2[j] + 1) |= (uint16_t)(tmp>>(16-Eoffset2_bit[j]));
      }
    }
    for (int j=E2_first_segment; j < nb_segments && (j < (nb_segments - 1) || i < (E2 - 16)); j++) {
      cshift = vandq_u8(vshlq_u8(((uint8x16_t*)f2)[i2],vshift[j]),vmask);
      tmp = (int)vaddv_u8(vget_low_u8(cshift));
      tmp += (int)(vaddv_u8(vget_high_u8(cshift))<<8);
      *(output_p + Eoffset2[j]) |= (uint16_t)(tmp<<Eoffset2_bit[j]);
      *(output_p + Eoffset2[j] + 1) |= (uint16_t)(tmp>>(16-Eoffset2_bit[j]));
    }
    output_p++;
    group_tail->index++;
  }

  // Do not write the last word of the segment group but keep it for fixing segment borders
  if (Eshift) {
    cshift = vandq_u8(vshlq_u8(((uint8x16_t*)f2)[(E2 - 1) >> 4], vshift[nb_segments - 1]), vmask);
  } else {
    cshift = vandq_u8(vshlq_u8(((uint8x16_t*)f)[(E2 - 1) >> 4], vshift[nb_segments - 1]), vmask);
  }
  tmp = (int)vaddv_u8(vget_low_u8(cshift));
  tmp += (int)(vaddv_u8(vget_high_u8(cshift)) << 8);
  *(output_p  - 1 + Eoffset2[nb_segments - 1]) |= (uint16_t)(tmp << Eoffset2_bit[nb_segments - 1]);
  group_tail->content = (uint16_t)(tmp >> (16 - Eoffset2_bit[nb_segments - 1]));
       
#else
  uint32_t *output_p = (uint32_t*)output;
  int i=0;
  uint32_t Eoffset2[8];
  uint32_t Eoffset2_bit[8];
  for (int n = 0; n < nb_segments; n++) {
    Eoffset2[n]  = Eoffset >> 5;
    Eoffset2_bit[n] = Eoffset & 31;
    Eoffset += select_E(E, E2, Eshift, n, E2_first_segment);
  } 
  group_tail->index = Eoffset2[nb_segments - 1];
  int i2 = 0;
  int tmp;
  __m64 tmp64, tmp64b, tmp64c, out64;
  
  for (i=0; i < E2; i += 32, i2++) {
    if (i < E) {
      for (int j = 0; j < E2_first_segment && (j < (nb_segments - 1) || i < (E2 - 32)); j++) {
        // Note: Here and below, we are using the 64-bit SIMD instruction
        // instead of C >>/<< because when the Eoffset2_bit is 64 or 0, the <<
        // and >> operations are undefined and in fact don't give "0" which is
        // what we want here. The SIMD version do give 0 when the shift is 64
        tmp = _mm256_movemask_epi8(_mm256_slli_epi16(((__m256i *)f)[i2], 7 - j));
        tmp64 = _mm_set1_pi32(tmp);
        out64 = _mm_set_pi32(*(output_p + Eoffset2[j] + 1), *(output_p + Eoffset2[j]));
        tmp64b = _mm_or_si64(out64, _mm_slli_pi32(tmp64, Eoffset2_bit[j]));
        tmp64c = _mm_or_si64(out64, _mm_srli_pi32(tmp64, (32 - Eoffset2_bit[j])));
        *(output_p + Eoffset2[j]) = _m_to_int(tmp64b);
        *(output_p + Eoffset2[j] + 1) = _m_to_int(_mm_srli_si64(tmp64c, 32));
      }
    } 
    for (int j = E2_first_segment; j < nb_segments && (j < (nb_segments - 1) || i < (E2 - 32)); j++) {
      tmp = _mm256_movemask_epi8(_mm256_slli_epi16(((__m256i *)f2)[i2], 7 - j));
      tmp64 = _mm_set1_pi32(tmp);
      out64 = _mm_set_pi32(*(output_p + Eoffset2[j] + 1), *(output_p + Eoffset2[j]));
      tmp64b = _mm_or_si64(out64, _mm_slli_pi32(tmp64, Eoffset2_bit[j]));
      tmp64c = _mm_or_si64(out64, _mm_srli_pi32(tmp64, (32 - Eoffset2_bit[j])));
      *(output_p + Eoffset2[j])  = _m_to_int(tmp64b);
      *(output_p + Eoffset2[j] + 1) = _m_to_int(_mm_srli_si64(tmp64c, 32));
    }
    output_p++;
    group_tail->index++;
  }

  // Do not write the last word of the segment group but keep it for fixing segment borders
  if (Eshift) {
    tmp = _mm256_movemask_epi8(_mm256_slli_epi16(((__m256i *)f2)[(E2 - 1) >> 5], 7 - nb_segments + 1));
  } else {
    tmp = _mm256_movemask_epi8(_mm256_slli_epi16(((__m256i *)f)[(E2 - 1) >> 5], 7 - nb_segments + 1));
  }
  tmp64 = _mm_set1_pi32(tmp);
  out64 = _mm_set_pi32(*(output_p - 1 + Eoffset2[nb_segments - 1] + 1), *(output_p - 1 + Eoffset2[nb_segments - 1]));
  tmp64b = _mm_or_si64(out64, _mm_slli_pi32(tmp64, Eoffset2_bit[nb_segments - 1]));
  tmp64c = _mm_srli_pi32(tmp64, (32 - Eoffset2_bit[nb_segments - 1]));
  *(output_p - 1 + Eoffset2[nb_segments - 1])  = _m_to_int(tmp64b);
  group_tail->content = _m_to_int(tmp64c);
  
#endif
}

typedef struct ldpc8blocks_args_s {
  nrLDPC_TB_encoding_parameters_t *nrLDPC_TB_encoding_parameters;
  encoder_implemparams_t impp;
  group_tail_t *group_tail;
} ldpc8blocks_args_t;

static void ldpc8blocks(void *p)
{
  ldpc8blocks_args_t *args = (ldpc8blocks_args_t *)p;
  nrLDPC_TB_encoding_parameters_t *nrLDPC_TB_encoding_parameters = args->nrLDPC_TB_encoding_parameters;
  encoder_implemparams_t *impp = &args->impp;

  uint8_t mod_order = nrLDPC_TB_encoding_parameters->Qm;
  uint16_t nb_rb = nrLDPC_TB_encoding_parameters->nb_rb;
  uint32_t A = nrLDPC_TB_encoding_parameters->A;

  unsigned int G = nrLDPC_TB_encoding_parameters->G;
  LOG_D(PHY, "dlsch coding A %d K %d G %d (nb_rb %d, mod_order %d)\n", A, impp->K, G, nb_rb, (int)mod_order);

  // nrLDPC_encoder output is in "d"
  // let's make this interface happy!
  uint8_t d[68 * 384] __attribute__((aligned(64)));
  uint8_t *c[nrLDPC_TB_encoding_parameters->C];
  unsigned int macro_segment, macro_segment_end;

  
  macro_segment = impp->first_seg;
  macro_segment_end = (impp->n_segments > impp->first_seg + 8) ? impp->first_seg + 8 : impp->n_segments;
  for (int r = 0; r < nrLDPC_TB_encoding_parameters->C; r++)
    c[r] = nrLDPC_TB_encoding_parameters->segments[r].c;
  start_meas(&nrLDPC_TB_encoding_parameters->segments[impp->first_seg].ts_ldpc_encode);
  LDPCencoder(c, d, impp);
  stop_meas(&nrLDPC_TB_encoding_parameters->segments[impp->first_seg].ts_ldpc_encode);
  // Compute where to place in output buffer that is concatenation of all segments

#ifdef DEBUG_LDPC_ENCODING
  LOG_D(PHY, "rvidx in encoding = %d\n", nrLDPC_TB_encoding_parameters->rv_index);
#endif
  const uint32_t E = nrLDPC_TB_encoding_parameters->segments[macro_segment].E;
  uint32_t E2=E,E2_first_segment=macro_segment_end-macro_segment;
  bool Eshift=false;
  uint32_t Emax = E;
  for (int s=macro_segment;s<macro_segment_end;s++)
      if (nrLDPC_TB_encoding_parameters->segments[s].E != E) {
	 E2=nrLDPC_TB_encoding_parameters->segments[s].E;
         Eshift=true;
	 E2_first_segment = s-macro_segment;
         if(E2 > Emax)
           Emax = E2;
         break;
      }	 
    

  LOG_D(NR_PHY,
        "Rate Matching, Code segment %d...%d/%d (coded bits (G) %u, E %d, E2 %d Filler bits %d, Filler offset %d mod_order %d, nb_rb "
          "%d,nrOfLayer %d)...\n",
        macro_segment,
        macro_segment_end,
        impp->n_segments,
        G,
        E,E2,
        impp->F,
        impp->K - impp->F - 2 * impp->Zc,
        mod_order,
        nb_rb,
        nrLDPC_TB_encoding_parameters->nb_layers);
/*
  printf("Rate Matching, Code segment %d...%d/%d (coded bits (G) %u, E %d, E2 %d Filler bits %d, Filler offset %d mod_order %d, nb_rb "
          "%d,nrOfLayer %d)...\n",
        macro_segment,
        macro_segment_end,
        impp->n_segments,
        G,
        E,E2,
        impp->F,
        impp->K - impp->F - 2 * impp->Zc,
        mod_order,
        nb_rb,
        nrLDPC_TB_encoding_parameters->nb_layers);
*/
  uint32_t Tbslbrm = nrLDPC_TB_encoding_parameters->tbslbrm;

  uint8_t e[Emax] __attribute__((aligned(64)));
  uint8_t f[E+64] __attribute__((aligned(64)));
  uint8_t f2[E2+64] __attribute__((aligned(64)));
  bzero(e, Emax);
  bzero(f, E + 64);
  if (Eshift) {
    bzero(f2, E2 + 64);
  }
  start_meas(&nrLDPC_TB_encoding_parameters->segments[macro_segment].ts_rate_match);
  nr_rate_matching_ldpc(Tbslbrm,
                        impp->BG,
                        impp->Zc,
                        d,
                        e,
                        impp->n_segments,
                        impp->F,
                        impp->K - impp->F - 2 * impp->Zc,
                        nrLDPC_TB_encoding_parameters->rv_index,
                        Emax);

  stop_meas(&nrLDPC_TB_encoding_parameters->segments[macro_segment].ts_rate_match);
  if (impp->K - impp->F - 2 * impp->Zc > E) {
    LOG_E(PHY,
          "dlsch coding A %d  Kr %d G %d (nb_rb %d, mod_order %d)\n",
          A,
          impp->K,
          G,
          nb_rb,
          (int)mod_order);

    LOG_E(NR_PHY,
          "Rate Matching, Code segments %d...%d/%d (coded bits (G) %u, E %d, Kr %d, Filler bits %d, Filler offset %d mod_order %d, "
          "nb_rb %d)...\n",
          macro_segment,
	  macro_segment_end,
          impp->n_segments,
          G,
          E,
          impp->K,
          impp->F,
          impp->K - impp->F - 2 * impp->Zc,
          mod_order,
          nb_rb);
  }
  start_meas(&nrLDPC_TB_encoding_parameters->segments[macro_segment].ts_interleave);
  nr_interleaving_ldpc(E,
                       mod_order,
                       e,
                       f);
  if (Eshift)
    nr_interleaving_ldpc(E2,
                         mod_order,
                         e,
                         f2);
  stop_meas(&nrLDPC_TB_encoding_parameters->segments[macro_segment].ts_interleave);
  if(impp->toutput != NULL) start_meas(impp->toutput);
/*
  for (int i=0;i<16;i++)
     for (int s=0;s<macro_segment_end-macro_segment;s++) 
        printf("i %d: segment %d : f[%d] %d\n",i,macro_segment+s,i,(f[i]>>s)&1);
*/
  // information part and puncture columns
 
  uint32_t Eoffset=0;
  for (int s=0; s<macro_segment; s++)
    Eoffset += (nrLDPC_TB_encoding_parameters->segments[s].E); 

  write_task_output(f,
                    E,
                    f2,
                    E2,
                    Eshift,
                    E2_first_segment,
                    macro_segment_end - macro_segment,
                    impp->output,
                    Eoffset,
		    args->group_tail);

  if(impp->toutput != NULL) stop_meas(impp->toutput);
/*
  printf("E %d (mod16 %d) F %d\n",E,E&15,impp->F); 
  for (int i=0;i<16;i++)
     for (int s=0;s<macro_segment_end-macro_segment;s++) 
        printf("i %d: segment %d : output[%d] %d\n",i,macro_segment+s,i,output_offset[i+s*E]);
*/
  // Task running in // completed
  completed_task_ans(impp->ans);
}

static int nrLDPC_launch_TB_encoding(nrLDPC_slot_encoding_parameters_t *nrLDPC_slot_encoding_parameters,
                                     int dlsch_id,
                                     thread_info_tm_t *t_info,
                                     group_tail_t *group_tail)
{
  nrLDPC_TB_encoding_parameters_t *nrLDPC_TB_encoding_parameters = &nrLDPC_slot_encoding_parameters->TBs[dlsch_id];

  encoder_implemparams_t common_segment_params = {
    .n_segments = nrLDPC_TB_encoding_parameters->C,
    .tinput = nrLDPC_slot_encoding_parameters->tinput,
    .tprep = nrLDPC_slot_encoding_parameters->tprep,
    .tparity = nrLDPC_slot_encoding_parameters->tparity,
    .toutput = nrLDPC_slot_encoding_parameters->toutput,
    .Kb = nrLDPC_TB_encoding_parameters->Kb,
    .Zc = nrLDPC_TB_encoding_parameters->Z,
    .BG = nrLDPC_TB_encoding_parameters->BG,
    .output = nrLDPC_TB_encoding_parameters->output, 
    .K = nrLDPC_TB_encoding_parameters->K,
    .F = nrLDPC_TB_encoding_parameters->F,
  };

  const size_t n_tasks = (common_segment_params.n_segments + 7) >> 3;

  for (int j = 0; j < n_tasks; j++) {
    ldpc8blocks_args_t *perJobImpp = &((ldpc8blocks_args_t *)t_info->buf)[t_info->len];
    DevAssert(t_info->len < t_info->cap);
    t_info->len += 1;

    perJobImpp->impp = common_segment_params;
    perJobImpp->impp.first_seg = j * 8;
    perJobImpp->impp.ans = t_info->ans;
    perJobImpp->nrLDPC_TB_encoding_parameters = nrLDPC_TB_encoding_parameters;
    perJobImpp->group_tail = &group_tail[j];

    task_t t = {.func = ldpc8blocks, .args = perJobImpp};
    pushTpool(nrLDPC_slot_encoding_parameters->threadPool, t);
  }
  return n_tasks;
}

int nrLDPC_coding_encoder(nrLDPC_slot_encoding_parameters_t *nrLDPC_slot_encoding_parameters)
{
  int nbTasks = 0;

  for (int dlsch_id = 0; dlsch_id < nrLDPC_slot_encoding_parameters->nb_TBs; dlsch_id++) {
    nrLDPC_TB_encoding_parameters_t *nrLDPC_TB_encoding_parameters = &nrLDPC_slot_encoding_parameters->TBs[dlsch_id];
    size_t n_seg = (nrLDPC_TB_encoding_parameters->C / 8 + ((nrLDPC_TB_encoding_parameters->C & 7) == 0 ? 0 : 1));
    nbTasks += n_seg;
  }
  ldpc8blocks_args_t arr[nbTasks];
  task_ans_t ans;
  init_task_ans(&ans, nbTasks);
  thread_info_tm_t t_info = {.buf = (uint8_t *)arr, .len = 0, .cap = nbTasks, .ans = &ans};

  group_tail_t group_tail[nbTasks];
  memset(group_tail, 0, nbTasks * sizeof(group_tail_t));

  int nbEncode = 0;
  for (int dlsch_id = 0; dlsch_id < nrLDPC_slot_encoding_parameters->nb_TBs; dlsch_id++) {
    nbEncode += nrLDPC_launch_TB_encoding(nrLDPC_slot_encoding_parameters, dlsch_id, &t_info, &group_tail[dlsch_id]);
  }
  if (nbEncode < nbTasks) {
    completed_many_task_ans(&ans, nbTasks - nbEncode);
  }
  // Execute thread pool tasks
  join_task_ans(&ans);

  int n_seg_sum = 0;
  for (int dlsch_id = 0; dlsch_id < nrLDPC_slot_encoding_parameters->nb_TBs; dlsch_id++) {
    nrLDPC_TB_encoding_parameters_t *nrLDPC_TB_encoding_parameters = &nrLDPC_slot_encoding_parameters->TBs[dlsch_id];
    int n_seg = (nrLDPC_TB_encoding_parameters->C / 8 + ((nrLDPC_TB_encoding_parameters->C & 7) == 0 ? 0 : 1));
    fix_group_borders(&group_tail[n_seg_sum], n_seg, nrLDPC_TB_encoding_parameters->output);
    n_seg_sum += n_seg;
  }

  return 0;
}
