// Lock-free buffer pool implementation for NR thread pool
#define _GNU_SOURCE
#include "nr_thread_pool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

// Missing function declaration - will be implemented later
static inline void* aligned_alloc(size_t alignment, size_t size) {
  void *ptr;
  if (posix_memalign(&ptr, alignment, size) == 0) {
    return ptr;
  }
  return NULL;
}

// Create buffer pool with pre-allocated buffers
nr_buffer_pool_t* nr_buffer_pool_create(int num_buffers, int num_beams, int num_antennas, int samples_per_slot) {
  if (num_buffers > 64) {
    printf("Error: Maximum 64 buffers supported (using 64-bit mask)\n");
    return NULL;
  }

  nr_buffer_pool_t *pool = calloc(1, sizeof(nr_buffer_pool_t));
  if (!pool) return NULL;

  pool->max_buffers = num_buffers;
  pool->buffer_size = num_beams * num_antennas * samples_per_slot * sizeof(int32_t);

  // Initialize allocation mask (all buffers available)
  atomic_store(&pool->allocation_mask, (1ULL << num_buffers) - 1);
  atomic_store(&pool->allocations, 0);
  atomic_store(&pool->deallocations, 0);
  atomic_store(&pool->allocation_failures, 0);

  // Allocate buffer array
  pool->buffers = calloc(num_buffers, sizeof(int32_t***));
  if (!pool->buffers) {
    free(pool);
    return NULL;
  }

  // Pre-allocate all buffers
  for (int buf = 0; buf < num_buffers; buf++) {
    // Allocate beam array
    pool->buffers[buf] = calloc(num_beams, sizeof(int32_t**));
    if (!pool->buffers[buf]) goto cleanup_error;

    for (int beam = 0; beam < num_beams; beam++) {
      // Allocate antenna array
      pool->buffers[buf][beam] = calloc(num_antennas, sizeof(int32_t*));
      if (!pool->buffers[buf][beam]) goto cleanup_error;

      for (int antenna = 0; antenna < num_antennas; antenna++) {
        // Allocate frequency domain sample buffer (cache-aligned)
        pool->buffers[buf][beam][antenna] = aligned_alloc(64, samples_per_slot * sizeof(int32_t));
        if (!pool->buffers[buf][beam][antenna]) goto cleanup_error;

        // Initialize to zero
        memset(pool->buffers[buf][beam][antenna], 0, samples_per_slot * sizeof(int32_t));
      }
    }
  }

  printf("Created buffer pool: %d buffers, %d beams, %d antennas, %d samples/slot\n",
         num_buffers, num_beams, num_antennas, samples_per_slot);
  return pool;

cleanup_error:
  nr_buffer_pool_destroy(pool);
  return NULL;
}

// Lock-free buffer acquisition
int32_t*** nr_buffer_pool_acquire(nr_buffer_pool_t *pool, uint32_t *buffer_id) {
  if (!pool || !buffer_id) return NULL;

  uint64_t mask = atomic_load_explicit(&pool->allocation_mask, memory_order_acquire);

  while (mask != 0) {
    // Find first available buffer (rightmost set bit)
    int bit = __builtin_ctzll(mask);
    uint64_t buffer_bit = 1ULL << bit;

    // Try to atomically claim this buffer
    uint64_t expected = mask;
    uint64_t new_mask = mask & ~buffer_bit;

    if (atomic_compare_exchange_weak_explicit(&pool->allocation_mask, &expected, new_mask,
                                             memory_order_release, memory_order_relaxed)) {
      *buffer_id = bit;
      atomic_fetch_add(&pool->allocations, 1);
      return pool->buffers[bit];
    }

    // CAS failed, reload mask and try again
    mask = expected;
  }

  // No buffers available
  atomic_fetch_add(&pool->allocation_failures, 1);
  return NULL;
}

// Release buffer back to pool
void nr_buffer_pool_release(nr_buffer_pool_t *pool, uint32_t buffer_id) {
  if (!pool || buffer_id >= pool->max_buffers) return;

  uint64_t buffer_bit = 1ULL << buffer_id;
  uint64_t old_mask = atomic_fetch_or_explicit(&pool->allocation_mask, buffer_bit, memory_order_release);

  // Check for double-free
  if (old_mask & buffer_bit) {
    printf("Warning: Double-free detected for buffer %u\n", buffer_id);
    return;
  }

  atomic_fetch_add(&pool->deallocations, 1);
}

