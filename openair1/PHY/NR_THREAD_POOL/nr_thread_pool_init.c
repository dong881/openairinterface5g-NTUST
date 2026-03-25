// _GNU_SOURCE is already defined by build system
#include "nr_thread_pool_init.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Global thread pool instance
nr_thread_pool_t *nr_global_thread_pool = NULL;

int nr_thread_pool_init_global(int num_workers) {
#if NR_THREAD_POOL_ENABLE
  if (nr_global_thread_pool != NULL) {
    printf("[ISIP thread pool] Warning: Global thread pool already initialized\n");
    return 1;
  }

  if (num_workers <= 0) {
    num_workers = nr_thread_pool_get_optimal_workers();
  }

  printf("[ISIP thread pool] Initializing global thread pool with %d workers\n", num_workers);

  nr_global_thread_pool = nr_thread_pool_create(num_workers);
  if (nr_global_thread_pool == NULL) {
    printf("[ISIP thread pool] Error: Failed to create global thread pool\n");
    return -1;
  }

  printf("[ISIP thread pool] Global thread pool initialized successfully\n");
  return 0;
#else
  (void)num_workers; // Suppress unused parameter warning
  printf("[ISIP thread pool] Disabled at compile time (NR_THREAD_POOL_ENABLE=0)\n");
  nr_global_thread_pool = NULL;
  return 0;
#endif
}

void nr_thread_pool_cleanup_global(void) {
#if NR_THREAD_POOL_ENABLE
  if (nr_global_thread_pool != NULL) {
    printf("[ISIP thread pool] Cleaning up global thread pool\n");
    nr_thread_pool_destroy(nr_global_thread_pool);
    nr_global_thread_pool = NULL;
    printf("[ISIP thread pool] Global thread pool cleaned up\n");
  }
#else
  // Thread pool disabled, nothing to cleanup
  nr_global_thread_pool = NULL;
#endif
}

int nr_thread_pool_get_optimal_workers(void) {
  // Get number of CPU cores
  int num_cores = sysconf(_SC_NPROCESSORS_ONLN);

  // For 20-core server, use 14 workers (cores 4-15,18-19, avoiding 16-17)
  // Leave cores 0-3 for system and MAC scheduling, avoid cores 16-17
  if (num_cores >= 20) {
    return 14;
  } else if (num_cores >= 8) {
    return num_cores - 4; // Leave 4 cores for system
  } else if (num_cores >= 4) {
    return num_cores / 2; // Use half the cores
  } else {
    return 2; // Minimum 2 workers
  }
}