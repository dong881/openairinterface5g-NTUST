// =========================================================================
// OPTIMIZED NR THREAD POOL FOR BEST 5G NR DOWNLINK PERFORMANCE
// =========================================================================
//
// High-performance implementation with work-stealing queues, event-driven
// workers, CPU affinity, and lock-free operations for maximum performance.
//
// Key Features:
// - Work-stealing queues for balanced load distribution
// - Event-driven workers to minimize CPU usage
// - CPU affinity for optimal cache performance
// - Lock-free atomic operations
// =========================================================================

#define _GNU_SOURCE
#include "nr_thread_pool.h"
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include "openair1/PHY/TOOLS/tools_defs.h"

// Get high-resolution timestamp
uint64_t nr_get_timestamp_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// =========================================================================
// LOCK-FREE QUEUE OPERATIONS
// =========================================================================

static bool nr_queue_push_optimized(nr_task_queue_t *queue, nr_task_t *task) {
  uint64_t head = atomic_load_explicit(&queue->head, memory_order_relaxed);
  uint64_t next_head = head + 1;

  if (next_head - atomic_load_explicit(&queue->tail, memory_order_acquire) >= NR_MAX_TASKS_PER_QUEUE) {
    return false; // Queue full
  }

  queue->tasks[head % NR_MAX_TASKS_PER_QUEUE] = *task;
  atomic_store_explicit(&queue->head, next_head, memory_order_release);
  return true;
}

static bool nr_queue_pop_optimized(nr_task_queue_t *queue, nr_task_t *task) {
  uint64_t tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
  uint64_t head = atomic_load_explicit(&queue->head, memory_order_acquire);

  if (tail >= head) {
    return false; // Queue empty
  }

  *task = queue->tasks[tail % NR_MAX_TASKS_PER_QUEUE];
  atomic_store_explicit(&queue->tail, tail + 1, memory_order_release);
  return true;
}

// =========================================================================
// OPTIMIZED WORK-STEALING ALGORITHM
// =========================================================================

static bool nr_steal_task_optimized(nr_thread_pool_t *pool, int worker_id, nr_task_t *task) {
  int start_victim = (worker_id + 1) % pool->num_workers;

  for (int i = 0; i < pool->num_workers - 1; i++) {
    int victim_id = (start_victim + i) % pool->num_workers;
    nr_task_queue_t *victim_queue = &pool->worker_queues[victim_id];

    uint64_t victim_head = atomic_load_explicit(&victim_queue->head, memory_order_acquire);
    uint64_t victim_tail = atomic_load_explicit(&victim_queue->tail, memory_order_acquire);

    if (victim_tail >= victim_head) continue; // Empty queue

    // Try to steal with exponential backoff
    for (int attempt = 0; attempt < 3; attempt++) {
      uint64_t expected_tail = victim_tail;
      if (atomic_compare_exchange_weak_explicit(&victim_queue->tail, &expected_tail, victim_tail + 1,
                                               memory_order_release, memory_order_relaxed)) {
        *task = victim_queue->tasks[victim_tail % NR_MAX_TASKS_PER_QUEUE];
        pool->workers[worker_id].tasks_stolen++;
        return true;
      }

      // Exponential backoff
      for (int delay = 0; delay < (1 << attempt); delay++) {
        __asm__ __volatile__("pause" ::: "memory");
      }

      victim_tail = atomic_load_explicit(&victim_queue->tail, memory_order_acquire);
      if (victim_tail >= victim_head) break;
    }
  }

  return false;
}

// =========================================================================
// OPTIMIZED WORKER THREAD
// =========================================================================

static void* nr_worker_thread_optimized(void *arg) {
  nr_worker_thread_t *worker = (nr_worker_thread_t*)arg;
  nr_thread_pool_t *pool = worker->pool;
  int worker_id = worker->worker_id;

  // Set CPU affinity
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(worker->cpu_core, &cpuset);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
    printf("[ISIP thread pool] WARNING - Failed to set CPU affinity for worker %d to core %d\n", worker_id, worker->cpu_core);
  } else {
    printf("[ISIP thread pool] Worker %d successfully pinned to core %d\n", worker_id, worker->cpu_core);
  }

  printf("[ISIP thread pool] Worker %d STARTED on core %d (Work-stealing, Event-driven)\n", worker_id, worker->cpu_core);

  nr_task_t task;
  while (!atomic_load(&pool->shutdown_requested)) {
    bool found_work = false;

    // 1. Try own queue first
    if (nr_queue_pop_optimized(&pool->worker_queues[worker_id], &task)) {
      found_work = true;
    }
    // 2. Try work stealing
    else if (nr_steal_task_optimized(pool, worker_id, &task)) {
      found_work = true;
    }

    if (found_work) {
      // Execute task
      if (task.func) {
        task.func(&task);
      }
      atomic_store(&task.completed, true);
      atomic_fetch_sub(&pool->active_tasks, 1);
      worker->tasks_executed++;
    } else {
      // Event-driven idle management
      worker->idle_cycles++;

      pthread_mutex_lock(&worker->work_mutex);
      if (!atomic_load(&worker->has_work) && !atomic_load(&pool->shutdown_requested)) {
        // Wait for work with timeout
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_nsec += 100000; // 100 microseconds
        if (timeout.tv_nsec >= 1000000000) {
          timeout.tv_sec++;
          timeout.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&worker->work_available_cond, &worker->work_mutex, &timeout);
      }
      atomic_store(&worker->has_work, false);
      pthread_mutex_unlock(&worker->work_mutex);
    }
  }

  printf("[ISIP thread pool] Worker %d TERMINATED (Core %d) - Stats: %lu tasks executed, %lu stolen, %lu idle cycles\n",
        worker_id, worker->cpu_core, worker->tasks_executed, worker->tasks_stolen, worker->idle_cycles);
  return NULL;
}

