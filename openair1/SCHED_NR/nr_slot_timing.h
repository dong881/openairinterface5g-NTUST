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

/*
 * \file nr_slot_timing.h
 * \brief Complete 500µs slot timing measurement for DL processing
 * \author OAI Team
 * \date 2024
 *
 * This header provides timing measurement for the ENTIRE DL slot processing chain:
 * - tx_func() entry to completion
 * - Includes MAC scheduling, PHY processing, RU processing, and fronthaul TX
 */

#ifndef __NR_SLOT_TIMING_H__
#define __NR_SLOT_TIMING_H__

#include <time.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

// Complete slot timing structure - covers ALL 500µs budget
typedef struct {
  // Timestamps
  struct timespec slot_start;      // When tx_func() starts
  struct timespec slot_end;        // When tx_func() returns (after fh_south_out)

  // Phase 1: MAC Scheduling (NR_slot_indication → gNB_dlsch_ulsch_scheduler)
  long slot_indication_ns;       // CSV column: mac_scheduler_ns

  // Phase 2: PHY Processing (phy_procedures_gNB_TX)
  long phy_proc_total_ns;          // Total phy_procedures_gNB_TX time
  long memory_clear_ns;            // Deprecated: now split into overlap + wait
  long memclear_wait_ns;           // Time waiting for memclear after encoding (critical path)
  long encoding_overlap_ns;        // Encoding time (runs parallel with memclear)
  long prs_gen_ns;
  long ssb_gen_ns;
  long pdcch_gen_ns;
  long pdsch_gen_ns;               // Total nr_generate_pdsch time
  long csirs_gen_ns;
  long phase_rot_ns;

  // Phase 2a: PDSCH sub-components (inside nr_generate_pdsch)
  long pdsch_encoding_ns;          // Total encoding time (CRC+Seg+LDPC+RM+Intlv)
  long pdsch_scrambling_ns;

  // Phase 2b: Encoding breakdown (inside nr_dlsch_encoding)
  long encoding_crc_ns;            // CRC attachment
  long encoding_segmentation_ns;   // Code block segmentation
  long encoding_ldpc_ns;           // LDPC encoding (ACC100 hardware)
  long encoding_rate_match_ns;     // Rate matching (CPU - main bottleneck)
  long encoding_interleave_ns;     // Interleaving
  long pdsch_modulation_ns;
  long pdsch_layer_mapping_ns;
  long pdsch_precoding_ns;         // Beamforming weights application
  long pdsch_re_mapping_ns;

  // Phase 3: RU Processing (ru_tx_func)
  long ru_tx_total_ns;             // Total ru_tx_func time
  long feptx_prec_ns;              // Copy txdataF to txdataF_BF
  long feptx_ofdm_ns;              // OFDM modulation (if done in DU)
  long fh_south_out_ns;            // Fronthaul TX to RU

  // Slot metadata
  int frame;
  int slot;
  int num_rbs;                     // Total RBs in PDSCH
  int mcs;                         // MCS index (-1 if no PDSCH)
  int num_layers;                  // MIMO layers
  int num_pdsch;                   // Number of PDSCH PDUs in slot
  int num_pdcch;                   // Number of PDCCH PDUs
  int slot_type;                   // DL/UL/Mixed

  // Total time
  long total_slot_ns;              // Complete slot processing time

  // Validity flag
  int valid;
} slot_timing_t;

// Thread-local storage for current slot timing
extern __thread slot_timing_t current_slot_timing;

// Global timing control
extern int slot_timing_enabled;

// Initialize timing log
void slot_timing_init(void);

// Cleanup timing log
void slot_timing_cleanup(void);

// Enable/disable timing measurement
void slot_timing_enable(void);
void slot_timing_disable(void);

// Start a new slot timing measurement
static inline void slot_timing_start(int frame, int slot) {
  if (!slot_timing_enabled) return;

  memset(&current_slot_timing, 0, sizeof(slot_timing_t));
  current_slot_timing.frame = frame;
  current_slot_timing.slot = slot;
  current_slot_timing.mcs = -1;  // Default: no PDSCH
  current_slot_timing.valid = 1;
  clock_gettime(CLOCK_MONOTONIC, &current_slot_timing.slot_start);
}

// End slot timing and write to log
void slot_timing_end_and_log(void);

// Helper: calculate time difference in nanoseconds
static inline long timespec_diff_ns_timing(struct timespec *start, struct timespec *end) {
  return (end->tv_sec - start->tv_sec) * 1000000000L + (end->tv_nsec - start->tv_nsec);
}

// Timing macros for easy instrumentation
#define SLOT_TIMING_START(frame, slot) slot_timing_start(frame, slot)
#define SLOT_TIMING_END() slot_timing_end_and_log()

#define TIMING_MEASURE_START(var) \
  struct timespec _t_##var##_start; \
  if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &_t_##var##_start)

#define TIMING_MEASURE_END(field) \
  do { \
    if (slot_timing_enabled) { \
      struct timespec _t_end; \
      clock_gettime(CLOCK_MONOTONIC, &_t_end); \
      current_slot_timing.field = timespec_diff_ns_timing(&_t_##field##_start, &_t_end); \
    } \
  } while(0)

#endif /* __NR_SLOT_TIMING_H__ */
