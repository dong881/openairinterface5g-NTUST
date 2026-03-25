#ifndef NR_THREAD_POOL_H
#define NR_THREAD_POOL_H

// =========================================================================
// OPTIMIZED NR THREAD POOL FOR BEST PHY DOWNLINK PERFORMANCE
// =========================================================================
//
// Clean optimized implementation with work-stealing queues, event-driven
// workers, CPU affinity, and lock-free operations for maximum performance.
//
// Features: Work stealing: ON, Event-driven: ON, CPU affinity: ON
// =========================================================================

#ifndef NR_THREAD_POOL_ENABLE
#define NR_THREAD_POOL_ENABLE 1  // ENABLED by default for performance
#endif

#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include "openair1/PHY/TOOLS/tools_defs.h"

// Optimized configuration constants
#define NR_MAX_WORKER_THREADS    16   // Up to 16 workers
#define NR_MAX_TASKS_PER_QUEUE   256  // Large queue for high throughput
#define NR_CACHE_LINE_SIZE       64   // Cache alignment for performance

// Core API structures
typedef struct nr_thread_pool nr_thread_pool_t;

// Basic task types
typedef enum {
  NR_TASK_MEMORY_INIT = 0,
  NR_TASK_SIGNAL_PROCESSING,
  NR_TASK_BUFFER_MERGE,
  NR_TASK_MAX
} nr_task_type_t;

// Simplified task structure (cache-line aligned)
typedef struct nr_task {
  nr_task_type_t type;
  void (*func)(struct nr_task *task);
  void *task_data;
  _Atomic(bool) completed;
  uint64_t submit_time;
} __attribute__((aligned(NR_CACHE_LINE_SIZE))) nr_task_t;

// Lock-free task queue
typedef struct {
  _Atomic(uint64_t) head __attribute__((aligned(NR_CACHE_LINE_SIZE)));
  _Atomic(uint64_t) tail __attribute__((aligned(NR_CACHE_LINE_SIZE)));
  nr_task_t tasks[NR_MAX_TASKS_PER_QUEUE];
  char padding[NR_CACHE_LINE_SIZE];
} nr_task_queue_t;

// Optimized worker thread
typedef struct {
  pthread_t thread;
  int worker_id;
  int cpu_core;
  nr_thread_pool_t *pool;

  // Event-driven optimization
  pthread_cond_t work_available_cond;
  pthread_mutex_t work_mutex;
  _Atomic(bool) has_work;

  // Performance statistics
  uint64_t tasks_executed;
  uint64_t tasks_stolen;
  uint64_t idle_cycles;

} nr_worker_thread_t;

// Main optimized thread pool structure
typedef struct nr_thread_pool {
  // Worker threads
  nr_worker_thread_t workers[NR_MAX_WORKER_THREADS];
  int num_workers;

  // Per-worker task queues (lockless)
  nr_task_queue_t worker_queues[NR_MAX_WORKER_THREADS];

  // Pool state
  _Atomic(bool) shutdown_requested;
  _Atomic(int) active_tasks;

  // Basic synchronization
  pthread_cond_t work_available;
  pthread_mutex_t pool_mutex;

  // Performance monitoring
  _Atomic(uint64_t) total_tasks_completed;
  _Atomic(uint64_t) total_work_stealing_events;

} nr_thread_pool_t;

// Core API functions - maintain compatibility with existing OAI integration
nr_thread_pool_t* nr_thread_pool_create(int num_workers);
void nr_thread_pool_destroy(nr_thread_pool_t *pool);
bool nr_thread_pool_submit(nr_thread_pool_t *pool, nr_task_t *task);
void nr_thread_pool_wait_completion(nr_thread_pool_t *pool);
void nr_thread_pool_print_stats(nr_thread_pool_t *pool);

// Utility functions
uint64_t nr_get_timestamp_ns(void);

// Memory initialization optimization (currently implemented)
void nr_parallel_txdataF_init(c16_t ***txdataF, int num_beams, int num_antennas, int samples_per_slot, int offset);

#endif // NR_THREAD_POOL_H