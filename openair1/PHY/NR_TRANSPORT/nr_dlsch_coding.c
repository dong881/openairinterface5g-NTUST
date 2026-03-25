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

/*! \file PHY/NR_TRANSPORT/nr_dlsch_coding_slot.c
 * \brief Top-level routines for implementing LDPC-coded (DLSCH) transport channels from 38-212, 15.2
 */

#include "PHY/defs_gNB.h"
#include "PHY/CODING/coding_extern.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/lte_interleaver_inline.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/CODING/nrLDPC_extern.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/NR_TRANSPORT/nr_dlsch.h"
#include "PHY/ISIP_POOL/isip_pool.h"  // For parallel segmentation
#include "SCHED_NR/sched_nr.h"
#include "SCHED_NR/nr_slot_timing.h"
#include "common/utils/LOG/vcd_signal_dumper.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"
#include <syscall.h>
#include <openair2/UTIL/OPT/opt.h>

// #define DEBUG_DLSCH_CODING
// #define DEBUG_DLSCH_FREE 1

void free_gNB_dlsch(NR_gNB_DLSCH_t *dlsch, uint16_t N_RB, const NR_DL_FRAME_PARMS *frame_parms)
{
  int max_layers = (frame_parms->nb_antennas_tx < NR_MAX_NB_LAYERS) ? frame_parms->nb_antennas_tx : NR_MAX_NB_LAYERS;
  uint16_t a_segments = MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER * max_layers;

  if (N_RB != 273) {
    a_segments = a_segments * N_RB;
    a_segments = a_segments / 273 + 1;
  }

  if (dlsch->b) {
    free16(dlsch->b, a_segments * 1056);
    dlsch->b = NULL;
  }
  if (dlsch->f) {
    free16(dlsch->f, N_RB * NR_SYMBOLS_PER_SLOT * NR_NB_SC_PER_RB * 8 * NR_MAX_NB_LAYERS);
    dlsch->f = NULL;
  }
  for (int r = 0; r < a_segments; r++) {
    free(dlsch->c[r]);
    dlsch->c[r] = NULL;
  }
  free(dlsch->c);
}

NR_gNB_DLSCH_t new_gNB_dlsch(NR_DL_FRAME_PARMS *frame_parms, uint16_t N_RB)
{
  int max_layers = (frame_parms->nb_antennas_tx < NR_MAX_NB_LAYERS) ? frame_parms->nb_antennas_tx : NR_MAX_NB_LAYERS;
  uint16_t a_segments = MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER * max_layers; // number of segments to be allocated

  if (N_RB != 273) {
    a_segments = a_segments * N_RB;
    a_segments = a_segments / 273 + 1;
  }

  LOG_D(PHY, "Allocating %d segments (MAX %d, N_PRB %d)\n", a_segments, MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER, N_RB);
  uint32_t dlsch_bytes = a_segments * 1056; // allocated bytes per segment
  NR_gNB_DLSCH_t dlsch = {0};

  dlsch.b = malloc16(dlsch_bytes);
  AssertFatal(dlsch.b, "cannot allocate memory for dlsch.b\n");
  bzero(dlsch.b, dlsch_bytes);

  dlsch.c = (uint8_t **)malloc16(a_segments * sizeof(uint8_t *));
  for (int r = 0; r < a_segments; r++) {
    // account for filler in first segment and CRCs for multiple segment case
    // [hna] 8448 is the maximum CB size in NR
    //       68*348 = 68*(maximum size of Zc)
    //       In section 5.3.2 in 38.212, the for loop is up to N + 2*Zc (maximum size of N is 66*Zc, therefore 68*Zc)
    dlsch.c[r] = malloc16(8448);
    AssertFatal(dlsch.c[r], "cannot allocate dlsch.c[%d]\n", r);
    bzero(dlsch.c[r], 8448);
  }

  dlsch.f = malloc16(N_RB * NR_SYMBOLS_PER_SLOT * NR_NB_SC_PER_RB * 8 * NR_MAX_NB_LAYERS);
  AssertFatal(dlsch.f, "cannot allocate dlsch->f\n");
  bzero(dlsch.f, N_RB * NR_SYMBOLS_PER_SLOT * NR_NB_SC_PER_RB * 8 * NR_MAX_NB_LAYERS);

  return (dlsch);
}

