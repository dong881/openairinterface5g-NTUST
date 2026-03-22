#ifndef NR_THREAD_POOL_INIT_H
#define NR_THREAD_POOL_INIT_H

#include "nr_thread_pool.h"

// Global thread pool instance
extern nr_thread_pool_t *nr_global_thread_pool;

// Initialization and cleanup functions
int nr_thread_pool_init_global(int num_workers);
void nr_thread_pool_cleanup_global(void);

// Configuration function
int nr_thread_pool_get_optimal_workers(void);

#endif // NR_THREAD_POOL_INIT_H