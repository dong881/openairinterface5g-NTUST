/*
 * ISIP Thread Pool - Implementation
 *
 * Provides C wrapper around enkiTS task scheduler for OAI 5G
 * Creates 10 worker threads pinned to cores 6,8,9,10,11,13,14,15,16,17
 */

#define _GNU_SOURCE
#include "isip_pool.h"
#include "TaskScheduler_c.h"
#include "common/utils/LOG/log.h"
#include "PHY/MODULATION/nr_modulation.h"
#include "PHY/NR_REFSIG/nr_refsig_common.h"
#include "PHY/NR_TRANSPORT/nr_sch_dmrs.h"
#include "PHY/CODING/coding_defs.h"  // For crc24b
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdint.h>

// Global enkiTS scheduler instance
static enkiTaskScheduler* g_isip_scheduler = NULL;

// Memory clear task arguments (forward declaration for async storage)
typedef struct {
    void* buffer;       // Buffer to clear
    size_t size;        // Size in bytes
} memclear_task_args_t;

// Pre-created task set for synchronous memory clear (reused every slot)
static enkiTaskSet* g_memclear_task = NULL;

// Ping-pong async memory clear: two independent task sets and arg buffers
// This avoids race condition when consecutive slots both start async clears
static enkiTaskSet* g_memclear_async_task[2] = {NULL, NULL};  // Ping-pong task sets
static volatile int g_memclear_async_in_progress[2] = {0, 0}; // Track each buffer
static memclear_task_args_t g_async_task_args[2][32];         // Ping-pong arg buffers
static int g_async_buffer_idx = 0;                            // Current buffer (0 or 1)

// Core assignment for ISIP thread pool workers
// Avoid cores 1,3,5,7 used by OAI thread pool (--thread-pool 1,3,5,7)
static const int ISIP_CORES[10] = {6, 8, 9, 10, 11, 13, 14, 15, 16, 17};

/**
 * Thread start callback for core binding
 * Called by enkiTS when each worker thread starts
 */
static void isip_thread_start(uint32_t threadNum) {
    if (threadNum < 10) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(ISIP_CORES[threadNum], &cpuset);

        if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0) {
            LOG_I(PHY, "[ISIP thread pool] Thread %d pinned to core %d\n",
                  threadNum, ISIP_CORES[threadNum]);
        } else {
            LOG_E(PHY, "[ISIP thread pool] Failed to pin thread %d to core %d\n",
                  threadNum, ISIP_CORES[threadNum]);
        }
    }
}

// NOTE: NT stores tested but caused 2x slowdown in RE Mapping due to cache misses
// Regular memset is better because buffer is immediately reused by signal generation
static void memclear_task_func(uint32_t start, uint32_t end, uint32_t threadNum, void* pArgs) {
    memclear_task_args_t* args = (memclear_task_args_t*)pArgs;
    memset(args[start].buffer, 0, args[start].size);
}

