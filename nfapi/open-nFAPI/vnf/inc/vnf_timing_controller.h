/*
 * VNF Robust Dynamic Timing Controller
 * 
 * Purpose: Provides traffic-aware timing synchronization for O-RAN nFAPI split
 *          environments where network queuing delays cause traditional NTP-style
 *          clock sync algorithms to fail.
 * 
 * Key Design Principles:
 * 1. Windowed Minimum Filter: Uses min(OWD) over a sliding window to extract
 *    true clock offset, rejecting queuing delay artifacts.
 * 2. Traffic-Aware Advance Calculation: Dynamically adjusts packet send times
 *    based on observed jitter, load (rb_size), and direction (DL/UL).
 * 3. Cross-Slot Boundary Handling: Supports scheduling packets in earlier slots
 *    when required advance exceeds current slot's available time.
 * 
 * Copyright 2024 OpenAirInterface
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _VNF_TIMING_CONTROLLER_H_
#define _VNF_TIMING_CONTROLLER_H_

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Configuration Constants
 *===========================================================================*/

/* Sliding window size for min-filter (number of samples) */
#define VNF_TC_WINDOW_SIZE          64

/* Minimum number of samples required before trusting the filter output */
#define VNF_TC_MIN_SAMPLES          8

/* Safety guard added to all advance calculations (microseconds) */
#define VNF_TC_SAFETY_GUARD_US      50

/* Maximum allowed advance time (to prevent excessive buffering) */
#define VNF_TC_MAX_ADVANCE_US       5000

/* Minimum advance time (must be positive for on-time arrival) */
#define VNF_TC_MIN_ADVANCE_US       100

/* Exponential moving average alpha for jitter (0.0-1.0, higher = more responsive) */
#define VNF_TC_JITTER_EMA_ALPHA     0.125f

/* Offset convergence threshold (microseconds) - sync locks when within this */
#define VNF_TC_OFFSET_LOCK_THRESHOLD_US  20

/* Number of consecutive converged samples before locking */
#define VNF_TC_LOCK_COUNT_THRESHOLD      5

/*===========================================================================
 * Traffic-Aware Coefficient Profiles
 *===========================================================================*/

/* Coefficients for dynamic advance calculation by direction */
typedef struct {
    float rb_size_coeff;      /* Microseconds per RB (processing time) */
    float jitter_coeff;       /* Multiplier for jitter headroom */
    int32_t base_margin_us;   /* Base margin for this direction */
} vnf_tc_direction_profile_t;

/* Default profiles - can be tuned per deployment */
#define VNF_TC_DL_PROFILE_DEFAULT { \
    .rb_size_coeff = 0.5f,          /* 0.5us per RB for DL processing */ \
    .jitter_coeff = 2.0f,           /* 2x jitter headroom */ \
    .base_margin_us = 300           /* 300us base margin for DL */ \
}

#define VNF_TC_UL_PROFILE_DEFAULT { \
    .rb_size_coeff = 0.3f,          /* 0.3us per RB for UL processing */ \
    .jitter_coeff = 1.5f,           /* 1.5x jitter headroom */ \
    .base_margin_us = 200           /* 200us base margin for UL */ \
}

/*===========================================================================
 * Timing Sample Structure (for min-filter window)
 *===========================================================================*/

typedef struct {
    uint32_t t1;              /* VNF timestamp at DL_NODE_SYNC send */
    uint32_t t2;              /* PNF timestamp at DL_NODE_SYNC receive */
    uint32_t t3;              /* PNF timestamp at UL_NODE_SYNC send */
    uint32_t t4;              /* VNF timestamp at UL_NODE_SYNC receive */
    int32_t  offset;          /* Calculated clock offset ((t2-t1) - (t4-t3))/2 */
    int32_t  owd_fwd;         /* Forward OWD: t2 - t1 */
    int32_t  owd_rev;         /* Reverse OWD: t4 - t3 */
    int32_t  owd_min;         /* min(owd_fwd, owd_rev) */
    uint32_t timestamp_us;    /* Local monotonic timestamp when sample was taken */
    uint16_t sfn;             /* SFN when sample was taken */
    uint16_t slot;            /* Slot when sample was taken */
    uint8_t  valid;           /* Sample validity flag */
} vnf_tc_timing_sample_t;

