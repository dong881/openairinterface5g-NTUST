/*
 * ISIP Thread Pool - C API for enkiTS integration
 *
 * This file provides a minimal C interface to the enkiTS task scheduler
 * for OAI 5G downlink processing parallelization.
 *
 * Creates 10 worker threads pinned to cores: 6,8,9,10,11,13,14,15,16,17
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Parallel Segmentation Structures
// ============================================================================

// Maximum segments supported: MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER (36) * NR_MAX_NB_LAYERS (4) = 144
#define ISIP_MAX_SEGMENTS 144

// Segmentation task parameters for a single TB
typedef struct {
    // Input (from harq->pdu with TB CRC appended)
    const uint8_t *input;       // Source: harq->pdu (after TB CRC)
    uint8_t **outputs;          // Destination: harq->c[] segment buffers

    // Segmentation parameters (calculated by nr_segmentation_params)
    uint32_t C;                 // Number of code blocks
    uint32_t K;                 // Code block size (bits)
    uint32_t Kprime;            // K' = B'/C
    uint32_t Z;                 // Lifting size
    uint32_t F;                 // Filler bits
    uint32_t L;                 // CRC length (0 if C==1, 24 if C>1)

    // State
    volatile int completed;     // 1 when all segments processed
} isip_seg_params_t;

// ============================================================================
// DMRS Precompute Structures
// ============================================================================

// Maximum values for DMRS precomputation
#define ISIP_MAX_DMRS_SYMBOLS 4      // Max DMRS symbols per PDSCH (double DMRS)
#define ISIP_MAX_DMRS_RES 1728       // Max DMRS REs: 275 PRBs * 6 (type 2) + alignment
#define ISIP_MAX_PDSCH_PER_SLOT 8    // Max PDSCHs to precompute in parallel

// Complex 16-bit type (must match c16_t)
typedef struct { int16_t r; int16_t i; } isip_c16_t;

// DMRS precompute parameters for a single PDSCH
typedef struct {
    // Input parameters
    int n_dmrs;                  // Number of DMRS REs per symbol
    int slot;                    // Slot number
    int dlDmrsScramblingId;      // DMRS scrambling ID
    int SCID;                    // Scrambling ID selector
    int StartSymbolIndex;        // First symbol of PDSCH
    int NrOfSymbols;             // Number of symbols
    uint16_t dmrs_symbol_map;    // Bitmap of DMRS symbols
    int N_RB_DL;                 // Number of RBs in DL
    int symbols_per_slot;        // Symbols per slot (14)

    // Output buffers (must be pre-allocated by caller)
    isip_c16_t *mod_dmrs_out;         // [MAX_DMRS_SYMBOLS * dmrs_buf_size] - flattened
    int dmrs_buf_stride;                 // Stride between DMRS symbols in mod_dmrs_out
    int symbol_indices_out[ISIP_MAX_DMRS_SYMBOLS];  // Which symbols have DMRS
    int l_prime_out[ISIP_MAX_DMRS_SYMBOLS];         // l_prime for each DMRS symbol
    int num_precomputed;                              // Number of DMRS symbols computed
} isip_dmrs_params_t;

/**
 * Initialize ISIP thread pool with 10 threads on cores 6,8,9,10,11,13,14,15,16,17
 *
 * @return 1 on success, 0 on failure
 */
int isip_pool_init(void);

/**
 * Shutdown ISIP thread pool and cleanup all resources
 */
void isip_pool_shutdown(void);

/**
 * Test function to verify thread creation and core binding
 * Should be called after isip_pool_init() to validate setup
 */
void isip_pool_test(void);

/**
 * Check if ISIP thread pool is initialized
 *
 * @return 1 if initialized, 0 if not
 */
int isip_pool_is_initialized(void);

/**
 * Get number of worker threads in ISIP thread pool
 *
 * @return Number of threads (should be 10), or 0 if not initialized
 */
int isip_pool_get_num_threads(void);

/**
 * Get enkiTS scheduler handle (for advanced usage)
 * WARNING: Only use if you know what you're doing
 *
 * @return Scheduler pointer, or NULL if not initialized
 */
void* isip_pool_get_scheduler(void);

/**
 * Register external thread with ISIP pool (MUST be called from the thread itself)
 * Required for any non-ISIP thread that wants to submit tasks
 *
 * @return 1 on success, 0 on failure
 */