int isip_pool_init(void) {
    if (g_isip_scheduler) {
        LOG_W(PHY, "[ISIP thread pool] Already initialized\n");
        return 1;
    }

    LOG_I(PHY, "[ISIP thread pool] Initializing with 10 threads on cores 6,8,9,10,11,13,14,15,16,17\n");

    // Create enkiTS scheduler
    g_isip_scheduler = enkiNewTaskScheduler();
    if (!g_isip_scheduler) {
        LOG_E(PHY, "[ISIP thread pool] Failed to create task scheduler\n");
        return 0;
    }

    // Configure scheduler for 10 threads with core binding + external threads
    struct enkiTaskSchedulerConfig config = enkiGetTaskSchedulerConfig(g_isip_scheduler);
    config.numTaskThreadsToCreate = 10;  // Exactly 10 worker threads
    config.numExternalTaskThreads = 4;  // Allow external threads (L1_tx, L1_rx, etc.) to submit tasks
    config.profilerCallbacks.threadStart = isip_thread_start;

    // Initialize with configuration
    enkiInitTaskSchedulerWithConfig(g_isip_scheduler, config);

    LOG_I(PHY, "[ISIP thread pool] Successfully initialized with %d threads\n",
          enkiGetNumTaskThreads(g_isip_scheduler));

    // Pre-create memory clear task set (will be reused for every slot)
    g_memclear_task = enkiCreateTaskSet(g_isip_scheduler, memclear_task_func);
    if (!g_memclear_task) {
        LOG_W(PHY, "[ISIP thread pool] Failed to pre-create memclear task\n");
    } else {
        LOG_I(PHY, "[ISIP thread pool] Pre-created memclear task for real-time reuse\n");
    }

    // Pre-create async memory clear task sets (ping-pong pattern for deferred clearing)
    g_memclear_async_task[0] = enkiCreateTaskSet(g_isip_scheduler, memclear_task_func);
    g_memclear_async_task[1] = enkiCreateTaskSet(g_isip_scheduler, memclear_task_func);
    if (!g_memclear_async_task[0] || !g_memclear_async_task[1]) {
        LOG_W(PHY, "[ISIP thread pool] Failed to pre-create async memclear tasks\n");
    } else {
        LOG_I(PHY, "[ISIP thread pool] Pre-created ping-pong async memclear tasks for deferred clearing\n");
    }

    if (g_memclear_task) {

        // TEST: Execute memclear from startup thread (NOT L1_tx_thread)
        LOG_I(PHY, "[ISIP thread pool] Testing memclear from startup thread...\n");

        // Allocate test buffers (simulate 1 beam x 4 antennas)
        void* test_buffers[1][4];
        for (int ant = 0; ant < 4; ant++) {
            test_buffers[0][ant] = malloc(57344 * sizeof(int32_t)); // 1 slot worth
            if (!test_buffers[0][ant]) {
                LOG_E(PHY, "[ISIP thread pool] Failed to allocate test buffer\n");
                return 1;
            }
        }

        // Call memclear using enkiTS (same code that will run in L1_tx_thread)
        static memclear_task_args_t task_args[4];
        for (int ant = 0; ant < 4; ant++) {
            task_args[ant].buffer = test_buffers[0][ant];
            task_args[ant].size = 57344 * sizeof(int32_t);
        }

        enkiAddTaskSetMinRange(g_isip_scheduler, g_memclear_task, task_args, 4, 1);
        enkiWaitForTaskSet(g_isip_scheduler, g_memclear_task);

        LOG_I(PHY, "[ISIP thread pool] Memclear test from startup thread PASSED\n");

        // Cleanup test buffers
        for (int ant = 0; ant < 4; ant++) {
            free(test_buffers[0][ant]);
        }
    }

    return 1;
}

void isip_pool_shutdown(void) {
    if (g_isip_scheduler) {
        LOG_I(PHY, "[ISIP thread pool] Shutting down task scheduler\n");

        // Delete pre-created tasks
        if (g_memclear_task) {
            enkiDeleteTaskSet(g_isip_scheduler, g_memclear_task);
            g_memclear_task = NULL;
        }
        // Delete ping-pong async task sets
        for (int i = 0; i < 2; i++) {
            if (g_memclear_async_task[i]) {
                enkiDeleteTaskSet(g_isip_scheduler, g_memclear_async_task[i]);
                g_memclear_async_task[i] = NULL;
            }
            g_memclear_async_in_progress[i] = 0;
        }
        g_async_buffer_idx = 0;

        // Wait for all tasks to complete and shutdown
        enkiWaitforAllAndShutdown(g_isip_scheduler);

        // Delete scheduler and cleanup
        enkiDeleteTaskScheduler(g_isip_scheduler);
        g_isip_scheduler = NULL;

        LOG_I(PHY, "[ISIP thread pool] Shutdown completed\n");
    }
}

