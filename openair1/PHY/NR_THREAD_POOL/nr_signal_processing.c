// Phase-based signal processing for NR thread pool
#define _GNU_SOURCE
#include "nr_thread_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Include OAI headers for proper type definitions
#include "openair1/PHY/defs_gNB.h"
#include "openair1/SCHED_NR/sched_nr.h"
#include "openair1/PHY/NR_TRANSPORT/nr_transport_proto.h"

// Constants
#define NR_SYMBOLS_PER_SLOT 14
#define EXTENDED 1

// Signal processing task functions
static void nr_task_prs_generation(nr_task_t *task) {
  nr_signal_context_t *ctx = task->signal_ctx;
  // TODO: Implement PRS generation to private buffer
  printf("[Worker] PRS generation for slot %d\n", ctx->slot);
}

static void nr_task_ssb_generation(nr_task_t *task) {
  nr_signal_context_t *ctx = task->signal_ctx;
  // TODO: Implement SSB generation to private buffer
  printf("[Worker] SSB generation for slot %d\n", ctx->slot);
}

static void nr_task_pdcch_generation(nr_task_t *task) {
  nr_signal_context_t *ctx = task->signal_ctx;
  // TODO: Implement PDCCH generation to private buffer
  printf("[Worker] PDCCH generation for slot %d\n", ctx->slot);
}

static void nr_task_pdsch_generation(nr_task_t *task) {
  nr_signal_context_t *ctx = task->signal_ctx;
  // TODO: Implement PDSCH generation to private buffer (with ACC100)
  printf("[Worker] PDSCH generation for slot %d\n", ctx->slot);
}

static void nr_task_csi_rs_generation(nr_task_t *task) {
  nr_signal_context_t *ctx = task->signal_ctx;
  // TODO: Implement CSI-RS generation to private buffer
  printf("[Worker] CSI-RS generation for slot %d\n", ctx->slot);
}