int nr_dlsch_encoding(PHY_VARS_gNB *gNB,
                      int n_dlsch,
                      NR_gNB_DLSCH_t *dlsch_array,
                      int frame,
                      uint8_t slot,
                      NR_DL_FRAME_PARMS *frame_parms,
                      unsigned char *output,
                      time_stats_t *tinput,
                      time_stats_t *tprep,
                      time_stats_t *tparity,
                      time_stats_t *toutput,
                      time_stats_t *dlsch_rate_matching_stats,
                      time_stats_t *dlsch_interleaving_stats,
                      time_stats_t *dlsch_segmentation_stats)
{
  VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_gNB_DLSCH_ENCODING, VCD_FUNCTION_IN);

  // Timing for encoding breakdown
  struct timespec t_crc_start, t_crc_end, t_seg_start, t_seg_end, t_ldpc_start, t_ldpc_end;
  long crc_time_ns = 0, seg_time_ns = 0, ldpc_total_ns = 0;

  nrLDPC_TB_encoding_parameters_t TBs[n_dlsch];
  memset(TBs, 0, sizeof(TBs));

  // OPTIMIZED: Use fixed-size array to avoid VLA and merge loops
  // Max segments = MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER * NR_MAX_NB_LAYERS * max_pdsch_per_slot
  #define MAX_SEGMENTS_PER_SLOT (MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER * NR_MAX_NB_LAYERS * 8)
  nrLDPC_segment_encoding_parameters_t segments[MAX_SEGMENTS_PER_SLOT];
  memset(segments, 0, sizeof(segments));

  int num_segments = 0;
  size_t segments_offset = 0;
  size_t dlsch_offset = 0;

  // Start CRC+Seg timing
  if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_crc_start);

  // MERGED LOOP: CRC + Segmentation + LDPC param setup in ONE pass
  for (int i = 0; i < n_dlsch; i++) {
    NR_gNB_DLSCH_t *dlsch = &dlsch_array[i];

    unsigned int crc = 1;
    const nfapi_nr_dl_tti_pdsch_pdu_rel15_t *rel15 = &dlsch->pdsch_pdu->pdsch_pdu_rel15;
    uint32_t A = rel15->TBSize[0] << 3;
    unsigned char *a = dlsch->pdu;
    if (rel15->rnti != SI_RNTI) {
      ws_trace_t tmp = {.nr = true,
                        .direction = DIRECTION_DOWNLINK,
                        .pdu_buffer = a,
                        .pdu_buffer_size = rel15->TBSize[0],
                        .ueid = 0,
                        .rntiType = WS_C_RNTI,
                        .rnti = rel15->rnti,
                        .sysFrame = frame,
                        .subframe = slot,
                        .harq_pid = 0, // difficult to find the harq pid here
                        .oob_event = 0,
                        .oob_event_value = 0};
      trace_pdu(&tmp);
    }

    NR_gNB_PHY_STATS_t *phy_stats = NULL;
    if (rel15->rnti != 0xFFFF)
      phy_stats = get_phy_stats(gNB, rel15->rnti);

    if (phy_stats) {
      phy_stats->frame = frame;
      phy_stats->dlsch_stats.total_bytes_tx += rel15->TBSize[0];
      phy_stats->dlsch_stats.current_RI = rel15->nrOfLayers;
      phy_stats->dlsch_stats.current_Qm = rel15->qamModOrder[0];
    }

    int max_bytes = MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER * rel15->nrOfLayers * 1056;
    int B;
    if (A > NR_MAX_PDSCH_TBS) {
      // Add 24-bit crc (polynomial A) to payload (in-place on harq->pdu)
      crc = crc24a(a, A) >> 8;
      a[A >> 3] = ((uint8_t *)&crc)[2];
      a[1 + (A >> 3)] = ((uint8_t *)&crc)[1];
      a[2 + (A >> 3)] = ((uint8_t *)&crc)[0];
      B = A + 24;
      AssertFatal((A / 8) + 4 <= max_bytes, "A %d is too big (A/8+4 = %d > %d)\n", A, (A / 8) + 4, max_bytes);
      // NOTE: Copy to dlsch->b is deferred to sequential fallback or handled by ISIP directly
    } else {
      // Add 16-bit crc (polynomial A) to payload (in-place on harq->pdu)
      crc = crc16(a, A) >> 16;
      a[A >> 3] = ((uint8_t *)&crc)[1];
      a[1 + (A >> 3)] = ((uint8_t *)&crc)[0];
      B = A + 16;
      AssertFatal((A / 8) + 3 <= max_bytes, "A %d is too big (A/8+3 = %d > %d)\n", A, (A / 8) + 3, max_bytes);
      // NOTE: Copy to dlsch->b is deferred to sequential fallback or handled by ISIP directly
    }

    nrLDPC_TB_encoding_parameters_t *TB_parameters = &TBs[i];

    // The harq_pid is not unique among the active HARQ processes in the instance so we use i instead
    TB_parameters->harq_unique_pid = i;
    TB_parameters->BG = rel15->maintenance_parms_v3.ldpcBaseGraph;
    TB_parameters->A = A;

    // End CRC timing, start segmentation timing (first TB only)
    if (slot_timing_enabled && i == 0) {
      clock_gettime(CLOCK_MONOTONIC, &t_crc_end);
      clock_gettime(CLOCK_MONOTONIC, &t_seg_start);
    }

    start_meas(dlsch_segmentation_stats);

    // Segmentation: direct from pdu, skip dlsch->b
    if (isip_pool_segmentation_enabled()) {
      unsigned int Kprime, L;
      int32_t Kb_result = nr_segmentation_params(B,
                                                  TB_parameters->BG,
                                                  &TB_parameters->C,
                                                  &TB_parameters->K,
                                                  &TB_parameters->Z,
                                                  &TB_parameters->F,
                                                  &Kprime,
                                                  &L);

      if (Kb_result < 0) {
        LOG_E(PHY, "nr_segmentation_params failed for B=%d\n", B);
        return (-1);
      }
      TB_parameters->Kb = (uint32_t)Kb_result;

      isip_seg_params_t seg_params = {
        .input = a,
        .outputs = dlsch->c,
        .C = TB_parameters->C,
        .K = TB_parameters->K,
        .Kprime = Kprime,
        .Z = TB_parameters->Z,
        .F = TB_parameters->F,
        .L = L,
        .completed = 0
      };
      isip_pool_segmentation_parallel(&seg_params);
    } else {
      // Fallback: Copy to dlsch->b and use sequential segmentation
      if (A > NR_MAX_PDSCH_TBS) {
        memcpy(dlsch->b, a, (A / 8) + 4);
      } else {
        memcpy(dlsch->b, a, (A / 8) + 3);
      }
      TB_parameters->Kb = nr_segmentation(dlsch->b,
                                          dlsch->c,
                                          B,
                                          &TB_parameters->C,
                                          &TB_parameters->K,
                                          &TB_parameters->Z,
                                          &TB_parameters->F,
                                          TB_parameters->BG);
    }

    stop_meas(dlsch_segmentation_stats);

    if (TB_parameters->C > MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER * rel15->nrOfLayers) {
      LOG_E(PHY, "nr_segmentation.c: too many segments %d, B %d\n", TB_parameters->C, B);
      return (-1);
    }
    // IMMEDIATELY setup LDPC params after segmentation (merged from second loop)
    TB_parameters->nb_rb = rel15->rbSize;
    TB_parameters->Qm = rel15->qamModOrder[0];
    TB_parameters->mcs = rel15->mcsIndex[0];
    TB_parameters->nb_layers = rel15->nrOfLayers;
    TB_parameters->rv_index = rel15->rvIndex[0];

    int nb_re_dmrs =
        (rel15->dmrsConfigType == NFAPI_NR_DMRS_TYPE1) ? (6 * rel15->numDmrsCdmGrpsNoData) : (4 * rel15->numDmrsCdmGrpsNoData);
    TB_parameters->G = nr_get_G(rel15->rbSize,
                                rel15->NrOfSymbols,
                                nb_re_dmrs,
                                get_num_dmrs(rel15->dlDmrsSymbPos),
                                dlsch->unav_res,
                                rel15->qamModOrder[0],
                                rel15->nrOfLayers);

    TB_parameters->tbslbrm = rel15->maintenance_parms_v3.tbSizeLbrmBytes;
    TB_parameters->output = &output[dlsch_offset >> 3];
    TB_parameters->segments = &segments[segments_offset];

    // Setup segment parameters IMMEDIATELY after segmentation
    for (int r = 0; r < TB_parameters->C; r++) {
      nrLDPC_segment_encoding_parameters_t *segment_parameters = &TB_parameters->segments[r];
      segment_parameters->c = dlsch->c[r];
      segment_parameters->E = nr_get_E(TB_parameters->G, TB_parameters->C, TB_parameters->Qm, rel15->nrOfLayers, r);

      reset_meas(&segment_parameters->ts_interleave);
      reset_meas(&segment_parameters->ts_rate_match);
      reset_meas(&segment_parameters->ts_ldpc_encode);
    }

    segments_offset += TB_parameters->C;
    num_segments += TB_parameters->C;

    const size_t dlsch_size = rel15->rbSize * NR_SYMBOLS_PER_SLOT * NR_NB_SC_PER_RB * rel15->qamModOrder[0] * rel15->nrOfLayers;
    dlsch_offset += ceil_mod(dlsch_size, 8 * 64);
  }

  // End segmentation timing
  if (slot_timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_seg_end);
    crc_time_ns = (t_crc_end.tv_sec - t_crc_start.tv_sec) * 1000000000L +
                  (t_crc_end.tv_nsec - t_crc_start.tv_nsec);
    seg_time_ns = (t_seg_end.tv_sec - t_seg_start.tv_sec) * 1000000000L +
                  (t_seg_end.tv_nsec - t_seg_start.tv_nsec);
  }

  // Start LDPC+RM+Interleave timing (ACC100 does all three in hardware)
  if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_ldpc_start);

  nrLDPC_slot_encoding_parameters_t slot_parameters = {.frame = frame,
                                                       .slot = slot,
                                                       .nb_TBs = n_dlsch,
                                                       .threadPool = &gNB->threadPool,
                                                       .tinput = tinput,
                                                       .tprep = tprep,
                                                       .tparity = tparity,
                                                       .toutput = toutput,
                                                       .TBs = TBs};

  gNB->nrLDPC_coding_interface.nrLDPC_coding_encoder(&slot_parameters);

  // End LDPC+RM+Interleave timing
  if (slot_timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_ldpc_end);
    ldpc_total_ns = (t_ldpc_end.tv_sec - t_ldpc_start.tv_sec) * 1000000000L +
                    (t_ldpc_end.tv_nsec - t_ldpc_start.tv_nsec);
  }

  // Merge per-segment stats for legacy time_stats (used by OAI stats printout)
  for (int i = 0; i < n_dlsch; i++) {
    nrLDPC_TB_encoding_parameters_t *TB_parameters = &TBs[i];
    for (int r = 0; r < TB_parameters->C; r++) {
      nrLDPC_segment_encoding_parameters_t *segment_parameters = &TB_parameters->segments[r];
      merge_meas(dlsch_interleaving_stats, &segment_parameters->ts_interleave);
      merge_meas(dlsch_rate_matching_stats, &segment_parameters->ts_rate_match);
    }
  }

  // Update slot timing with encoding breakdown
  // Note: For ACC100, LDPC+RM+Interleave are all done in hardware (ldpc_total_ns)
  //       For software encoder, per-segment stats would be available but we use wall-clock time
  if (slot_timing_enabled) {
    current_slot_timing.encoding_crc_ns = crc_time_ns;
    current_slot_timing.encoding_segmentation_ns = seg_time_ns;
    current_slot_timing.encoding_ldpc_ns = ldpc_total_ns;  // Combined LDPC+RM+Interleave for ACC100
    current_slot_timing.encoding_rate_match_ns = 0;         // Included in ldpc_total_ns for ACC100
    current_slot_timing.encoding_interleave_ns = 0;         // Included in ldpc_total_ns for ACC100
  }

  VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_gNB_DLSCH_ENCODING, VCD_FUNCTION_OUT);
  return 0;
}