/**
 * Test task function for verification
 */
static void test_task_func(uint32_t start, uint32_t end, uint32_t threadNum, void* pArgs) {
    int* results = (int*)pArgs;

    // Simple computation to verify thread is working
    for (uint32_t i = start; i < end; i++) {
        results[i] = threadNum * 1000 + i;
    }
}

void isip_pool_test(void) {
    if (!g_isip_scheduler) {
        LOG_W(PHY, "[ISIP thread pool] Cannot run test - scheduler not initialized\n");
        return;
    }

    LOG_I(PHY, "[ISIP thread pool] Running basic verification test\n");

    // Simple test parameters
    const int test_size = 80;
    int results[test_size];
    memset(results, 0, sizeof(results));

    // Create test task set
    enkiTaskSet* test_task = enkiCreateTaskSet(g_isip_scheduler, test_task_func);
    if (!test_task) {
        LOG_E(PHY, "[ISIP thread pool] Failed to create test task\n");
        return;
    }

    // Execute test task with parameters
    enkiAddTaskSetMinRange(g_isip_scheduler, test_task, results, test_size, 10);
    enkiWaitForTaskSet(g_isip_scheduler, test_task);

    // Cleanup test task
    enkiDeleteTaskSet(g_isip_scheduler, test_task);

    // Verify results
    int success = 1;
    for (int i = 0; i < test_size; i++) {
        if (results[i] == 0) {
            success = 0;
            break;
        }
    }

    if (success) {
        LOG_I(PHY, "[ISIP thread pool] Basic test PASSED - thread pool is working\n");
    } else {
        LOG_E(PHY, "[ISIP thread pool] Basic test FAILED\n");
    }
}

int isip_pool_is_initialized(void) {
    return (g_isip_scheduler != NULL);
}

int isip_pool_get_num_threads(void) {
    if (!g_isip_scheduler) {
        return 0;
    }
    return enkiGetNumTaskThreads(g_isip_scheduler);
}

void* isip_pool_get_scheduler(void) {
    return (void*)g_isip_scheduler;
}

int isip_pool_register_external_thread(void) {
    if (!g_isip_scheduler) {
        return 0;
    }

    int result = enkiRegisterExternalTaskThread(g_isip_scheduler);
    if (result) {
        LOG_I(PHY, "[ISIP thread pool] External thread registered successfully\n");
    } else {
        LOG_E(PHY, "[ISIP thread pool] Failed to register external thread\n");
    }
    return result;
}

void isip_pool_deregister_external_thread(void) {
    if (g_isip_scheduler) {
        enkiDeRegisterExternalTaskThread(g_isip_scheduler);
        LOG_I(PHY, "[ISIP thread pool] External thread deregistered\n");
    }
}