// =========================================================================
// THREAD POOL MANAGEMENT
// =========================================================================

nr_thread_pool_t* nr_thread_pool_create(int num_workers) {
  printf("[ISIP thread pool] ENABLED - Creating optimized thread pool\n");
  printf("[ISIP thread pool] Features - Work stealing: ON, Event-driven: ON, CPU affinity: ON\n");
  printf("[ISIP thread pool] Initializing %d worker threads for 5G NR downlink processing\n", num_workers);

  if (num_workers <= 0 || num_workers > NR_MAX_WORKER_THREADS) {
    printf("[ISIP thread pool] ERROR - Invalid worker count %d (max: %d)\n", num_workers, NR_MAX_WORKER_THREADS);
    return NULL;
  }

  nr_thread_pool_t *pool = calloc(1, sizeof(nr_thread_pool_t));
  if (!pool) return NULL;

  pool->num_workers = num_workers;
  atomic_store(&pool->shutdown_requested, false);
  atomic_store(&pool->active_tasks, 0);

  // Initialize task queues
  for (int i = 0; i < num_workers; i++) {
    atomic_store(&pool->worker_queues[i].head, 0);
    atomic_store(&pool->worker_queues[i].tail, 0);
  }

  // Core assignment for 20-core server (avoiding cores 16,17)
  // Primary: cores 4-15, 18-19 (14 workers)
  // Fallback: cores 1-2 if more than 14 workers requested
  int core_assignments[NR_MAX_WORKER_THREADS] = {
    4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 18, 19, 1, 2
  };

  // Create worker threads
  printf("[ISIP thread pool] Creating %d optimized worker threads...\n", num_workers);
  for (int i = 0; i < num_workers; i++) {
    pool->workers[i].worker_id = i;
    pool->workers[i].cpu_core = core_assignments[i];
    pool->workers[i].pool = pool;
    pool->workers[i].tasks_executed = 0;
    pool->workers[i].tasks_stolen = 0;
    pool->workers[i].idle_cycles = 0;

    // Initialize event-driven synchronization
    atomic_store(&pool->workers[i].has_work, false);
    pthread_cond_init(&pool->workers[i].work_available_cond, NULL);
    pthread_mutex_init(&pool->workers[i].work_mutex, NULL);

    printf("[ISIP thread pool] Creating worker %d on core %d...\n", i, core_assignments[i]);
    if (pthread_create(&pool->workers[i].thread, NULL, nr_worker_thread_optimized, &pool->workers[i]) != 0) {
      printf("[ISIP thread pool] ERROR - Failed to create worker thread %d on core %d\n", i, core_assignments[i]);
      nr_thread_pool_destroy(pool);
      return NULL;
    }
    printf("[ISIP thread pool] Worker %d thread created successfully\n", i);
  }

  // Initialize synchronization primitives
  pthread_cond_init(&pool->work_available, NULL);
  pthread_mutex_init(&pool->pool_mutex, NULL);

  printf("[ISIP thread pool] Successfully created %d workers (cores 4-15,18-19, avoiding 16-17)\n", num_workers);
  printf("[ISIP thread pool] Initialization complete\n\n");
  return pool;
}

bool nr_thread_pool_submit(nr_thread_pool_t *pool, nr_task_t *task) {
  if (!pool || !task) return false;

  task->submit_time = nr_get_timestamp_ns();
  atomic_store(&task->completed, false);

  // Round-robin assignment
  int target_worker = atomic_fetch_add(&pool->active_tasks, 1) % pool->num_workers;

  if (nr_queue_push_optimized(&pool->worker_queues[target_worker], task)) {
    // Signal worker that work is available
    nr_worker_thread_t *worker = &pool->workers[target_worker];
    atomic_store(&worker->has_work, true);
    pthread_cond_signal(&worker->work_available_cond);
    return true;
  }

  // Fallback: try all queues
  for (int i = 0; i < pool->num_workers; i++) {
    int worker_idx = (target_worker + i) % pool->num_workers;
    if (nr_queue_push_optimized(&pool->worker_queues[worker_idx], task)) {
      nr_worker_thread_t *worker = &pool->workers[worker_idx];
      atomic_store(&worker->has_work, true);
      pthread_cond_signal(&worker->work_available_cond);
      return true;
    }
  }

  atomic_fetch_sub(&pool->active_tasks, 1);
  printf("[ISIP thread pool] Warning: Failed to submit task - all queues full\n");
  return false;
}

