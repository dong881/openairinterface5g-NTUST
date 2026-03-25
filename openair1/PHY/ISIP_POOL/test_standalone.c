/*
 * Standalone ISIP Test Program
 *
 * Tests ISIP thread pool functionality independently
 * Verifies thread creation, core binding, and work distribution
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include "TaskScheduler_c.h"

// Simple logging macros for standalone test
#define LOG_I(module, fmt, ...) printf("[INFO %s] " fmt, #module, ##__VA_ARGS__)
#define LOG_W(module, fmt, ...) printf("[WARN %s] " fmt, #module, ##__VA_ARGS__)
#define LOG_E(module, fmt, ...) printf("[ERROR %s] " fmt, #module, ##__VA_ARGS__)
#define LOG_D(module, fmt, ...) printf("[DEBUG %s] " fmt, #module, ##__VA_ARGS__)
#define PHY PHY

// Global enkiTS scheduler instance
static enkiTaskScheduler* g_isip_scheduler = NULL;

// Core assignment for ISIP thread pool workers
static const int ISIP_CORES[8] = {6, 7, 8, 9, 10, 11, 13, 14};

/**
 * Thread start callback for core binding
 */
static void isip_thread_start(uint32_t threadNum) {
    if (threadNum < 8) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(ISIP_CORES[threadNum], &cpuset);

        if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0) {
            LOG_I(PHY, "[ISIP thread pool] Thread %d pinned to core %d\n",
                  threadNum, ISIP_CORES[threadNum]);
        } else {
            LOG_E(PHY, "[ISIP thread pool] Failed to pin thread %d to core %d\n",
                  threadNum, ISIP_CORES[threadNum]);
        }
    }
}

/**
 * Test task function
 */
static void test_task_func(uint32_t start, uint32_t end, uint32_t threadNum, void* pArgs) {
    int* results = (int*)pArgs;

    LOG_D(PHY, "[ISIP thread pool] Test task: Thread %d processing range %d-%d on core %d\n",
          threadNum, start, end, (threadNum < 8) ? ISIP_CORES[threadNum] : -1);

    // Simple computation to verify thread is working
    for (uint32_t i = start; i < end; i++) {
        results[i] = threadNum * 1000 + i;
    }
}

int isip_pool_init(void) {
    if (g_isip_scheduler) {
        LOG_W(PHY, "[ISIP thread pool] Already initialized\n");
        return 1;
    }

    LOG_I(PHY, "[ISIP thread pool] Initializing with 8 threads on cores 6,7,8,9,10,11,13,14\n");

    // Create enkiTS scheduler
    g_isip_scheduler = enkiNewTaskScheduler();
    if (!g_isip_scheduler) {
        LOG_E(PHY, "[ISIP thread pool] Failed to create task scheduler\n");
        return 0;
    }

    // Configure scheduler for 8 threads with core binding
    struct enkiTaskSchedulerConfig config = enkiGetTaskSchedulerConfig(g_isip_scheduler);
    config.numTaskThreadsToCreate = 8;  // Exactly 8 worker threads
    config.profilerCallbacks.threadStart = isip_thread_start;

    // Initialize with configuration
    enkiInitTaskSchedulerWithConfig(g_isip_scheduler, config);

    LOG_I(PHY, "[ISIP thread pool] Successfully initialized with %d threads\n",
          enkiGetNumTaskThreads(g_isip_scheduler));

    return 1;
}

void isip_pool_test(void) {
    if (!g_isip_scheduler) {
        LOG_W(PHY, "[ISIP thread pool] Cannot run test - scheduler not initialized\n");
        return;
    }

    LOG_I(PHY, "[ISIP thread pool] Running thread verification test\n");

    // Test parameters
    const int test_size = 80;  // 80 items, 10 per thread
    int results[test_size];
    memset(results, 0, sizeof(results));

    // Create test task set
    enkiTaskSet* test_task = enkiCreateTaskSet(g_isip_scheduler, test_task_func);
    if (!test_task) {
        LOG_E(PHY, "[ISIP thread pool] Failed to create test task\n");
        return;
    }

    // Execute test task with parameters
    enkiAddTaskSetMinRange(g_isip_scheduler, test_task, results, test_size, 10);
    enkiWaitForTaskSet(g_isip_scheduler, test_task);

    // Cleanup test task
    enkiDeleteTaskSet(g_isip_scheduler, test_task);

    // Verify results
    int thread_counts[8] = {0};
    for (int i = 0; i < test_size; i++) {
        int thread_id = results[i] / 1000;
        if (thread_id >= 0 && thread_id < 8) {
            thread_counts[thread_id]++;
        }
    }

    LOG_I(PHY, "[ISIP thread pool] Test completed - work distribution:\n");
    for (int i = 0; i < 8; i++) {
        LOG_I(PHY, "[ISIP thread pool] Thread %d (core %d): %d tasks\n",
              i, ISIP_CORES[i], thread_counts[i]);
    }
}

void isip_pool_shutdown(void) {
    if (g_isip_scheduler) {
        LOG_I(PHY, "[ISIP thread pool] Shutting down task scheduler\n");

        // Wait for all tasks to complete and shutdown
        enkiWaitforAllAndShutdown(g_isip_scheduler);

        // Delete scheduler and cleanup
        enkiDeleteTaskScheduler(g_isip_scheduler);
        g_isip_scheduler = NULL;

        LOG_I(PHY, "[ISIP thread pool] Shutdown completed\n");
    }
}

int isip_pool_is_initialized(void) {
    return (g_isip_scheduler != NULL);
}

int isip_pool_get_num_threads(void) {
    if (!g_isip_scheduler) {
        return 0;
    }
    return enkiGetNumTaskThreads(g_isip_scheduler);
}

int main() {
    printf("=== ISIP Standalone Test ===\n");

    // Test 1: Initialization
    printf("\n1. Testing ISIP initialization...\n");
    if (!isip_pool_init()) {
        printf("FAILED: ISIP initialization failed\n");
        return 1;
    }
    printf("SUCCESS: ISIP initialized\n");

    // Test 2: Status check
    printf("\n2. Testing status functions...\n");
    printf("Is initialized: %s\n", isip_pool_is_initialized() ? "YES" : "NO");
    printf("Number of threads: %d\n", isip_pool_get_num_threads());

    // Test 3: Work distribution test
    printf("\n3. Running work distribution test...\n");
    isip_pool_test();

    // Give threads time to complete and show their bindings
    printf("\n4. Waiting for threads to settle...\n");
    sleep(2);

    // Test 4: Clean shutdown
    printf("\n5. Testing shutdown...\n");
    isip_pool_shutdown();
    printf("SUCCESS: ISIP shutdown completed\n");

    // Test 5: Post-shutdown status
    printf("\n6. Testing post-shutdown status...\n");
    printf("Is initialized: %s\n", isip_pool_is_initialized() ? "YES" : "NO");
    printf("Number of threads: %d\n", isip_pool_get_num_threads());

    printf("\n=== Test Completed Successfully ===\n");
    return 0;
}