void isip_pool_memclear_tx(void*** txdataF, int num_beams, int num_antennas,
                             int offset, int samples_per_slot) {
    if (!g_isip_scheduler || !g_memclear_task) {
        // Fallback to sequential
        for (int beam = 0; beam < num_beams; beam++) {
            for (int ant = 0; ant < num_antennas; ant++) {
                void* buffer = (char*)txdataF[beam][ant] + offset * sizeof(int32_t);
                memset(buffer, 0, samples_per_slot * sizeof(int32_t));
            }
        }
        return;
    }

    // Split each antenna buffer into 2 tasks for 8 total tasks (4 antennas x 2)
    // This fully utilizes all 8 worker threads
    const int tasks_per_antenna = 2;
    int total_tasks = num_beams * num_antennas * tasks_per_antenna;

    if (total_tasks > 32 || total_tasks <= 0) {
        // Fallback to sequential
        for (int beam = 0; beam < num_beams; beam++) {
            for (int ant = 0; ant < num_antennas; ant++) {
                void* buffer = (char*)txdataF[beam][ant] + offset * sizeof(int32_t);
                memset(buffer, 0, samples_per_slot * sizeof(int32_t));
            }
        }
        return;
    }

    static memclear_task_args_t task_args[32];

    int task_idx = 0;
    for (int beam = 0; beam < num_beams; beam++) {
        for (int ant = 0; ant < num_antennas; ant++) {
            if (!txdataF[beam][ant]) {
                // NULL pointer - fall back to sequential
                for (int b = 0; b < num_beams; b++) {
                    for (int a = 0; a < num_antennas; a++) {
                        if (txdataF[b][a]) {
                            void* buffer = (char*)txdataF[b][a] + offset * sizeof(int32_t);
                            memset(buffer, 0, samples_per_slot * sizeof(int32_t));
                        }
                    }
                }
                return;
            }

            // Split antenna buffer into 2 halves for better parallelization
            int half_samples = samples_per_slot / 2;

            // First half
            task_args[task_idx].buffer = (char*)txdataF[beam][ant] + offset * sizeof(int32_t);
            task_args[task_idx].size = half_samples * sizeof(int32_t);
            task_idx++;

            // Second half
            task_args[task_idx].buffer = (char*)txdataF[beam][ant] +
                                          (offset + half_samples) * sizeof(int32_t);
            task_args[task_idx].size = (samples_per_slot - half_samples) * sizeof(int32_t);
            task_idx++;
        }
    }

    // Execute parallel memclear using enkiTS with 8 tasks
    enkiAddTaskSetMinRange(g_isip_scheduler, g_memclear_task, task_args, total_tasks, 1);
    enkiWaitForTaskSet(g_isip_scheduler, g_memclear_task);
}

// ============================================================================
// Async memory clear for deferred clearing (removes memclear from critical path)
// ============================================================================

void isip_pool_memclear_tx_async(void*** txdataF, int num_beams, int num_antennas,
                                   int offset, int samples_per_slot) {
    if (!g_isip_scheduler || !g_memclear_async_task[0] || !g_memclear_async_task[1]) {
        // No async support - fallback to sync
        isip_pool_memclear_tx(txdataF, num_beams, num_antennas, offset, samples_per_slot);
        return;
    }

    // Use ping-pong pattern: alternate between buffer 0 and 1
    // This avoids race condition when consecutive slots both start async clears
    int current_buf = g_async_buffer_idx;
    int next_buf = 1 - current_buf;  // Toggle: 0->1 or 1->0

    // If the buffer we're about to use still has a task in progress, wait for it
    if (g_memclear_async_in_progress[next_buf]) {
        enkiWaitForTaskSet(g_isip_scheduler, g_memclear_async_task[next_buf]);
        g_memclear_async_in_progress[next_buf] = 0;
    }

    // Switch to the next buffer for this async operation
    g_async_buffer_idx = next_buf;

    const int tasks_per_antenna = 2;
    int total_tasks = num_beams * num_antennas * tasks_per_antenna;

    if (total_tasks > 32 || total_tasks <= 0) {
        // Too many tasks - fallback to sync
        isip_pool_memclear_tx(txdataF, num_beams, num_antennas, offset, samples_per_slot);
        return;
    }

    // Prepare task arguments in ping-pong buffer (each buffer persists independently)
    memclear_task_args_t* args = g_async_task_args[next_buf];
    int task_idx = 0;
    for (int beam = 0; beam < num_beams; beam++) {
        for (int ant = 0; ant < num_antennas; ant++) {
            if (!txdataF[beam][ant]) {
                // NULL pointer - fallback to sync
                isip_pool_memclear_tx(txdataF, num_beams, num_antennas, offset, samples_per_slot);
                return;
            }

            int half_samples = samples_per_slot / 2;

            // First half
            args[task_idx].buffer = (char*)txdataF[beam][ant] + offset * sizeof(int32_t);
            args[task_idx].size = half_samples * sizeof(int32_t);
            task_idx++;

            // Second half
            args[task_idx].buffer = (char*)txdataF[beam][ant] +
                                    (offset + half_samples) * sizeof(int32_t);
            args[task_idx].size = (samples_per_slot - half_samples) * sizeof(int32_t);
            task_idx++;
        }
    }

    // Start async memclear on the next_buf task set - DO NOT wait for completion
    g_memclear_async_in_progress[next_buf] = 1;
    enkiAddTaskSetMinRange(g_isip_scheduler, g_memclear_async_task[next_buf], args, total_tasks, 1);
    // No enkiWaitForTaskSet here - that's what makes it async!
}

