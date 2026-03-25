// Test program for optimized NR thread pool
#include "nr_thread_pool_optimized.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

// Test task function
void test_task_func(nr_task_t *task) {
  int *data = (int*)task->task_data;
  printf("Worker executing task with data: %d\n", *data);

  // Simulate some work
  usleep(1000); // 1ms

  *data = *data * 2; // Double the value
}

// Memory initialization test task
void memory_init_task(nr_task_t *task) {
  int *buffer = (int*)task->task_data;
  // Simulate memory initialization
  for (int i = 0; i < 1000; i++) {
    buffer[i] = 0;
  }
  printf("Memory initialization completed\n");
}

int main(int argc, char *argv[]) {
  int num_workers = 4;
  int num_tasks = 16;

  if (argc > 1) {
    num_workers = atoi(argv[1]);
  }
  if (argc > 2) {
    num_tasks = atoi(argv[2]);
  }

  printf("Testing optimized NR thread pool with %d workers and %d tasks\n", num_workers, num_tasks);

  // Create optimized thread pool
  nr_thread_pool_t *pool = nr_thread_pool_create_optimized(num_workers);
  if (!pool) {
    printf("Failed to create thread pool\n");
    return 1;
  }

  // Test 1: Basic task submission
  printf("\n=== Test 1: Basic Task Submission ===\n");

  int *test_data = malloc(num_tasks * sizeof(int));
  nr_task_t *tasks = malloc(num_tasks * sizeof(nr_task_t));

  for (int i = 0; i < num_tasks; i++) {
    test_data[i] = i + 1;
    tasks[i] = (nr_task_t) {
      .type = NR_TASK_SIGNAL_PROCESSING,
      .func = test_task_func,
      .task_data = &test_data[i]
    };

    if (!nr_thread_pool_submit_optimized(pool, &tasks[i])) {
      printf("Failed to submit task %d\n", i);
    }
  }

  // Wait for completion
  nr_thread_pool_wait_completion_optimized(pool);

  printf("Task results: ");
  for (int i = 0; i < num_tasks; i++) {
    printf("%d ", test_data[i]);
  }
  printf("\n");

  // Test 2: Memory initialization simulation
  printf("\n=== Test 2: Memory Initialization ===\n");

  int *memory_buffers[4];
  nr_task_t memory_tasks[4];

  for (int i = 0; i < 4; i++) {
    memory_buffers[i] = malloc(1000 * sizeof(int));
    memory_tasks[i] = (nr_task_t) {
      .type = NR_TASK_MEMORY_INIT,
      .func = memory_init_task,
      .task_data = memory_buffers[i]
    };

    nr_thread_pool_submit_optimized(pool, &memory_tasks[i]);
  }

  nr_thread_pool_wait_completion_optimized(pool);

  // Test 3: Performance measurement
  printf("\n=== Test 3: Performance Measurement ===\n");

  uint64_t start_time = nr_get_timestamp_ns();

  // Submit many tasks
  for (int batch = 0; batch < 10; batch++) {
    for (int i = 0; i < num_tasks; i++) {
      test_data[i] = i + 1;
      tasks[i] = (nr_task_t) {
        .type = NR_TASK_SIGNAL_PROCESSING,
        .func = test_task_func,
        .task_data = &test_data[i]
      };
      nr_thread_pool_submit_optimized(pool, &tasks[i]);
    }
    nr_thread_pool_wait_completion_optimized(pool);
  }

  uint64_t end_time = nr_get_timestamp_ns();
  uint64_t duration_ns = end_time - start_time;

  printf("Performance: %d tasks in %.2f ms\n",
         10 * num_tasks, duration_ns / 1000000.0);
  printf("Throughput: %.0f tasks/second\n",
         (10.0 * num_tasks * 1000000000.0) / duration_ns);

  // Print statistics
  nr_thread_pool_print_stats_optimized(pool);

  // Cleanup
  for (int i = 0; i < 4; i++) {
    free(memory_buffers[i]);
  }
  free(test_data);
  free(tasks);

  nr_thread_pool_destroy_optimized(pool);

  printf("Test completed successfully\n");
  return 0;
}