int isip_pool_register_external_thread(void);

/**
 * Deregister external thread from ISIP pool
 * Should be called before thread exits
 */
void isip_pool_deregister_external_thread(void);

/**
 * Parallel memory clear for TX buffers (synchronous)
 * Clears txdataF buffers for all beams/antennas in parallel using ISIP pool
 * NOTE: Calling thread must be registered first with isip_pool_register_external_thread()
 *
 * @param txdataF Array of pointers: txdataF[beam][antenna]
 * @param num_beams Number of beams
 * @param num_antennas Number of antennas per beam
 * @param offset Sample offset within buffer
 * @param samples_per_slot Number of samples to clear per antenna
 */
void isip_pool_memclear_tx(void*** txdataF, int num_beams, int num_antennas,
                             int offset, int samples_per_slot);

/**
 * Start async memory clear for a future slot (deferred clearing)
 * Launches memclear in background without waiting - removes memclear from critical path.
 * Call isip_pool_memclear_tx_wait() before using the cleared buffer.
 *
 * Typical usage (ping-pong pattern):
 *   Slot N:   wait_for_async()  -> process slot N -> start_async(slot N+2)
 *   Slot N+1: wait_for_async()  -> process slot N+1 -> start_async(slot N+3)
 *
 * @param txdataF Array of pointers: txdataF[beam][antenna]
 * @param num_beams Number of beams
 * @param num_antennas Number of antennas per beam
 * @param offset Sample offset within buffer (for future slot)
 * @param samples_per_slot Number of samples to clear per antenna
 */
void isip_pool_memclear_tx_async(void*** txdataF, int num_beams, int num_antennas,
                                   int offset, int samples_per_slot);

/**
 * Wait for async memory clear to complete
 * Safe to call even if no async clear is in progress (returns immediately)
 */
void isip_pool_memclear_tx_wait(void);

/**
 * Parallel memcpy for RU TX buffers (feptx_prec acceleration)
 * Copies txdataF to txdataF_BF in parallel using ISIP pool
 * NOTE: Calling thread must be registered first with isip_pool_register_external_thread()
 *
 * @param dst_buffers Destination array: txdataF_BF[tx_idx]
 * @param src_buffers Source array: txdataF[beam][antenna]
 * @param num_beams Number of beams
 * @param num_antennas Number of antennas per beam
 * @param src_offset Sample offset in source buffer
 * @param samples_per_slot Number of samples to copy per antenna
 */
void isip_pool_memcpy_tx(void** dst_buffers, void*** src_buffers,
                           int num_beams, int num_antennas,
                           int src_offset, int samples_per_slot);

// ============================================================================
// DMRS Precompute Functions
// ============================================================================

/**
 * Start async DMRS precomputation for multiple PDSCHs in parallel
 * Runs DMRS modulation in background while other processing continues.
 * Call isip_pool_dmrs_precompute_wait() before using the results.
 *
 * @param params Array of DMRS parameters, one per PDSCH
 * @param num_pdsch Number of PDSCHs to process (max ISIP_MAX_PDSCH_PER_SLOT)
 */
void isip_pool_dmrs_precompute_async(isip_dmrs_params_t *params, int num_pdsch);

/**
 * Wait for async DMRS precomputation to complete
 * Safe to call even if no async precompute is in progress (returns immediately)
 */
void isip_pool_dmrs_precompute_wait(void);

/**
 * Check if DMRS precompute is enabled and available
 * @return 1 if DMRS precompute can be used, 0 otherwise
 */
int isip_pool_dmrs_precompute_enabled(void);

// ============================================================================
// Parallel Segmentation Functions
// ============================================================================

/**
 * Execute parallel segmentation for a transport block
 * Segments are processed in parallel using ISIP pool workers.
 * Each segment: memcpy from input -> crc24b (if C>1) -> filler zeroing
 *
 * @param params Segmentation parameters (input, outputs, C, K, etc.)
 *               Must be filled with valid segmentation parameters before calling.
 *
 * Note: For C==1 (single segment), this falls back to sequential processing
 *       as there's no benefit from parallelization.
 */
void isip_pool_segmentation_parallel(isip_seg_params_t *params);

/**
 * Check if parallel segmentation is available
 * @return 1 if ISIP pool is initialized and can be used, 0 otherwise
 */
int isip_pool_segmentation_enabled(void);

#ifdef __cplusplus
}
#endif