void isip_pool_memclear_tx_wait(void) {
    if (!g_isip_scheduler) return;

    // Wait for any in-progress async clears in both ping-pong buffers
    for (int i = 0; i < 2; i++) {
        if (g_memclear_async_in_progress[i] && g_memclear_async_task[i]) {
            enkiWaitForTaskSet(g_isip_scheduler, g_memclear_async_task[i]);
            g_memclear_async_in_progress[i] = 0;
        }
    }
}

// Memcpy task arguments
typedef struct {
    void* dst;          // Destination buffer
    const void* src;    // Source buffer
    size_t size;        // Size in bytes
} memcpy_task_args_t;

// Pre-created task set for memcpy (reused every slot)
static enkiTaskSet* g_memcpy_task = NULL;

// Memcpy task function - each task copies one buffer segment
static void memcpy_task_func(uint32_t start, uint32_t end, uint32_t threadNum, void* pArgs) {
    memcpy_task_args_t* args = (memcpy_task_args_t*)pArgs;
    for (uint32_t i = start; i < end; i++) {
        memcpy(args[i].dst, args[i].src, args[i].size);
    }
}

void isip_pool_memcpy_tx(void** dst_buffers, void*** src_buffers,
                           int num_beams, int num_antennas,
                           int src_offset, int samples_per_slot) {
    // Create task set on first use
    if (!g_memcpy_task && g_isip_scheduler) {
        g_memcpy_task = enkiCreateTaskSet(g_isip_scheduler, memcpy_task_func);
        if (g_memcpy_task) {
            LOG_I(PHY, "[ISIP thread pool] Created memcpy task for feptx_prec acceleration\n");
        }
    }

    if (!g_isip_scheduler || !g_memcpy_task) {
        // Fallback to sequential memcpy
        for (int beam = 0; beam < num_beams; beam++) {
            for (int ant = 0; ant < num_antennas; ant++) {
                int tx_idx = ant + beam * num_antennas;
                void* dst = dst_buffers[tx_idx];
                const void* src = (const char*)src_buffers[beam][ant] + src_offset * sizeof(int32_t);
                memcpy(dst, src, samples_per_slot * sizeof(int32_t));
            }
        }
        return;
    }

    int total_antennas = num_beams * num_antennas;

    // Each antenna is one copy task (4 tasks for 4 antennas)
    // enkiTS will distribute across 8 workers
    static memcpy_task_args_t task_args[16];  // Max 16 antennas

    if (total_antennas > 16 || total_antennas <= 0) {
        // Fallback
        for (int beam = 0; beam < num_beams; beam++) {
            for (int ant = 0; ant < num_antennas; ant++) {
                int tx_idx = ant + beam * num_antennas;
                void* dst = dst_buffers[tx_idx];
                const void* src = (const char*)src_buffers[beam][ant] + src_offset * sizeof(int32_t);
                memcpy(dst, src, samples_per_slot * sizeof(int32_t));
            }
        }
        return;
    }

    int task_idx = 0;
    for (int beam = 0; beam < num_beams; beam++) {
        for (int ant = 0; ant < num_antennas; ant++) {
            int tx_idx = ant + beam * num_antennas;
            task_args[task_idx].dst = dst_buffers[tx_idx];
            task_args[task_idx].src = (const char*)src_buffers[beam][ant] + src_offset * sizeof(int32_t);
            task_args[task_idx].size = samples_per_slot * sizeof(int32_t);
            task_idx++;
        }
    }

    // Execute parallel memcpy
    enkiAddTaskSetMinRange(g_isip_scheduler, g_memcpy_task, task_args, total_antennas, 1);
    enkiWaitForTaskSet(g_isip_scheduler, g_memcpy_task);
}