/*===========================================================================
 * Timing Statistics (for logging/analysis)
 *===========================================================================*/

typedef struct {
    /* OWD statistics from current window */
    int32_t owd_min;          /* Minimum OWD in window */
    int32_t owd_max;          /* Maximum OWD in window */
    int32_t owd_avg;          /* Average OWD in window */
    
    /* Jitter estimate (EMA-filtered) */
    float jitter_estimate_us;
    
    /* Offset statistics */
    int32_t offset_raw;       /* Raw offset from latest sample */
    int32_t offset_filtered;  /* Min-filtered offset (baseline) */
    int32_t offset_trend;     /* Offset trend (drift rate) */
    
    /* Convergence tracking */
    uint32_t samples_in_window;
    uint32_t consecutive_converged;
    uint8_t  sync_locked;
    
    /* Dynamic advance calculation result */
    int32_t last_calculated_advance_us;
    int32_t last_slot_offset;
} vnf_tc_stats_t;

/*===========================================================================
 * Main Timing Controller Structure
 *===========================================================================*/

typedef struct vnf_timing_controller {
    /* Circular buffer for timing samples (min-filter window) */
    vnf_tc_timing_sample_t samples[VNF_TC_WINDOW_SIZE];
    uint32_t sample_head;           /* Next write position */
    uint32_t sample_count;          /* Total valid samples in window */
    
    /* Min-filter state */
    uint32_t min_owd_idx;           /* Index of sample with minimum OWD */
    int32_t  baseline_offset;       /* Clock offset derived from min-OWD sample */
    int32_t  baseline_owd;          /* Minimum OWD (physical propagation delay) */
    
    /* Jitter tracking (EMA-filtered max-min spread) */
    float jitter_ema_us;
    
    /* Direction-specific profiles */
    vnf_tc_direction_profile_t dl_profile;
    vnf_tc_direction_profile_t ul_profile;
    
    /* Numerology-dependent parameters */
    int mu;
    int32_t slot_duration_us;
    
    /* Sync lock state */
    uint8_t sync_locked;
    uint32_t consecutive_converged;
    
    /* TIMING_INFO feedback tracking */
    int32_t latest_dl_delay_us;     /* Latest DL packet delay from TIMING_INFO */
    int32_t latest_tx_delay_us;     /* Latest TX_DATA delay from TIMING_INFO */
    int32_t latest_ul_delay_us;     /* Latest UL packet delay from TIMING_INFO */
    int32_t max_delay_us;           /* Maximum of all delays */
    int32_t delta_slots;            /* PNF slot - VNF slot (positive = VNF behind) */
    uint32_t timing_info_count;     /* Number of TIMING_INFO messages received */
    
    /* Adjustment control - prevent oscillation */
    uint32_t last_adjustment_time;  /* Timestamp of last adjustment (for cooldown) */
    uint32_t adjustment_cooldown_ms;/* Minimum time between adjustments */
    int32_t pending_adjustment;     /* Adjustment in progress (waiting for effect) */
    uint32_t adjustment_settle_count; /* TIMING_INFO messages to wait after adjustment */
    
    /* Congestion detection - prevent adjustments during network storms */
    int64_t min_rtt_us;             /* Minimum RTT observed (baseline) */
    int64_t recent_rtt_us;          /* Most recent RTT measurement */
    int64_t rtt_spike_threshold;    /* RTT above this = congestion (5x min_rtt) */
    uint8_t network_congested;      /* Flag: 1 = congested, skip adjustments */
    uint32_t congestion_count;      /* How many consecutive congested samples */
    uint32_t stable_count;          /* How many consecutive stable samples after congestion */
    
    /* Output adjustments (applied by timing thread) */
    volatile int32_t us_adjustment;
    volatile int32_t slot_adjustment;
    
    /* Statistics for logging */
    vnf_tc_stats_t stats;
    
    /* Thread safety */
    pthread_mutex_t lock;
    
    /* Logging control */
    uint8_t csv_logging_enabled;
    int csv_log_fd;
    
} vnf_timing_controller_t;

