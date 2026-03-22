// Test program to demonstrate easy enable/disable of thread pool
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

// Include the wrapper which handles enable/disable logic
#include "nr_thread_pool_wrapper.h"

// Global thread pool instance
nr_pool_t *nr_global_pool = NULL;

// Test task function
void test_task(nr_pool_task_t *task) {
  int *data = (int*)task->task_data;
  printf("Processing task with data: %d\n", *data);
  *data *= 2; // Double the value
}

int main(int argc, char *argv[]) {
  printf("=== NR Thread Pool Toggle Test ===\n");

  // Show current configuration
#if NR_THREAD_POOL_OPTIMIZED_ENABLE
  printf("Thread Pool Status: ENABLED\n");
  printf("Number of Workers: %d\n", NR_THREAD_POOL_NUM_WORKERS);
  printf("Work Stealing: %s\n", NR_THREAD_POOL_WORK_STEALING ? "Enabled" : "Disabled");
  printf("Event Driven: %s\n", NR_THREAD_POOL_EVENT_DRIVEN ? "Enabled" : "Disabled");
  printf("CPU Affinity: %s\n", NR_THREAD_POOL_CPU_AFFINITY ? "Enabled" : "Disabled");
#else
  printf("Thread Pool Status: DISABLED (Sequential Processing)\n");
#endif

  // Initialize global thread pool
  printf("\n=== Initializing Thread Pool ===\n");
  if (!nr_pool_init_global()) {
    printf("Failed to initialize thread pool\n");
    return 1;
  }

  // Test basic functionality
  printf("\n=== Testing Task Submission ===\n");

  int test_data[8];
  nr_pool_task_t tasks[8];

  // Create test tasks
  for (int i = 0; i < 8; i++) {
    test_data[i] = i + 1;
    tasks[i] = (nr_pool_task_t) {
      .func = (void(*)(void*))test_task,
      .task_data = &test_data[i]
    };

    printf("Submitting task %d\n", i);
    nr_pool_submit(nr_global_pool, &tasks[i]);
  }

  // Wait for completion
  printf("\n=== Waiting for Task Completion ===\n");
  nr_pool_wait_completion(nr_global_pool);

  // Show results
  printf("\n=== Task Results ===\n");
  printf("Results: ");
  for (int i = 0; i < 8; i++) {
    printf("%d ", test_data[i]);
  }
  printf("\n");

  // Test parallel memory initialization
  printf("\n=== Testing Parallel Memory Initialization ===\n");

  // Simulate txdataF arrays
  int32_t **beam_data[2];  // 2 beams
  for (int beam = 0; beam < 2; beam++) {
    beam_data[beam] = malloc(4 * sizeof(int32_t*)); // 4 antennas
    for (int ant = 0; ant < 4; ant++) {
      beam_data[beam][ant] = malloc(1000 * sizeof(int32_t)); // 1000 samples
      // Fill with test pattern
      for (int i = 0; i < 1000; i++) {
        beam_data[beam][ant][i] = i;
      }
    }
  }

  // Test parallel memory initialization
  nr_parallel_memory_init((int32_t***)beam_data, 2, 4, 1000);

  // Verify memory was cleared
  bool memory_cleared = true;
  for (int beam = 0; beam < 2 && memory_cleared; beam++) {
    for (int ant = 0; ant < 4 && memory_cleared; ant++) {
      for (int i = 0; i < 10; i++) { // Check first 10 samples
        if (beam_data[beam][ant][i] != 0) {
          memory_cleared = false;
          break;
        }
      }
    }
  }

  printf("Memory initialization: %s\n", memory_cleared ? "SUCCESS" : "FAILED");

  // Test signal processing wrapper
  printf("\n=== Testing Signal Processing Wrapper ===\n");
  nr_parallel_signal_processing(NULL, 123, 456);

  // Print performance statistics
  printf("\n=== Performance Statistics ===\n");
  nr_pool_print_stats(nr_global_pool);

  // Cleanup
  printf("\n=== Cleanup ===\n");
  for (int beam = 0; beam < 2; beam++) {
    for (int ant = 0; ant < 4; ant++) {
      free(beam_data[beam][ant]);
    }
    free(beam_data[beam]);
  }

  nr_pool_cleanup_global();

  printf("\n=== Test Completed ===\n");

  // Show how to toggle the thread pool
  printf("\n=== How to Toggle Thread Pool ===\n");
  printf("To DISABLE thread pool:\n");
  printf("  Edit nr_thread_pool_config.h\n");
  printf("  Change: #define NR_THREAD_POOL_OPTIMIZED_ENABLE 1\n");
  printf("  To:     #define NR_THREAD_POOL_OPTIMIZED_ENABLE 0\n");
  printf("  Then recompile.\n");
  printf("\n");
  printf("To change worker count:\n");
  printf("  Edit: #define NR_THREAD_POOL_NUM_WORKERS 8\n");
  printf("  To desired number (4, 8, 16, etc.)\n");

  return 0;
}