// Main parallel slot processing function
void nr_parallel_slot_processing(nr_thread_pool_t *pool,
                                 processingData_L1tx_t *msgTx,
                                 int32_t ***final_txdataF,
                                 int frame, int slot) {
  if (!pool || !msgTx || !final_txdataF) return;

  printf("\n=== Starting parallel slot processing: Frame %d, Slot %d ===\n", frame, slot);

  // Phase 1: Clear memory (sequential, required first)
  start_meas(&msgTx->gNB->memory_clear_stats);
  nr_parallel_txdataF_init(final_txdataF, msgTx->gNB->common_vars.num_beams_period,
                          msgTx->gNB->frame_parms.nb_antennas_tx,
                          msgTx->gNB->frame_parms.samples_per_slot_wCP,
                          0);  // No offset for full buffer clear
  stop_meas(&msgTx->gNB->memory_clear_stats);

  // Phase 2: Parallel signal generation
  nr_signal_context_t signal_contexts[NR_MAX_SIGNAL_TYPES];
  int32_t ****private_buffers[NR_MAX_SIGNAL_TYPES] = {NULL};
  uint32_t buffer_ids[NR_MAX_SIGNAL_TYPES];
  int active_signals = 0;

  start_meas(&msgTx->gNB->signal_generation_stats);

  // Submit PRS generation
  for (int rsc_id = 0; rsc_id < msgTx->gNB->prs_vars.NumPRSResources; rsc_id++) {
    // Simplified PRS slot condition - implement proper check later
    if (slot % 4 == 0) {
      private_buffers[active_signals] = nr_buffer_pool_acquire(pool->buffer_pool, &buffer_ids[active_signals]);
      if (private_buffers[active_signals]) {
        signal_contexts[active_signals] = (nr_signal_context_t){
          .signal_type = NR_TASK_PRS_GENERATE,
          .signal_config = &msgTx->gNB->prs_vars.prs_cfg[rsc_id],
          .frame = frame,
          .slot = slot,
          .private_txdataF = private_buffers[active_signals],
          .num_beams = msgTx->gNB->common_vars.num_beams_period,
          .num_antennas = msgTx->gNB->frame_parms.nb_antennas_tx,
          .samples_per_slot = msgTx->gNB->frame_parms.samples_per_slot_wCP,
          .merge_barrier = &pool->buffer_merge_barrier,
          .merge_counter = &pool->pending_signals
        };
        nr_thread_pool_submit_signal(pool, &signal_contexts[active_signals]);
        active_signals++;
      }
    }
  }

  // Submit SSB generation
  for (int i = 0; i < msgTx->gNB->frame_parms.Lmax; i++) {
    if (msgTx->ssb[i].active) {
      private_buffers[active_signals] = nr_buffer_pool_acquire(pool->buffer_pool, &buffer_ids[active_signals]);
      if (private_buffers[active_signals]) {
        signal_contexts[active_signals] = (nr_signal_context_t){
          .signal_type = NR_TASK_SSB_GENERATE,
          .signal_config = &msgTx->ssb[i],
          .frame = frame,
          .slot = slot,
          .private_txdataF = private_buffers[active_signals],
          .num_beams = msgTx->gNB->common_vars.num_beams_period,
          .num_antennas = msgTx->gNB->frame_parms.nb_antennas_tx,
          .samples_per_slot = msgTx->gNB->frame_parms.samples_per_slot_wCP,
          .merge_barrier = &pool->buffer_merge_barrier,
          .merge_counter = &pool->pending_signals
        };
        nr_thread_pool_submit_signal(pool, &signal_contexts[active_signals]);
        active_signals++;
      }
    }
  }

  // Submit PDCCH generation
  if (msgTx->num_dl_pdcch > 0 || msgTx->num_ul_pdcch > 0) {
    private_buffers[active_signals] = nr_buffer_pool_acquire(pool->buffer_pool, &buffer_ids[active_signals]);
    if (private_buffers[active_signals]) {
      signal_contexts[active_signals] = (nr_signal_context_t){
        .signal_type = NR_TASK_PDCCH_GENERATE,
        .signal_config = msgTx,
        .frame = frame,
        .slot = slot,
        .private_txdataF = private_buffers[active_signals],
        .num_beams = msgTx->gNB->common_vars.num_beams_period,
        .num_antennas = msgTx->gNB->frame_parms.nb_antennas_tx,
        .samples_per_slot = msgTx->gNB->frame_parms.samples_per_slot_wCP,
        .merge_barrier = &pool->buffer_merge_barrier,
        .merge_counter = &pool->pending_signals
      };
      nr_thread_pool_submit_signal(pool, &signal_contexts[active_signals]);
      active_signals++;
    }
  }

  // Submit PDSCH generation
  if (msgTx->num_pdsch_slot > 0) {
    private_buffers[active_signals] = nr_buffer_pool_acquire(pool->buffer_pool, &buffer_ids[active_signals]);
    if (private_buffers[active_signals]) {
      signal_contexts[active_signals] = (nr_signal_context_t){
        .signal_type = NR_TASK_PDSCH_PIPELINE,
        .signal_config = msgTx,
        .frame = frame,
        .slot = slot,
        .private_txdataF = private_buffers[active_signals],
        .num_beams = msgTx->gNB->common_vars.num_beams_period,
        .num_antennas = msgTx->gNB->frame_parms.nb_antennas_tx,
        .samples_per_slot = msgTx->gNB->frame_parms.samples_per_slot_wCP,
        .merge_barrier = &pool->buffer_merge_barrier,
        .merge_counter = &pool->pending_signals
      };
      nr_thread_pool_submit_signal(pool, &signal_contexts[active_signals]);
      active_signals++;
    }
  }

  // Submit CSI-RS generation
  for (int i = 0; i < NR_SYMBOLS_PER_SLOT; i++) {
    if (msgTx->csirs_pdu[i].active == 1) {
      private_buffers[active_signals] = nr_buffer_pool_acquire(pool->buffer_pool, &buffer_ids[active_signals]);
      if (private_buffers[active_signals]) {
        signal_contexts[active_signals] = (nr_signal_context_t){
          .signal_type = NR_TASK_CSIRS_GENERATE,
          .signal_config = &msgTx->csirs_pdu[i],
          .frame = frame,
          .slot = slot,
          .private_txdataF = private_buffers[active_signals],
          .num_beams = msgTx->gNB->common_vars.num_beams_period,
          .num_antennas = msgTx->gNB->frame_parms.nb_antennas_tx,
          .samples_per_slot = msgTx->gNB->frame_parms.samples_per_slot_wCP,
          .merge_barrier = &pool->buffer_merge_barrier,
          .merge_counter = &pool->pending_signals
        };
        nr_thread_pool_submit_signal(pool, &signal_contexts[active_signals]);
        active_signals++;
      }
    }
  }

  // Wait for all signal generation to complete
  atomic_store(&pool->pending_signals, active_signals);
  pthread_barrier_wait(&pool->signal_generation_barrier);

  stop_meas(&msgTx->gNB->signal_generation_stats);

  // Phase 3: Buffer merging (sequential or parallel depending on implementation)
  start_meas(&msgTx->gNB->buffer_merge_stats);

  if (active_signals > 0) {
    nr_parallel_buffer_merge(final_txdataF, private_buffers, active_signals,
                            msgTx->gNB->common_vars.num_beams_period,
                            msgTx->gNB->frame_parms.nb_antennas_tx,
                            msgTx->gNB->frame_parms.samples_per_slot_wCP);

    // Release all private buffers
    for (int i = 0; i < active_signals; i++) {
      nr_buffer_pool_release(pool->buffer_pool, buffer_ids[i]);
    }
  }

  stop_meas(&msgTx->gNB->buffer_merge_stats);

  // Phase 4: Phase rotation (sequential, required last)
  start_meas(&msgTx->gNB->phase_comp_stats);
  if (msgTx->gNB->phase_comp) {
    for (int i = 0; i < msgTx->gNB->common_vars.num_beams_period; ++i) {
      for (int aa = 0; aa < msgTx->gNB->frame_parms.nb_antennas_tx; aa++) {
        apply_nr_rotation_TX(&msgTx->gNB->frame_parms,
                            &final_txdataF[i][aa][slot * msgTx->gNB->frame_parms.samples_per_slot_wCP],
                            msgTx->gNB->frame_parms.symbol_rotation[0],
                            slot,
                            msgTx->gNB->frame_parms.N_RB_DL,
                            0,
                            msgTx->gNB->frame_parms.Ncp == EXTENDED ? 12 : 14);
      }
    }
  }
  stop_meas(&msgTx->gNB->phase_comp_stats);

  printf("=== Parallel slot processing completed ===\n");
}