/*===========================================================================
 * Direction Enum for Traffic-Aware Scheduling
 *===========================================================================*/

typedef enum {
    VNF_TC_DIR_DL = 0,
    VNF_TC_DIR_UL = 1
} vnf_tc_direction_t;

/*===========================================================================
 * Function Declarations
 *===========================================================================*/

/**
 * vnf_tc_init - Initialize the timing controller
 * @tc: Pointer to timing controller structure
 * @mu: Numerology (0-3)
 * 
 * Initializes all internal state, sets default profiles, and prepares
 * the controller for operation.
 */
void vnf_tc_init(vnf_timing_controller_t* tc, int mu);

/**
 * vnf_tc_destroy - Clean up timing controller resources
 * @tc: Pointer to timing controller structure
 */
void vnf_tc_destroy(vnf_timing_controller_t* tc);

/**
 * vnf_tc_update_clock_offset - Process a new UL_NODE_SYNC timing sample
 * @tc: Pointer to timing controller structure
 * @t1: VNF timestamp at DL_NODE_SYNC send
 * @t2: PNF timestamp at DL_NODE_SYNC receive
 * @t3: PNF timestamp at UL_NODE_SYNC send
 * @t4: VNF timestamp at UL_NODE_SYNC receive
 * @sfn: Current SFN
 * @slot: Current slot
 * 
 * This function implements the Windowed Minimum Filter algorithm:
 * 1. Calculates OWD and offset from the timing tuple
 * 2. Adds sample to the circular buffer
 * 3. Updates the min-OWD sample pointer
 * 4. Recalculates baseline offset from the min-OWD sample
 * 5. Updates jitter estimate (EMA of max-min OWD spread)
 * 6. Checks for sync convergence
 * 
 * Returns: 0 on success, -1 on error
 */
int vnf_tc_update_clock_offset(vnf_timing_controller_t* tc,
                               uint32_t t1, uint32_t t2,
                               uint32_t t3, uint32_t t4,
                               uint16_t sfn, uint16_t slot);

/**
 * vnf_tc_calculate_tx_advance - Calculate dynamic transmission advance time
 * @tc: Pointer to timing controller structure
 * @target_sfn: Target SFN for the message
 * @target_slot: Target slot for the message
 * @rb_size: Resource block size (load indicator)
 * @direction: DL or UL direction
 * @out_advance_us: Output: required advance time in microseconds
 * @out_slot_offset: Output: number of slots to send earlier (0 = current slot)
 * 
 * This function implements Traffic-Aware Scheduling:
 * Formula: Required_Advance = Base_Network_Delay + (Coeff_A * rb_size) 
 *                            + (Coeff_B * current_jitter_estimate) + Safety_Guard
 * 
 * Cross-Slot Boundary Logic:
 * If Required_Advance > slot_duration, calculates how many slots earlier
 * the packet must be transmitted.
 * 
 * Returns: 0 on success, -1 on error
 */
int vnf_tc_calculate_tx_advance(vnf_timing_controller_t* tc,
                                uint16_t target_sfn, uint16_t target_slot,
                                uint16_t rb_size,
                                vnf_tc_direction_t direction,
                                int32_t* out_advance_us,
                                int32_t* out_slot_offset);

/**
 * vnf_tc_get_adjustments - Get current timing adjustments for the timing thread
 * @tc: Pointer to timing controller structure
 * @out_us_adj: Output: microsecond adjustment to apply
 * @out_slot_adj: Output: slot adjustment to apply
 * 
 * Atomically retrieves and clears the pending adjustments.
 */
