// NR Thread Pool Wrapper - Conditional compilation based on config
#ifndef NR_THREAD_POOL_WRAPPER_H
#define NR_THREAD_POOL_WRAPPER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nr_thread_pool_config.h"

#if NR_THREAD_POOL_OPTIMIZED_ENABLE
  // Use optimized thread pool
  #include "nr_thread_pool_optimized.h"

  // Type aliases for easy switching
  typedef nr_thread_pool_t nr_pool_t;
  typedef nr_task_t nr_pool_task_t;

  // Function aliases
  #define nr_pool_create(workers)           nr_thread_pool_create_optimized(workers)
  #define nr_pool_destroy(pool)             nr_thread_pool_destroy_optimized(pool)
  #define nr_pool_submit(pool, task)        nr_thread_pool_submit_optimized(pool, task)
  #define nr_pool_wait_completion(pool)     nr_thread_pool_wait_completion_optimized(pool)
  #define nr_pool_print_stats(pool)         nr_thread_pool_print_stats_optimized(pool)

#else
  // Thread pool disabled - provide dummy implementations

  typedef struct {
    int dummy;
  } nr_pool_t;

  typedef struct {
    void (*func)(void *);
    void *task_data;
  } nr_pool_task_t;

  // Dummy implementations that fall back to sequential processing
  static inline nr_pool_t* nr_pool_create(int workers) {
    (void)workers; // Suppress unused parameter warning
    printf("NR Thread Pool: DISABLED - using sequential processing\n");
    return (nr_pool_t*)1; // Return non-NULL to indicate "success"
  }

  static inline void nr_pool_destroy(nr_pool_t *pool) {
    (void)pool;
    printf("NR Thread Pool: DISABLED - no cleanup needed\n");
  }

  static inline bool nr_pool_submit(nr_pool_t *pool, nr_pool_task_t *task) {
    (void)pool;
    // Execute task immediately in calling thread
    if (task && task->func) {
      task->func(task);
    }
    return true;
  }

  static inline void nr_pool_wait_completion(nr_pool_t *pool) {
    (void)pool;
    // No-op when disabled - tasks already executed inline
  }

  static inline void nr_pool_print_stats(nr_pool_t *pool) {
    (void)pool;
    printf("NR Thread Pool Statistics: DISABLED\n");
  }

#endif // NR_THREAD_POOL_OPTIMIZED_ENABLE

// =========================================================================
// HIGH-LEVEL API FOR EASY INTEGRATION
// =========================================================================

// Global thread pool instance
extern nr_pool_t *nr_global_pool;

// Easy initialization functions
static inline bool nr_pool_init_global(void) {
#if NR_THREAD_POOL_OPTIMIZED_ENABLE
  if (nr_global_pool) {
    printf("Warning: Global NR thread pool already initialized\n");
    return true;
  }

  nr_global_pool = nr_pool_create(NR_THREAD_POOL_NUM_WORKERS);
  if (!nr_global_pool) {
    printf("Error: Failed to create global NR thread pool\n");
    return false;
  }

  printf("Global NR thread pool initialized with %d workers\n", NR_THREAD_POOL_NUM_WORKERS);
  return true;
#else
  nr_global_pool = nr_pool_create(0);
  return true;
#endif
}

static inline void nr_pool_cleanup_global(void) {
  if (nr_global_pool) {
#if NR_THREAD_POOL_OPTIMIZED_ENABLE
    nr_pool_print_stats(nr_global_pool);
#endif
    nr_pool_destroy(nr_global_pool);
    nr_global_pool = NULL;
  }
}

// =========================================================================
// PARALLEL PROCESSING WRAPPERS
// =========================================================================

// Memory initialization wrapper
static inline void nr_parallel_memory_init(int32_t ***txdataF, int num_beams, int num_antennas, int samples_per_slot) {
#if NR_THREAD_POOL_OPTIMIZED_ENABLE
  // Use thread pool if available
  if (nr_global_pool) {
    // TODO: Implement parallel memory initialization
    // For now, fall back to sequential
  }
#endif

  // Sequential fallback
  for (int beam = 0; beam < num_beams; beam++) {
    for (int aa = 0; aa < num_antennas; aa++) {
      memset(txdataF[beam][aa], 0, samples_per_slot * sizeof(int32_t));
    }
  }
}

// Signal processing wrapper
static inline void nr_parallel_signal_processing(void *msgTx, int frame, int slot) {
#if NR_THREAD_POOL_OPTIMIZED_ENABLE
  if (nr_global_pool) {
    printf("NR Thread Pool: Parallel signal processing for frame %d, slot %d\n", frame, slot);
    // TODO: Implement parallel signal processing
    // For now, fall back to sequential
  }
#endif

  // Sequential fallback - call original OAI functions
  printf("NR Thread Pool: Sequential signal processing for frame %d, slot %d\n", frame, slot);
  // Original phy_procedures_gNB_TX() calls would go here
}

#endif // NR_THREAD_POOL_WRAPPER_H