// Optimized buffer merging function
void nr_parallel_buffer_merge(int32_t ***dest, int32_t ****sources, int num_sources,
                             int num_beams, int num_antennas, int samples_per_slot) {
  if (!dest || !sources || num_sources == 0) return;

  for (int beam = 0; beam < num_beams; beam++) {
    for (int antenna = 0; antenna < num_antennas; antenna++) {
      int32_t *dest_ptr = dest[beam][antenna];

      // Clear destination first
      memset(dest_ptr, 0, samples_per_slot * sizeof(int32_t));

      // Add all source buffers
      for (int src = 0; src < num_sources; src++) {
        if (sources[src] && sources[src][beam] && sources[src][beam][antenna]) {
          int32_t *src_ptr = sources[src][beam][antenna];

          // SIMD-optimized addition (vectorized by compiler)
          for (int sample = 0; sample < samples_per_slot; sample++) {
            dest_ptr[sample] += src_ptr[sample];
          }
        }
      }
    }
  }
}

// Destroy buffer pool and free all memory
void nr_buffer_pool_destroy(nr_buffer_pool_t *pool) {
  if (!pool) return;

  if (pool->buffers) {
    for (int buf = 0; buf < pool->max_buffers; buf++) {
      if (pool->buffers[buf]) {
        // Free antenna buffers
        for (int beam = 0; beam < 16; beam++) { // Assume max 16 beams
          if (pool->buffers[buf][beam]) {
            for (int antenna = 0; antenna < 16; antenna++) { // Assume max 16 antennas
              if (pool->buffers[buf][beam][antenna]) {
                free(pool->buffers[buf][beam][antenna]);
              }
            }
            free(pool->buffers[buf][beam]);
          }
        }
        free(pool->buffers[buf]);
      }
    }
    free(pool->buffers);
  }

  printf("Buffer pool destroyed. Stats: alloc=%lu, dealloc=%lu, failures=%lu\n",
         atomic_load(&pool->allocations),
         atomic_load(&pool->deallocations),
         atomic_load(&pool->allocation_failures));

  free(pool);
}

// Enhanced batch task submission
bool nr_thread_pool_submit_batch(nr_thread_pool_t *pool, nr_task_t *tasks, int count) {
  if (!pool || !tasks || count <= 0) return false;

  int submitted = 0;
  for (int i = 0; i < count; i++) {
    if (nr_thread_pool_submit(pool, &tasks[i])) {
      submitted++;
    }
  }

  return submitted == count;
}

// Signal processing context submission
bool nr_thread_pool_submit_signal(nr_thread_pool_t *pool, nr_signal_context_t *ctx) {
  if (!pool || !ctx) return false;

  // Acquire private buffer for this signal
  uint32_t buffer_id;
  int32_t ***private_buffer = nr_buffer_pool_acquire(pool->buffer_pool, &buffer_id);
  if (!private_buffer) {
    printf("Warning: No buffers available for signal processing\n");
    return false;
  }

  // Create task with signal context
  nr_task_t task = {
    .type = ctx->signal_type,
    .priority = (ctx->signal_type == NR_TASK_PDSCH_PIPELINE) ? NR_PRIORITY_HIGH : NR_PRIORITY_MEDIUM,
    .func = (nr_task_func_t)ctx, // Will be cast to signal function
    .signal_ctx = ctx,
    .private_buffer = private_buffer,
    .buffer_id = buffer_id,
    .submit_time = nr_get_timestamp_ns()
  };

  atomic_store(&task.completed, false);
  atomic_store(&task.dependency_count, 0);

  return nr_thread_pool_submit(pool, &task);
}

// Enhanced statistics with NUMA awareness
void nr_thread_pool_print_numa_stats(nr_thread_pool_t *pool) {
  if (!pool) return;

  printf("\n=== NR Thread Pool NUMA Statistics ===\n");

  int numa_tasks[8] = {0}; // Support up to 8 NUMA nodes
  int numa_stolen[8] = {0};

  for (int i = 0; i < pool->num_workers; i++) {
    nr_worker_thread_t *worker = &pool->workers[i];
    int numa = worker->numa_node;

    numa_tasks[numa] += worker->tasks_executed;
    numa_stolen[numa] += worker->tasks_stolen;

    printf("Worker %d (Core %d, NUMA %d): executed=%lu, stolen=%lu\n",
           i, worker->cpu_core, numa, worker->tasks_executed, worker->tasks_stolen);
  }

  printf("\nNUMA Node Summary:\n");
  for (int numa = 0; numa < 2; numa++) { // Assume 2 NUMA nodes
    if (numa_tasks[numa] > 0) {
      printf("NUMA %d: executed=%d, stolen=%d, efficiency=%.1f%%\n",
             numa, numa_tasks[numa], numa_stolen[numa],
             numa_tasks[numa] > 0 ? (100.0 - (numa_stolen[numa] * 100.0 / numa_tasks[numa])) : 0.0);
    }
  }

  printf("=== End NUMA Statistics ===\n\n");
}