// Minimal buildable optimized NR thread pool implementation
#define _GNU_SOURCE
#include "nr_thread_pool_optimized.h"
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>

// Get high-resolution timestamp
uint64_t nr_get_timestamp_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Lock-free queue operations (optimized)
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

// Optimized work stealing with exponential backoff
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

// Optimized worker thread function
static void* nr_worker_thread_optimized(void *arg) {
  nr_worker_thread_t *worker = (nr_worker_thread_t*)arg;
  nr_thread_pool_t *pool = worker->pool;
  int worker_id = worker->worker_id;

  // Set CPU affinity
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(worker->cpu_core, &cpuset);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
    printf("Warning: Failed to set CPU affinity for worker %d to core %d\n", worker_id, worker->cpu_core);
  }

  printf("Optimized NR Worker %d started on core %d\n", worker_id, worker->cpu_core);

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
        // Wait for work with timeout to handle missed signals
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

  printf("Optimized NR Worker %d shutting down. Tasks executed: %lu, stolen: %lu\n",
        worker_id, worker->tasks_executed, worker->tasks_stolen);
  return NULL;
}

// Create optimized thread pool
nr_thread_pool_t* nr_thread_pool_create_optimized(int num_workers) {
  printf("=== NR Thread Pool: ENABLED - Optimized parallel processing active ===\n");
  printf("NR Thread Pool: Creating %d worker threads with work-stealing queues\n", num_workers);

  if (num_workers > NR_MAX_WORKER_THREADS) {
    printf("NR Thread Pool: ERROR - Requested %d workers exceeds maximum %d\n", num_workers, NR_MAX_WORKER_THREADS);
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

  // Core assignment for 20-core server
  int core_assignments[NR_MAX_WORKER_THREADS] = {
    4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19
  };

  // Create worker threads
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

    if (pthread_create(&pool->workers[i].thread, NULL, nr_worker_thread_optimized, &pool->workers[i]) != 0) {
      printf("Error: Failed to create optimized worker thread %d\n", i);
      nr_thread_pool_destroy_optimized(pool);
      return NULL;
    }
  }

  // Initialize synchronization primitives
  pthread_cond_init(&pool->work_available, NULL);
  pthread_mutex_init(&pool->pool_mutex, NULL);

  printf("NR Thread Pool: Successfully created %d workers (cores 4-%d)\n", num_workers, 3 + num_workers);
  printf("NR Thread Pool: Features - Work stealing: ON, Event-driven: ON, CPU affinity: ON\n");
  return pool;
}

// Submit task to optimized thread pool
bool nr_thread_pool_submit_optimized(nr_thread_pool_t *pool, nr_task_t *task) {
  if (!pool || !task) return false;

  task->submit_time = nr_get_timestamp_ns();
  atomic_store(&task->completed, false);

  // Simple round-robin assignment for now
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
  printf("Warning: Failed to submit task - all queues full\n");
  return false;
}

// Wait for completion
void nr_thread_pool_wait_completion_optimized(nr_thread_pool_t *pool) {
  if (!pool) return;

  while (atomic_load(&pool->active_tasks) > 0) {
    usleep(10); // 10 microsecond polling - can be optimized further
  }
}

// Print performance statistics
void nr_thread_pool_print_stats_optimized(nr_thread_pool_t *pool) {
  if (!pool) return;

  printf("\n=== Optimized NR Thread Pool Statistics ===\n");
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

  printf("=== End Optimized Statistics ===\n\n");
}

// Destroy optimized thread pool
void nr_thread_pool_destroy_optimized(nr_thread_pool_t *pool) {
  if (!pool) return;

  // Signal shutdown
  atomic_store(&pool->shutdown_requested, true);

  // Wake up all workers
  for (int i = 0; i < pool->num_workers; i++) {
    pthread_cond_signal(&pool->workers[i].work_available_cond);
  }

  // Wait for all workers to finish
  for (int i = 0; i < pool->num_workers; i++) {
    pthread_join(pool->workers[i].thread, NULL);
    pthread_cond_destroy(&pool->workers[i].work_available_cond);
    pthread_mutex_destroy(&pool->workers[i].work_mutex);
  }

  // Cleanup synchronization primitives
  pthread_cond_destroy(&pool->work_available);
  pthread_mutex_destroy(&pool->pool_mutex);

  printf("Destroyed optimized NR thread pool\n");
  free(pool);
}