void vnf_tc_get_adjustments(vnf_timing_controller_t* tc,
                            int32_t* out_us_adj,
                            int32_t* out_slot_adj);

/**
 * vnf_tc_get_stats - Get current statistics snapshot
 * @tc: Pointer to timing controller structure
 * @out_stats: Output: statistics structure
 */
void vnf_tc_get_stats(vnf_timing_controller_t* tc, vnf_tc_stats_t* out_stats);

/**
 * vnf_tc_is_locked - Check if sync is locked
 * @tc: Pointer to timing controller structure
 * 
 * Returns: 1 if locked, 0 if still converging
 */
int vnf_tc_is_locked(vnf_timing_controller_t* tc);

/**
 * vnf_tc_process_timing_info - Process TIMING_INFO feedback from PNF
 * @tc: Pointer to timing controller structure
 * @pnf_sfn: PNF's current SFN
 * @pnf_slot: PNF's current slot
 * @vnf_sfn: VNF's current SFN
 * @vnf_slot: VNF's current slot
 * @dl_delay: DL_TTI latest delay in microseconds (positive = late)
 * @tx_delay: TX_DATA latest delay in microseconds
 * @ul_delay: UL_TTI latest delay in microseconds
 * @out_slot_adj: Output: recommended slot adjustment
 * @out_us_adj: Output: recommended microsecond adjustment
 * 
 * This function uses the TIMING_INFO feedback to calculate how much
 * the VNF timing needs to be adjusted to prevent late packet arrival.
 * 
 * Returns: 0 on success, -1 on error
 */
int vnf_tc_process_timing_info(vnf_timing_controller_t* tc,
                               uint16_t pnf_sfn, uint16_t pnf_slot,
                               uint16_t vnf_sfn, uint16_t vnf_slot,
                               int32_t dl_delay, int32_t tx_delay, int32_t ul_delay,
                               int32_t* out_slot_adj, int32_t* out_us_adj);

/**
 * vnf_tc_reset - Reset the timing controller state
 * @tc: Pointer to timing controller structure
 * 
 * Clears all samples and resets to initial state (unlocked).
 */
void vnf_tc_reset(vnf_timing_controller_t* tc);

/**
 * vnf_tc_log_csv - Log timing data in CSV format for offline analysis
 * @tc: Pointer to timing controller structure
 * @sfn: Current SFN
 * @slot: Current slot
 * @rb_size: Resource block size
 * @direction: DL or UL
 * @measured_margin: Actual measured margin at PNF (from timing_info)
 * 
 * Format: sfn,slot,rb_size,direction,current_owd,current_offset,baseline_offset,jitter_est,calculated_advance,measured_margin
 */
void vnf_tc_log_csv(vnf_timing_controller_t* tc,
                    uint16_t sfn, uint16_t slot,
                    uint16_t rb_size,
                    vnf_tc_direction_t direction,
                    int32_t measured_margin);

/**
 * vnf_tc_enable_csv_logging - Enable CSV logging to file
 * @tc: Pointer to timing controller structure
 * @filepath: Path to CSV output file
 * 
 * Returns: 0 on success, -1 on error
 */
int vnf_tc_enable_csv_logging(vnf_timing_controller_t* tc, const char* filepath);

/**
 * vnf_tc_disable_csv_logging - Disable CSV logging
 * @tc: Pointer to timing controller structure
 */
void vnf_tc_disable_csv_logging(vnf_timing_controller_t* tc);

/**
 * vnf_tc_allocate - Allocate and initialize a timing controller
 * @mu: Numerology (0-3)
 * 
 * Returns: Pointer to allocated timing controller, or NULL on error
 */
vnf_timing_controller_t* vnf_tc_allocate(int mu);

/**
 * vnf_tc_free - Free a timing controller
 * @tc: Pointer to timing controller structure
 */
void vnf_tc_free(vnf_timing_controller_t* tc);

#ifdef __cplusplus
}
#endif

#endif /* _VNF_TIMING_CONTROLLER_H_ */