// ============================================================================
// DMRS Precompute Functions
// ============================================================================

#define DMRS_MOD_ORDER 2  // QPSK for DMRS

// Global DMRS precompute state (ping-pong pattern like memclear)
static enkiTaskSet* g_dmrs_precompute_task[2] = {NULL, NULL};
static volatile int g_dmrs_precompute_in_progress[2] = {0, 0};
static isip_dmrs_params_t* g_dmrs_params_ptr[2] = {NULL, NULL};
static int g_dmrs_num_pdsch[2] = {0, 0};
static int g_dmrs_buffer_idx = 0;

// DMRS precompute task function - processes one PDSCH (all its DMRS symbols)
static void dmrs_precompute_task_func(uint32_t start, uint32_t end, uint32_t threadNum, void* pArgs) {
    isip_dmrs_params_t* all_params = (isip_dmrs_params_t*)pArgs;

    for (uint32_t pdsch_idx = start; pdsch_idx < end; pdsch_idx++) {
        isip_dmrs_params_t* params = &all_params[pdsch_idx];

        // Compute l_overline (first DMRS symbol position)
        int l_overline = get_l0(params->dmrs_symbol_map);
        int num_precomputed = 0;

        // Iterate through all symbols and compute DMRS for DMRS symbols
        for (int l_symbol = params->StartSymbolIndex;
             l_symbol < params->StartSymbolIndex + params->NrOfSymbols && num_precomputed < ISIP_MAX_DMRS_SYMBOLS;
             l_symbol++) {

            if (params->dmrs_symbol_map & (1 << l_symbol)) {
                // Compute l_prime for this symbol
                int l_prime = 0;
                if (l_symbol == (l_overline + 1)) {
                    l_prime = 1;
                } else if (l_symbol > (l_overline + 1)) {
                    l_overline = l_symbol;
                    l_prime = 0;
                }

                // Get gold sequence for this DMRS symbol
                const uint32_t *gold = nr_gold_pdsch(params->N_RB_DL,
                                                     params->symbols_per_slot,
                                                     params->dlDmrsScramblingId,
                                                     params->SCID,
                                                     params->slot,
                                                     l_symbol);

                // Compute output buffer location for this DMRS symbol
                isip_c16_t *out_ptr = params->mod_dmrs_out + num_precomputed * params->dmrs_buf_stride;

                // Modulate DMRS (QPSK)
                nr_modulation(gold,
                              params->n_dmrs * DMRS_MOD_ORDER,
                              DMRS_MOD_ORDER,
                              (int16_t *)out_ptr);

                // Store metadata
                params->symbol_indices_out[num_precomputed] = l_symbol;
                params->l_prime_out[num_precomputed] = l_prime;
                num_precomputed++;
            }
        }

        params->num_precomputed = num_precomputed;
    }
}

