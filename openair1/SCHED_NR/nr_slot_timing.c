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

#include "nr_slot_timing.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "common/utils/LOG/log.h"

// Thread-local slot timing
__thread slot_timing_t current_slot_timing = {0};

// Global timing control
int slot_timing_enabled = 0;

// File and mutex for logging
static FILE *timing_log = NULL;
static pthread_mutex_t timing_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char *timing_log_path = "/home/kelvin/openairinterface5g/l1_slot_timing.csv";

void slot_timing_init(void) {
  if (timing_log) return;  // Already initialized

  pthread_mutex_lock(&timing_mutex);
  if (!timing_log) {
    timing_log = fopen(timing_log_path, "w");
    if (timing_log) {
      // CSV header with ALL timing fields
      fprintf(timing_log,
        "frame,slot,slot_type,num_pdcch,num_pdsch,num_rbs,mcs,num_layers,"
        // Phase 1: MAC Scheduling (includes gNB_dlsch_ulsch_scheduler)
        "mac_scheduler_ns,"
        // Phase 2: PHY total
        "phy_proc_total_ns,"
        // Phase 2 breakdown: Async overlap section
        "encoding_overlap_ns,memclear_wait_ns,memory_clear_ns,"
        // Phase 2 breakdown: Signal generation
        "prs_gen_ns,ssb_gen_ns,pdcch_gen_ns,pdsch_gen_ns,csirs_gen_ns,phase_rot_ns,"
        // PDSCH sub-components
        "pdsch_encoding_ns,pdsch_scrambling_ns,pdsch_modulation_ns,pdsch_layer_mapping_ns,pdsch_precoding_ns,pdsch_re_mapping_ns,"
        // Encoding breakdown (inside pdsch_encoding)
        "enc_crc_ns,enc_segmentation_ns,enc_ldpc_ns,enc_rate_match_ns,enc_interleave_ns,"
        // Phase 3: RU
        "ru_tx_total_ns,feptx_prec_ns,feptx_ofdm_ns,fh_south_out_ns,"
        // Total
        "total_slot_ns\n");
      fflush(timing_log);

      // Change ownership to kelvin user
      if (chown(timing_log_path, 1000, 1000) == 0) {
        chmod(timing_log_path, 0644);
      }
      LOG_I(PHY, "Slot timing measurement initialized: %s\n", timing_log_path);
    } else {
      LOG_E(PHY, "Failed to open slot timing log: %s\n", timing_log_path);
    }
  }
  pthread_mutex_unlock(&timing_mutex);
}

void slot_timing_cleanup(void) {
  pthread_mutex_lock(&timing_mutex);
  if (timing_log) {
    fclose(timing_log);
    timing_log = NULL;
    LOG_I(PHY, "Slot timing measurement closed\n");
  }
  pthread_mutex_unlock(&timing_mutex);
}

void slot_timing_enable(void) {
  slot_timing_enabled = 1;
  slot_timing_init();
  LOG_I(PHY, "Slot timing measurement enabled\n");
}

void slot_timing_disable(void) {
  slot_timing_enabled = 0;
  slot_timing_cleanup();
  LOG_I(PHY, "Slot timing measurement disabled\n");
}

void slot_timing_end_and_log(void) {
  if (!slot_timing_enabled || !current_slot_timing.valid) return;

  // Capture end time
  clock_gettime(CLOCK_MONOTONIC, &current_slot_timing.slot_end);
  current_slot_timing.total_slot_ns = timespec_diff_ns_timing(
    &current_slot_timing.slot_start,
    &current_slot_timing.slot_end);

  // Write to log
  if (timing_log) {
    pthread_mutex_lock(&timing_mutex);
    slot_timing_t *t = &current_slot_timing;

    fprintf(timing_log,
      "%d,%d,%d,%d,%d,%d,%d,%d,"   // frame,slot,slot_type,num_pdcch,num_pdsch,num_rbs,mcs,num_layers
      "%ld,"                        // slot_indication_ns
      "%ld,"                        // phy_proc_total_ns
      "%ld,%ld,%ld,"                // encoding_overlap,memclear_wait,memory_clear(total)
      "%ld,%ld,%ld,%ld,%ld,%ld,"    // prs,ssb,pdcch,pdsch,csirs,phase_rot
      "%ld,%ld,%ld,%ld,%ld,%ld,"    // pdsch sub-components
      "%ld,%ld,%ld,%ld,%ld,"        // encoding breakdown
      "%ld,%ld,%ld,%ld,"            // ru_tx_total,feptx_prec,feptx_ofdm,fh_south_out
      "%ld\n",                       // total_slot_ns
      t->frame, t->slot, t->slot_type, t->num_pdcch, t->num_pdsch,
      t->num_rbs, t->mcs, t->num_layers,
      t->slot_indication_ns,
      t->phy_proc_total_ns,
      t->encoding_overlap_ns, t->memclear_wait_ns, t->memory_clear_ns,
      t->prs_gen_ns, t->ssb_gen_ns, t->pdcch_gen_ns,
      t->pdsch_gen_ns, t->csirs_gen_ns, t->phase_rot_ns,
      t->pdsch_encoding_ns, t->pdsch_scrambling_ns, t->pdsch_modulation_ns,
      t->pdsch_layer_mapping_ns, t->pdsch_precoding_ns, t->pdsch_re_mapping_ns,
      t->encoding_crc_ns, t->encoding_segmentation_ns, t->encoding_ldpc_ns,
      t->encoding_rate_match_ns, t->encoding_interleave_ns,
      t->ru_tx_total_ns, t->feptx_prec_ns, t->feptx_ofdm_ns, t->fh_south_out_ns,
      t->total_slot_ns);

    fflush(timing_log);
    pthread_mutex_unlock(&timing_mutex);
  }

  // Reset for next slot
  current_slot_timing.valid = 0;
}
