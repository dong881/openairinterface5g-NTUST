/*
 * Single-threaded Comparison Test
 *
 * Same computation as heavy test but without ISIP parallelization
 * Used to measure performance improvement
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

// Test parameters - reduced for benchmark
#define MATRIX_SIZE 128
#define NUM_ITERATIONS 10000  // Reduced from 1M to 10K
#define NUM_TASKS 64

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Single-threaded computation function
 */
static void compute_single_threaded(double* input_data, double* output_data) {
    for (int task_idx = 0; task_idx < NUM_TASKS; task_idx++) {
        double* input = &input_data[task_idx * MATRIX_SIZE];
        double* output = &output_data[task_idx * MATRIX_SIZE];

        // Same DSP operations as heavy test
        for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
            for (int i = 0; i < MATRIX_SIZE; i++) {
                // FFT-like butterfly operations
                double real_part = cos(2.0 * M_PI * i / MATRIX_SIZE + iter * 0.001);
                double imag_part = sin(2.0 * M_PI * i / MATRIX_SIZE + iter * 0.001);

                // Complex multiplication and accumulation
                output[i] += input[i] * (real_part * real_part + imag_part * imag_part);

                // Filtering operations
                if (i > 0) {
                    output[i] += 0.1 * output[i-1];
                }
            }
        }

        // Final normalization
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

int main() {
    printf("=== Single-threaded Performance Baseline ===\n");

    // Allocate test data
    double* input_data = (double*)malloc(NUM_TASKS * MATRIX_SIZE * sizeof(double));
    double* output_data = (double*)calloc(NUM_TASKS * MATRIX_SIZE, sizeof(double));

    if (!input_data || !output_data) {
        printf("FAILED: Memory allocation failed\n");
        return 1;
    }

    // Initialize input data with same pattern as heavy test
    for (int i = 0; i < NUM_TASKS * MATRIX_SIZE; i++) {
        input_data[i] = sin(i * 0.1) + cos(i * 0.05);
    }

    printf("Tasks: %d, Matrix size: %d, Iterations per task: %d\n",
           NUM_TASKS, MATRIX_SIZE, NUM_ITERATIONS);
    printf("Starting single-threaded computation...\n");

    uint64_t start_time = get_time_us();

    compute_single_threaded(input_data, output_data);

    uint64_t end_time = get_time_us();
    double duration_ms = (end_time - start_time) / 1000.0;

    printf("Single-threaded computation completed in %.2f ms\n", duration_ms);

    // Calculate performance metrics
    double total_ops = (double)NUM_TASKS * NUM_ITERATIONS * MATRIX_SIZE;
    double gflops = total_ops / (duration_ms * 1e6);
    printf("Performance: %.2f GFLOPS\n", gflops);

    free(input_data);
    free(output_data);

    printf("=== Single-threaded Test Completed ===\n");
    return 0;
}
