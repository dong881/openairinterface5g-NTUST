/*
 * Heavy ISIP Test Program
 *
 * Tests ISIP thread pool with computationally intensive tasks
 * Simulates DSP-like operations similar to 5G signal processing
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "TaskScheduler_c.h"

// Simple logging macros
#define LOG_I(module, fmt, ...) printf("[INFO %s] " fmt, #module, ##__VA_ARGS__)
#define LOG_W(module, fmt, ...) printf("[WARN %s] " fmt, #module, ##__VA_ARGS__)
#define LOG_E(module, fmt, ...) printf("[ERROR %s] " fmt, #module, ##__VA_ARGS__)
#define LOG_D(module, fmt, ...) printf("[DEBUG %s] " fmt, #module, ##__VA_ARGS__)
#define PHY PHY

// Global enkiTS scheduler instance
static enkiTaskScheduler* g_isip_scheduler = NULL;

// Core assignment for ISIP thread pool workers
static const int ISIP_CORES[8] = {6, 7, 8, 9, 10, 11, 13, 14};

// Test parameters for heavy computation
#define MATRIX_SIZE 128
#define NUM_ITERATIONS 10000  // Reduced from 1M to 10K for benchmark
#define NUM_TASKS 64  // More tasks to distribute across threads

typedef struct {
    int task_id;
    int thread_counts[8];
    double* input_data;
    double* output_data;
    struct timeval start_time;
} heavy_task_data_t;

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

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
 * Heavy computational task simulating DSP operations
 * - Matrix operations (similar to beamforming)
 * - FFT-like computations (similar to OFDM)
 * - Complex number arithmetic (similar to signal processing)
 */
static void heavy_task_func(uint32_t start, uint32_t end, uint32_t threadNum, void* pArgs) {
    heavy_task_data_t* task_data = (heavy_task_data_t*)pArgs;

    LOG_D(PHY, "[ISIP thread pool] Heavy task: Thread %d processing range %d-%d on core %d\n",
          threadNum, start, end, (threadNum < 8) ? ISIP_CORES[threadNum] : -1);

    // Track which thread processed this work
    if (threadNum < 8) {
        __sync_fetch_and_add(&task_data->thread_counts[threadNum], end - start);
    }

    for (uint32_t task_idx = start; task_idx < end; task_idx++) {
        double* input = &task_data->input_data[task_idx * MATRIX_SIZE];
        double* output = &task_data->output_data[task_idx * MATRIX_SIZE];

        // Simulate complex DSP operations
        for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
            for (int i = 0; i < MATRIX_SIZE; i++) {
                // Simulate FFT-like butterfly operations
                double real_part = cos(2.0 * M_PI * i / MATRIX_SIZE + iter * 0.001);
                double imag_part = sin(2.0 * M_PI * i / MATRIX_SIZE + iter * 0.001);

                // Complex multiplication and accumulation
                output[i] += input[i] * (real_part * real_part + imag_part * imag_part);

                // Simulate filtering operations
                if (i > 0) {
                    output[i] += 0.1 * output[i-1];
                }
            }
        }

        // Simulate final normalization (like in OFDM symbol processing)
        double sum = 0.0;
        for (int i = 0; i < MATRIX_SIZE; i++) {
            sum += output[i] * output[i];
        }
        double norm_factor = 1.0 / sqrt(sum + 1e-10);

        for (int i = 0; i < MATRIX_SIZE; i++) {
            output[i] *= norm_factor;
        }
    }
}

int isip_pool_init(void) {
    if (g_isip_scheduler) {
        LOG_W(PHY, "[ISIP thread pool] Already initialized\n");
        return 1;
    }

    LOG_I(PHY, "[ISIP thread pool] Initializing with 8 threads on cores 6,7,8,9,10,11,13,14\n");

    g_isip_scheduler = enkiNewTaskScheduler();
    if (!g_isip_scheduler) {
        LOG_E(PHY, "[ISIP thread pool] Failed to create task scheduler\n");
        return 0;
    }

    struct enkiTaskSchedulerConfig config = enkiGetTaskSchedulerConfig(g_isip_scheduler);
    config.numTaskThreadsToCreate = 8;
    config.profilerCallbacks.threadStart = isip_thread_start;

    enkiInitTaskSchedulerWithConfig(g_isip_scheduler, config);

    LOG_I(PHY, "[ISIP thread pool] Successfully initialized with %d threads\n",
          enkiGetNumTaskThreads(g_isip_scheduler));

    return 1;
}