void nr_thread_pool_wait_completion(nr_thread_pool_t *pool) {
  if (!pool) return;

  while (atomic_load(&pool->active_tasks) > 0) {
    usleep(10); // 10 microsecond polling
  }
}

void nr_thread_pool_print_stats(nr_thread_pool_t *pool) {
  if (!pool) return;

  printf("\n[ISIP thread pool] Performance Statistics:\n");
  printf("Workers: %d\n", pool->num_workers);
  printf("Active tasks: %d\n", atomic_load(&pool->active_tasks));

  uint64_t total_executed = 0;
  uint64_t total_stolen = 0;
  uint64_t total_idle = 0;

  for (int i = 0; i < pool->num_workers; i++) {
    nr_worker_thread_t *worker = &pool->workers[i];
    printf("Worker %d (Core %d): executed=%lu, stolen=%lu, idle_cycles=%lu\n",
           i, worker->cpu_core, worker->tasks_executed, worker->tasks_stolen, worker->idle_cycles);

    total_executed += worker->tasks_executed;
    total_stolen += worker->tasks_stolen;
    total_idle += worker->idle_cycles;
  }

  printf("\nTotals: executed=%lu, stolen=%lu, idle_cycles=%lu\n", total_executed, total_stolen, total_idle);

  if (total_executed > 0) {
    printf("Work stealing efficiency: %.1f%%\n", (total_stolen * 100.0) / total_executed);
  }

  printf("[ISIP thread pool] End Performance Statistics\n\n");
}

void nr_thread_pool_destroy(nr_thread_pool_t *pool) {
  if (!pool) return;

  printf("[ISIP thread pool] Initiating shutdown of %d workers\n", pool->num_workers);

  // Signal shutdown
  atomic_store(&pool->shutdown_requested, true);

  // Wake up all workers
  printf("[ISIP thread pool] Signaling shutdown to all workers...\n");
  for (int i = 0; i < pool->num_workers; i++) {
    pthread_cond_signal(&pool->workers[i].work_available_cond);
  }

  // Wait for all workers to finish
  printf("[ISIP thread pool] Waiting for all workers to terminate...\n");
  for (int i = 0; i < pool->num_workers; i++) {
    printf("[ISIP thread pool] Waiting for worker %d (core %d) to finish...\n", i, pool->workers[i].cpu_core);
    pthread_join(pool->workers[i].thread, NULL);
    pthread_cond_destroy(&pool->workers[i].work_available_cond);
    pthread_mutex_destroy(&pool->workers[i].work_mutex);
    printf("[ISIP thread pool] Worker %d terminated and cleaned up\n", i);
  }

  // Cleanup synchronization primitives
  pthread_cond_destroy(&pool->work_available);
  pthread_mutex_destroy(&pool->pool_mutex);

  printf("[ISIP thread pool] All %d workers terminated, cleanup complete\n", pool->num_workers);
  free(pool);
}

// =========================================================================
// PARALLEL MEMORY INITIALIZATION (IMPLEMENTED OPTIMIZATION)
// =========================================================================

typedef struct {
  c16_t ***txdataF;
  int beam;
  int num_antennas;
  int samples_per_slot;
  int offset;
} nr_memory_init_task_data_t;

static void nr_task_memory_init(nr_task_t *task) {
  nr_memory_init_task_data_t *data = (nr_memory_init_task_data_t*)task->task_data;

  for (int aa = 0; aa < data->num_antennas; aa++) {
    memset(&data->txdataF[data->beam][aa][data->offset], 0, data->samples_per_slot * sizeof(c16_t));
  }
}

void nr_parallel_txdataF_init(c16_t ***txdataF, int num_beams, int num_antennas, int samples_per_slot, int offset) {
  extern nr_thread_pool_t *nr_global_thread_pool;

  if (!nr_global_thread_pool || num_beams <= 0) {
    // Fallback to sequential
    for (int i = 0; i < num_beams; i++) {
      for (int aa = 0; aa < num_antennas; aa++) {
        memset(&txdataF[i][aa][offset], 0, samples_per_slot * sizeof(c16_t));
      }
    }
    return;
  }

  // Parallel initialization

  nr_task_t tasks[num_beams];
  nr_memory_init_task_data_t task_data[num_beams];

  for (int beam = 0; beam < num_beams; beam++) {
    task_data[beam].txdataF = txdataF;
    task_data[beam].beam = beam;
    task_data[beam].num_antennas = num_antennas;
    task_data[beam].samples_per_slot = samples_per_slot;
    task_data[beam].offset = offset;

    tasks[beam].type = NR_TASK_MEMORY_INIT;
    tasks[beam].func = nr_task_memory_init;
    tasks[beam].task_data = &task_data[beam];

    nr_thread_pool_submit(nr_global_thread_pool, &tasks[beam]);
  }

  nr_thread_pool_wait_completion(nr_global_thread_pool);
}