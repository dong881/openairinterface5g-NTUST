// Minimal buildable optimized NR thread pool
#ifndef NR_THREAD_POOL_OPTIMIZED_H
#define NR_THREAD_POOL_OPTIMIZED_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Basic configuration - keep it simple for now
#define NR_MAX_WORKER_THREADS    16
#define NR_MAX_TASKS_PER_QUEUE   256
#define NR_CACHE_LINE_SIZE       64

// Basic task types
typedef enum {
  NR_TASK_MEMORY_INIT = 0,
  NR_TASK_SIGNAL_PROCESSING,
  NR_TASK_BUFFER_MERGE,
  NR_TASK_MAX
} nr_task_type_t;

// Simple task structure
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
  struct nr_thread_pool *pool;

  // Event-driven optimization
  pthread_cond_t work_available_cond;
  pthread_mutex_t work_mutex;
  _Atomic(bool) has_work;

  // Performance stats
  uint64_t tasks_executed;
  uint64_t tasks_stolen;
  uint64_t idle_cycles;
} nr_worker_thread_t;

// Main optimized thread pool
typedef struct nr_thread_pool {
  nr_worker_thread_t workers[NR_MAX_WORKER_THREADS];
  int num_workers;
  nr_task_queue_t worker_queues[NR_MAX_WORKER_THREADS];

  // Pool state
  _Atomic(bool) shutdown_requested;
  _Atomic(int) active_tasks;

  // Basic synchronization
  pthread_cond_t work_available;
  pthread_mutex_t pool_mutex;
} nr_thread_pool_t;

// Core API functions
nr_thread_pool_t* nr_thread_pool_create_optimized(int num_workers);
void nr_thread_pool_destroy_optimized(nr_thread_pool_t *pool);
bool nr_thread_pool_submit_optimized(nr_thread_pool_t *pool, nr_task_t *task);
void nr_thread_pool_wait_completion_optimized(nr_thread_pool_t *pool);
void nr_thread_pool_print_stats_optimized(nr_thread_pool_t *pool);

// Utility functions
uint64_t nr_get_timestamp_ns(void);

#endif // NR_THREAD_POOL_OPTIMIZED_H