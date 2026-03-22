// NR Thread Pool Configuration Header
// Easy enable/disable controls for optimized thread pool
#ifndef NR_THREAD_POOL_CONFIG_H
#define NR_THREAD_POOL_CONFIG_H

// =========================================================================
// MAIN THREAD POOL ENABLE/DISABLE SWITCH
// =========================================================================
//
// Set to 1 to enable optimized thread pool, 0 to disable
// When disabled, all parallel functions fall back to sequential processing
//
#define NR_THREAD_POOL_OPTIMIZED_ENABLE 1

// =========================================================================
// CONFIGURATION OPTIONS (only active when thread pool is enabled)
// =========================================================================

#if NR_THREAD_POOL_OPTIMIZED_ENABLE

// Number of worker threads (recommend 4-16 for 20-core system)
#define NR_THREAD_POOL_NUM_WORKERS      8

// CPU core assignment strategy
#define NR_THREAD_POOL_CORE_START       4   // Start from core 4 (skip 0-3 for system)
#define NR_THREAD_POOL_CORE_END         19  // End at core 19

// Performance tuning
#define NR_THREAD_POOL_QUEUE_SIZE       256 // Tasks per worker queue
#define NR_THREAD_POOL_CACHE_LINE_SIZE  64  // Cache line alignment

// Feature flags
#define NR_THREAD_POOL_WORK_STEALING    1   // Enable work stealing
#define NR_THREAD_POOL_EVENT_DRIVEN     1   // Event-driven workers (vs polling)
#define NR_THREAD_POOL_CPU_AFFINITY     1   // Pin workers to specific cores
#define NR_THREAD_POOL_PERFORMANCE_STATS 1  // Collect performance statistics

// Advanced features (for future development)
#define NR_THREAD_POOL_BUFFER_POOL      0   // Private buffer management
#define NR_THREAD_POOL_NUMA_AWARE       0   // NUMA-aware work stealing
#define NR_THREAD_POOL_PRIORITY_QUEUES  0   // Priority-based task queues

#else

// When thread pool is disabled, define everything as 0/disabled
#define NR_THREAD_POOL_NUM_WORKERS      0
#define NR_THREAD_POOL_WORK_STEALING    0
#define NR_THREAD_POOL_EVENT_DRIVEN     0
#define NR_THREAD_POOL_CPU_AFFINITY     0
#define NR_THREAD_POOL_PERFORMANCE_STATS 0
#define NR_THREAD_POOL_BUFFER_POOL      0
#define NR_THREAD_POOL_NUMA_AWARE       0
#define NR_THREAD_POOL_PRIORITY_QUEUES  0

#endif // NR_THREAD_POOL_OPTIMIZED_ENABLE

// =========================================================================
// CONVENIENCE MACROS FOR CONDITIONAL COMPILATION
// =========================================================================

// Use these in your code for conditional compilation
#if NR_THREAD_POOL_OPTIMIZED_ENABLE
  #define NR_THREAD_POOL_ENABLED()   1
  #define NR_THREAD_POOL_DISABLED()  0
#else
  #define NR_THREAD_POOL_ENABLED()   0
  #define NR_THREAD_POOL_DISABLED()  1
#endif

// Conditional function calls
#if NR_THREAD_POOL_OPTIMIZED_ENABLE
  #define NR_THREAD_POOL_CALL(func, ...) func(__VA_ARGS__)
  #define NR_THREAD_POOL_CALL_OR_FALLBACK(func, fallback, ...) func(__VA_ARGS__)
#else
  #define NR_THREAD_POOL_CALL(func, ...) /* disabled */
  #define NR_THREAD_POOL_CALL_OR_FALLBACK(func, fallback, ...) fallback(__VA_ARGS__)
#endif

// =========================================================================
// EASY PRESET CONFIGURATIONS
// =========================================================================

// Uncomment ONE of these presets, or customize above

// Preset 1: High Performance (16 workers, all features)
// #undef NR_THREAD_POOL_NUM_WORKERS
// #define NR_THREAD_POOL_NUM_WORKERS 16

// Preset 2: Balanced (8 workers, essential features)
// #undef NR_THREAD_POOL_NUM_WORKERS
// #define NR_THREAD_POOL_NUM_WORKERS 8

// Preset 3: Conservative (4 workers, basic features)
// #undef NR_THREAD_POOL_NUM_WORKERS
// #define NR_THREAD_POOL_NUM_WORKERS 4

// Preset 4: Debug (2 workers, all statistics)
// #undef NR_THREAD_POOL_NUM_WORKERS
// #define NR_THREAD_POOL_NUM_WORKERS 2
// #undef NR_THREAD_POOL_PERFORMANCE_STATS
// #define NR_THREAD_POOL_PERFORMANCE_STATS 1

#endif // NR_THREAD_POOL_CONFIG_H