// Phase barrier synchronization
void nr_thread_pool_phase_barrier(nr_thread_pool_t *pool, int phase) {
  if (!pool) return;

  switch (phase) {
    case 0: // Signal generation phase
      pthread_barrier_wait(&pool->signal_generation_barrier);
      break;
    case 1: // Buffer merge phase
      pthread_barrier_wait(&pool->buffer_merge_barrier);
      break;
    case 2: // Phase rotation phase
      pthread_barrier_wait(&pool->phase_rotation_barrier);
      break;
    default:
      printf("Warning: Unknown phase %d\n", phase);
  }
}

// Create NUMA-aware thread pool
nr_thread_pool_t* nr_thread_pool_create_with_numa(int num_workers, bool numa_aware) {
  nr_thread_pool_t *pool = nr_thread_pool_create(num_workers);
  if (!pool) return NULL;

  if (numa_aware) {
    // Count workers per NUMA node
    for (int i = 0; i < pool->num_workers; i++) {
      int numa = pool->workers[i].numa_node;
      if (numa < 8) pool->workers_per_numa[numa]++;
    }

    // Create buffer pool with NUMA awareness
    pool->buffer_pool = nr_buffer_pool_create(
      NR_BUFFER_POOL_SIZE,
      4,  // num_beams
      4,  // num_antennas
      30720  // Default samples per slot for 30.72MHz
    );

    if (!pool->buffer_pool) {
      printf("Warning: Failed to create buffer pool\n");
    }

    printf("NUMA-aware thread pool created with %d workers\n", num_workers);
  }

  return pool;
}