void isip_pool_shutdown(void) {
    if (g_isip_scheduler) {
        LOG_I(PHY, "[ISIP thread pool] Shutting down task scheduler\n");
        enkiWaitforAllAndShutdown(g_isip_scheduler);
        enkiDeleteTaskScheduler(g_isip_scheduler);
        g_isip_scheduler = NULL;
        LOG_I(PHY, "[ISIP thread pool] Shutdown completed\n");
    }
}

void run_heavy_test(void) {
    if (!g_isip_scheduler) {
        LOG_W(PHY, "[ISIP thread pool] Cannot run test - scheduler not initialized\n");
        return;
    }

    LOG_I(PHY, "[ISIP thread pool] Running HEAVY computational test\n");
    LOG_I(PHY, "[ISIP thread pool] Tasks: %d, Matrix size: %d, Iterations per task: %d\n",
          NUM_TASKS, MATRIX_SIZE, NUM_ITERATIONS);

    // Allocate test data
    heavy_task_data_t task_data;
    memset(&task_data, 0, sizeof(task_data));

    task_data.input_data = (double*)malloc(NUM_TASKS * MATRIX_SIZE * sizeof(double));
    task_data.output_data = (double*)calloc(NUM_TASKS * MATRIX_SIZE, sizeof(double));

    if (!task_data.input_data || !task_data.output_data) {
        LOG_E(PHY, "[ISIP thread pool] Failed to allocate test data\n");
        return;
    }

    // Initialize input data with pseudo-random values
    for (int i = 0; i < NUM_TASKS * MATRIX_SIZE; i++) {
        task_data.input_data[i] = sin(i * 0.1) + cos(i * 0.05);
    }

    LOG_I(PHY, "[ISIP thread pool] Starting heavy computation...\n");

    uint64_t start_time = get_time_us();

    // Create and execute heavy task set
    enkiTaskSet* heavy_task = enkiCreateTaskSet(g_isip_scheduler, heavy_task_func);
    if (!heavy_task) {
        LOG_E(PHY, "[ISIP thread pool] Failed to create heavy task\n");
        goto cleanup;
    }

    // Execute with smaller minimum range to encourage more distribution
    enkiAddTaskSetMinRange(g_isip_scheduler, heavy_task, &task_data, NUM_TASKS, 1);
    enkiWaitForTaskSet(g_isip_scheduler, heavy_task);

    uint64_t end_time = get_time_us();
    double duration_ms = (end_time - start_time) / 1000.0;

    enkiDeleteTaskSet(g_isip_scheduler, heavy_task);

    LOG_I(PHY, "[ISIP thread pool] Heavy computation completed in %.2f ms\n", duration_ms);

    // Analyze work distribution
    int total_work = 0;
    for (int i = 0; i < 8; i++) {
        total_work += task_data.thread_counts[i];
    }

    LOG_I(PHY, "[ISIP thread pool] Work distribution across threads:\n");
    for (int i = 0; i < 8; i++) {
        double percentage = (total_work > 0) ? (100.0 * task_data.thread_counts[i] / total_work) : 0.0;
        LOG_I(PHY, "[ISIP thread pool] Thread %d (core %d): %d tasks (%.1f%%)\n",
              i, ISIP_CORES[i], task_data.thread_counts[i], percentage);
    }

    // Calculate performance metrics
    double total_ops = (double)NUM_TASKS * NUM_ITERATIONS * MATRIX_SIZE;
    double gflops = total_ops / (duration_ms * 1e6);
    LOG_I(PHY, "[ISIP thread pool] Performance: %.2f GFLOPS\n", gflops);

cleanup:
    free(task_data.input_data);
    free(task_data.output_data);
}

int main() {
    printf("=== ISIP Heavy Load Test ===\n");

    // Test 1: Initialization
    printf("\n1. Initializing ISIP with 8 worker threads...\n");
    if (!isip_pool_init()) {
        printf("FAILED: ISIP initialization failed\n");
        return 1;
    }

    // Test 2: Heavy computational workload
    printf("\n2. Running heavy computational test...\n");
    run_heavy_test();

    // Test 3: Clean shutdown
    printf("\n3. Shutting down ISIP...\n");
    isip_pool_shutdown();

    printf("\n=== Heavy Test Completed ===\n");
    return 0;
}