void isip_pool_dmrs_precompute_async(isip_dmrs_params_t *params, int num_pdsch) {
    if (!g_isip_scheduler || num_pdsch <= 0 || num_pdsch > ISIP_MAX_PDSCH_PER_SLOT) {
        // No scheduler or invalid params - compute synchronously
        if (params && num_pdsch > 0) {
            dmrs_precompute_task_func(0, num_pdsch, 0, params);
        }
        return;
    }

    // Create task sets on first use
    if (!g_dmrs_precompute_task[0]) {
        g_dmrs_precompute_task[0] = enkiCreateTaskSet(g_isip_scheduler, dmrs_precompute_task_func);
        g_dmrs_precompute_task[1] = enkiCreateTaskSet(g_isip_scheduler, dmrs_precompute_task_func);
        if (g_dmrs_precompute_task[0] && g_dmrs_precompute_task[1]) {
            LOG_I(PHY, "[ISIP thread pool] Created DMRS precompute task sets\n");
        }
    }

    if (!g_dmrs_precompute_task[0] || !g_dmrs_precompute_task[1]) {
        // Fallback to sync
        dmrs_precompute_task_func(0, num_pdsch, 0, params);
        return;
    }

    // Use ping-pong pattern
    int current_buf = g_dmrs_buffer_idx;
    int next_buf = 1 - current_buf;

    // Wait for any in-progress task on the buffer we're about to use
    if (g_dmrs_precompute_in_progress[next_buf]) {
        enkiWaitForTaskSet(g_isip_scheduler, g_dmrs_precompute_task[next_buf]);
        g_dmrs_precompute_in_progress[next_buf] = 0;
    }

    // Switch to next buffer
    g_dmrs_buffer_idx = next_buf;

    // Store params pointer for this async operation
    g_dmrs_params_ptr[next_buf] = params;
    g_dmrs_num_pdsch[next_buf] = num_pdsch;

    // Start async DMRS precompute - each PDSCH is one task
    g_dmrs_precompute_in_progress[next_buf] = 1;
    enkiAddTaskSetMinRange(g_isip_scheduler, g_dmrs_precompute_task[next_buf], params, num_pdsch, 1);
    // No wait - that's what makes it async!
}

void isip_pool_dmrs_precompute_wait(void) {
    if (!g_isip_scheduler) return;

    // Wait for any in-progress DMRS precompute in both ping-pong buffers
    for (int i = 0; i < 2; i++) {
        if (g_dmrs_precompute_in_progress[i] && g_dmrs_precompute_task[i]) {
            enkiWaitForTaskSet(g_isip_scheduler, g_dmrs_precompute_task[i]);
            g_dmrs_precompute_in_progress[i] = 0;
        }
    }
}

int isip_pool_dmrs_precompute_enabled(void) {
    return (g_isip_scheduler != NULL);
}

// ============================================================================
// Parallel Segmentation Functions
// ============================================================================

// Per-segment task arguments
typedef struct {
    const uint8_t *src;         // Source pointer in input buffer
    uint8_t *dst;               // Destination: harq->c[r]
    uint32_t data_bytes;        // (Kprime - L) >> 3
    uint32_t data_bits;         // Kprime - L (actual bit length for CRC)
    uint32_t filler_start;      // Kprime >> 3
    uint32_t filler_end;        // K >> 3
    int needs_crc24b;           // 1 if C > 1
} seg_task_args_t;

// Pre-created task set for segmentation (reused)
static enkiTaskSet* g_seg_task = NULL;

// Segmentation task function - processes one segment
// Each task: memcpy segment data, compute CRC24B if needed, zero filler bits
static void segmentation_task_func(uint32_t start, uint32_t end, uint32_t threadNum, void* pArgs) {
    seg_task_args_t* args = (seg_task_args_t*)pArgs;

    for (uint32_t i = start; i < end; i++) {
        seg_task_args_t* task = &args[i];

        // 1. Copy segment data from input to output buffer
        memcpy(task->dst, task->src, task->data_bytes);

        // 2. Compute and append CRC24B if multiple segments (C > 1)
        if (task->needs_crc24b) {
            uint32_t crc = crc24b(task->dst, task->data_bits) >> 8;
            task->dst[task->data_bytes]     = ((uint8_t*)&crc)[2];
            task->dst[task->data_bytes + 1] = ((uint8_t*)&crc)[1];
            task->dst[task->data_bytes + 2] = ((uint8_t*)&crc)[0];
        }

        // 3. Zero filler bits
        if (task->filler_end > task->filler_start) {
            memset(&task->dst[task->filler_start], 0,
                   task->filler_end - task->filler_start);
        }
    }
}

// Sequential fallback for single segment or when ISIP pool unavailable
static void segmentation_sequential(isip_seg_params_t *params) {
    uint32_t data_bits = params->Kprime - params->L;
    uint32_t data_bytes = data_bits >> 3;
    uint32_t src_offset = 0;

    for (uint32_t r = 0; r < params->C; r++) {
        // Copy segment data
        memcpy(params->outputs[r], params->input + src_offset, data_bytes);
        src_offset += data_bytes;

        // CRC24B for multi-segment TB
        if (params->C > 1) {
            uint32_t crc = crc24b(params->outputs[r], data_bits) >> 8;
            params->outputs[r][data_bytes]     = ((uint8_t*)&crc)[2];
            params->outputs[r][data_bytes + 1] = ((uint8_t*)&crc)[1];
            params->outputs[r][data_bytes + 2] = ((uint8_t*)&crc)[0];
        }

        // Zero filler bits
        uint32_t filler_start = params->Kprime >> 3;
        uint32_t filler_end = params->K >> 3;
        if (filler_end > filler_start) {
            memset(&params->outputs[r][filler_start], 0, filler_end - filler_start);
        }
    }
}

void isip_pool_segmentation_parallel(isip_seg_params_t *params) {
    if (!params || params->C == 0) {
        return;
    }

    // For single segment, use sequential (no parallelization benefit)
    if (params->C == 1 || !g_isip_scheduler) {
        segmentation_sequential(params);
        params->completed = 1;
        return;
    }

    // Create task set on first use
    if (!g_seg_task) {
        g_seg_task = enkiCreateTaskSet(g_isip_scheduler, segmentation_task_func);
        if (g_seg_task) {
            LOG_I(PHY, "[ISIP thread pool] Created segmentation task for parallel CRC+copy\n");
        }
    }

    if (!g_seg_task) {
        // Fallback to sequential
        segmentation_sequential(params);
        params->completed = 1;
        return;
    }

    // Prepare per-segment task arguments
    static seg_task_args_t task_args[ISIP_MAX_SEGMENTS];

    if (params->C > ISIP_MAX_SEGMENTS) {
        LOG_E(PHY, "[ISIP thread pool] Too many segments (%d > %d), fallback to sequential\n",
              params->C, ISIP_MAX_SEGMENTS);
        segmentation_sequential(params);
        params->completed = 1;
        return;
    }

    uint32_t data_bits = params->Kprime - params->L;
    uint32_t data_bytes = data_bits >> 3;
    uint32_t src_offset = 0;
    int needs_crc = (params->C > 1) ? 1 : 0;

    for (uint32_t r = 0; r < params->C; r++) {
        task_args[r].src = params->input + src_offset;
        task_args[r].dst = params->outputs[r];
        task_args[r].data_bytes = data_bytes;
        task_args[r].data_bits = data_bits;
        task_args[r].filler_start = params->Kprime >> 3;
        task_args[r].filler_end = params->K >> 3;
        task_args[r].needs_crc24b = needs_crc;
        src_offset += data_bytes;
    }

    // Execute parallel segmentation with COARSE-GRAINED parallelism
    // Use min_range to batch multiple segments per task, reducing task overhead
    // Goal: ~3-4 tasks max (task overhead ~4-5us, per-segment work ~3us)
    // With 7 segments and min_range=3: 3 tasks instead of 7
    uint32_t min_range = (params->C + 2) / 3;  // ceil(C/3) -> max ~3 tasks
    if (min_range < 2) min_range = 2;  // At least 2 segments per task

    enkiAddTaskSetMinRange(g_isip_scheduler, g_seg_task, task_args, params->C, min_range);
    enkiWaitForTaskSet(g_isip_scheduler, g_seg_task);

    params->completed = 1;
}

int isip_pool_segmentation_enabled(void) {
    return (g_isip_scheduler != NULL);
}
