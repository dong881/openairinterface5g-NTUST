/*
 * VNF Robust Dynamic Timing Controller - Implementation
 * 
 * This implements a traffic-aware timing synchronization algorithm for O-RAN
 * nFAPI split environments. Key features:
 * 
 * 1. WINDOWED MINIMUM FILTER for clock offset estimation
 *    - Traditional NTP-style offset = ((t2-t1) - (t4-t3))/2 fails under load
 *    - Network queuing delays cause t2-t1 to spike, corrupting the offset
 *    - Solution: Track min(OWD) over a sliding window - this represents the
 *      "lucky" packet that experienced zero queuing, giving true offset
 * 
 * 2. TRAFFIC-AWARE ADVANCE CALCULATION
 *    - Static margins fail because VNF processing time varies with rb_size
 *    - Dynamic formula: advance = base_delay + (coeff_rb * rb_size) + 
 *                                 (coeff_jitter * jitter_est) + safety
 * 
 * 3. CROSS-SLOT BOUNDARY HANDLING
 *    - When required advance > slot_duration, schedule in earlier slot
 * 
 * Copyright 2024 OpenAirInterface
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>

#include "vnf_timing_controller.h"
#include "nfapi_interface.h"
#include "debug.h"

/*===========================================================================
 * Internal Helper: Get monotonic time in microseconds
 *===========================================================================*/
static uint32_t get_monotonic_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL) & 0xFFFFFFFF);
}

/*===========================================================================
 * Internal Helper: RTT-Based Timing Metrics for Distributed Clocks
 * 
 * CRITICAL INSIGHT: VNF and PNF run on DIFFERENT MACHINES with different
 * CLOCK_MONOTONIC bases. We CANNOT directly compare t1 with t2, or t3 with t4,
 * because these timestamps come from clocks that may differ by SECONDS!
 * 
 * SOLUTION: Use only RELATIVE time differences within each machine:
 *   - delta_vnf = t4 - t1 (both on VNF clock, so this is valid)
 *   - delta_pnf = t3 - t2 (both on PNF clock, so this is valid)
 *   - RTT = delta_vnf - delta_pnf (machine-independent, always ~100-500µs)
 *   - OWD = RTT / 2 (assuming symmetric network)
 * 
 * Timestamps:
 *   t1: VNF sends DL_NODE_SYNC (VNF clock)
 *   t2: PNF receives DL_NODE_SYNC (PNF clock) 
 *   t3: PNF sends UL_NODE_SYNC (PNF clock)
 *   t4: VNF receives UL_NODE_SYNC (VNF clock)
 *===========================================================================*/
static void calculate_timing_metrics(uint32_t t1, uint32_t t2, 
                                      uint32_t t3, uint32_t t4,
                                      int32_t* out_offset,
                                      int32_t* out_owd_fwd,
                                      int32_t* out_owd_rev,
                                      int32_t* out_owd_min)
{
    /*
     * Calculate time elapsed on each machine - these are VALID comparisons
     * because both timestamps in each subtraction are from the SAME clock!
     */
    int64_t delta_vnf = (int64_t)(t4 - t1);  /* Time elapsed on VNF (microseconds) */
    int64_t delta_pnf = (int64_t)(t3 - t2);  /* Time elapsed on PNF (microseconds) */
    
    /* Handle 32-bit wraparound for delta_vnf */
    if (delta_vnf < 0) delta_vnf += 0x100000000LL;
    if (delta_vnf > 0x80000000LL) delta_vnf -= 0x100000000LL;
    
    /* Handle 32-bit wraparound for delta_pnf */
    if (delta_pnf < 0) delta_pnf += 0x100000000LL;
    if (delta_pnf > 0x80000000LL) delta_pnf -= 0x100000000LL;
    
    /*
     * RTT = total VNF time - PNF processing time
     * This is the TRUE round-trip network delay, independent of clock bases!
     * 
     * Why this works:
     *   delta_vnf = network_forward + pnf_processing + network_reverse
     *   delta_pnf = pnf_processing
     *   RTT = delta_vnf - delta_pnf = network_forward + network_reverse
     * 
     * Should be ~100-500µs for a local network
     */
    int32_t rtt = (int32_t)(delta_vnf - delta_pnf);
    
    /* Sanity check: RTT should be positive and reasonable */
    if (rtt < 10) {
        /* RTT < 10µs is likely an error or measurement noise */
        rtt = 100;  /* Default to 100µs as fallback */
    }
    if (rtt > 100000) {
        /* RTT > 100ms is unreasonable for local network, likely error */
        rtt = 500;  /* Default to 500µs as fallback */
    }
    
    /* Symmetric OWD estimate = RTT / 2 */
    int32_t owd_estimate = rtt / 2;
    
    /*
     * Output values:
     * - offset: We repurpose this as RTT (the machine-independent metric)
     *           The downstream code uses this for min-filtering
     * - owd_fwd/owd_rev: Set to OWD estimate (symmetric assumption)
     * - owd_min: The OWD estimate used for scheduling
     */
    *out_offset = rtt;  /* RTT stored here for tracking and logging */
    *out_owd_fwd = owd_estimate;
    *out_owd_rev = owd_estimate;
    *out_owd_min = owd_estimate;
}

/*===========================================================================
 * Internal Helper: Find minimum OWD sample in window
 *===========================================================================*/
static void find_min_owd_sample(vnf_timing_controller_t* tc)
{
    int32_t min_owd = INT32_MAX;
    uint32_t min_idx = 0;
    uint32_t count = (tc->sample_count < VNF_TC_WINDOW_SIZE) ? 
                      tc->sample_count : VNF_TC_WINDOW_SIZE;
    
    for (uint32_t i = 0; i < count; i++) {
        if (tc->samples[i].valid && tc->samples[i].owd_min < min_owd) {
            min_owd = tc->samples[i].owd_min;
            min_idx = i;
        }
    }
    
    if (min_owd != INT32_MAX) {
        tc->min_owd_idx = min_idx;
        tc->baseline_owd = min_owd;
        /* baseline_offset now stores min RTT (offset field contains RTT in RTT-based mode) */
        tc->baseline_offset = tc->samples[min_idx].offset;
    }
}

/*===========================================================================
 * Internal Helper: Calculate OWD statistics for jitter estimation
 *===========================================================================*/
static void calculate_owd_stats(vnf_timing_controller_t* tc)
{
    int32_t min_owd = INT32_MAX;
    int32_t max_owd = INT32_MIN;
    int64_t sum_owd = 0;
    uint32_t valid_count = 0;
    
    uint32_t count = (tc->sample_count < VNF_TC_WINDOW_SIZE) ? 
                      tc->sample_count : VNF_TC_WINDOW_SIZE;
    
    for (uint32_t i = 0; i < count; i++) {
        if (tc->samples[i].valid) {
            int32_t owd = tc->samples[i].owd_min;
            if (owd < min_owd) min_owd = owd;
            if (owd > max_owd) max_owd = owd;
            sum_owd += owd;
            valid_count++;
        }
    }
    
    if (valid_count > 0) {
        tc->stats.owd_min = min_owd;
        tc->stats.owd_max = max_owd;
        tc->stats.owd_avg = (int32_t)(sum_owd / valid_count);
        
        /* Jitter estimate: EMA of (max - min) spread in window */
        float current_spread = (float)(max_owd - min_owd);
        tc->jitter_ema_us = (1.0f - VNF_TC_JITTER_EMA_ALPHA) * tc->jitter_ema_us +
                            VNF_TC_JITTER_EMA_ALPHA * current_spread;
        tc->stats.jitter_estimate_us = tc->jitter_ema_us;
    }
    
    tc->stats.samples_in_window = valid_count;
}

/*===========================================================================
 * vnf_tc_init - Initialize the timing controller
 *===========================================================================*/
void vnf_tc_init(vnf_timing_controller_t* tc, int mu)
{
    if (!tc) return;
    
    memset(tc, 0, sizeof(vnf_timing_controller_t));
    
    /* Set numerology-dependent parameters */
    tc->mu = mu;
    tc->slot_duration_us = 1000 >> mu;  /* 1000us for mu0, 500us for mu1, etc. */
    
    /* Initialize default direction profiles */
    vnf_tc_direction_profile_t dl_default = VNF_TC_DL_PROFILE_DEFAULT;
    vnf_tc_direction_profile_t ul_default = VNF_TC_UL_PROFILE_DEFAULT;
    tc->dl_profile = dl_default;
    tc->ul_profile = ul_default;
    
    /* Scale base margin by numerology (shorter slots need proportionally smaller margins) */
    /* But keep minimum viable margins */
    int32_t mu_scale = 1 << mu;  /* 1 for mu0, 2 for mu1, 4 for mu2, 8 for mu3 */
    tc->dl_profile.base_margin_us = (tc->dl_profile.base_margin_us * 2) / mu_scale;
    tc->ul_profile.base_margin_us = (tc->ul_profile.base_margin_us * 2) / mu_scale;
    
    /* Ensure minimum margins */
    if (tc->dl_profile.base_margin_us < 100) tc->dl_profile.base_margin_us = 100;
    if (tc->ul_profile.base_margin_us < 100) tc->ul_profile.base_margin_us = 100;
    
    /* Initialize sample buffer */
    tc->sample_head = 0;
    tc->sample_count = 0;
    
    /* Initialize filter state */
    tc->min_owd_idx = 0;
    tc->baseline_offset = 0;
    tc->baseline_owd = 0;
    tc->jitter_ema_us = 100.0f;  /* Start with conservative jitter estimate */
    
    /* Initialize sync state */
    tc->sync_locked = 0;
    tc->consecutive_converged = 0;
    tc->us_adjustment = 0;
    tc->slot_adjustment = 0;
    
    /* Initialize TIMING_INFO feedback tracking */
    tc->latest_dl_delay_us = 0;
    tc->latest_tx_delay_us = 0;
    tc->latest_ul_delay_us = 0;
    tc->max_delay_us = 0;
    tc->delta_slots = 0;
    tc->timing_info_count = 0;
    
    /* Initialize adjustment control (prevent oscillation) */
    tc->last_adjustment_time = 0;
    tc->adjustment_cooldown_ms = 100;  /* 100ms minimum between adjustments */
    tc->pending_adjustment = 0;
    tc->adjustment_settle_count = 0;
    
    /* Initialize congestion detection */
    tc->min_rtt_us = INT64_MAX;        /* Will be set on first RTT measurement */
    tc->recent_rtt_us = 0;
    tc->rtt_spike_threshold = 0;       /* Set after first min_rtt measurement */
    tc->network_congested = 0;
    tc->congestion_count = 0;
    tc->stable_count = 0;
    
    /* Initialize logging */
    tc->csv_logging_enabled = 0;
    tc->csv_log_fd = -1;
    
    pthread_mutex_init(&tc->lock, NULL);
    
    NFAPI_TRACE(NFAPI_TRACE_INFO, 
        "[VNF_TC] Initialized: mu=%d slot_duration=%dus dl_margin=%dus ul_margin=%dus\n",
        mu, tc->slot_duration_us, tc->dl_profile.base_margin_us, tc->ul_profile.base_margin_us);
}

/*===========================================================================
 * vnf_tc_destroy - Clean up timing controller resources
 *===========================================================================*/
void vnf_tc_destroy(vnf_timing_controller_t* tc)
{
    if (!tc) return;
    
    if (tc->csv_logging_enabled && tc->csv_log_fd >= 0) {
        close(tc->csv_log_fd);
    }
    
    pthread_mutex_destroy(&tc->lock);
}

/*===========================================================================
 * vnf_tc_update_clock_offset - Process a new UL_NODE_SYNC timing sample
 * 
 * ALGORITHM: Windowed Minimum Filter
 * 
 * Why this works:
 * - Network queuing causes OWD to increase unpredictably
 * - But queuing is transient - some packets will get through with minimal delay
 * - The minimum OWD in a window represents the "clean" path: physical propagation
 *   delay + serialization delay + true clock offset, with ZERO queuing
 * - Using this sample for offset estimation removes the queuing artifact
 * 
 * Why simple averaging fails:
 * - Under iPerf load, 90% of packets may have 1-5ms queuing delay
 * - Average is dominated by these delayed packets
 * - Result: calculated offset includes network congestion as if it were clock drift
 * - VNF tries to "correct" for this phantom drift, causing oscillation
 *===========================================================================*/
int vnf_tc_update_clock_offset(vnf_timing_controller_t* tc,
                               uint32_t t1, uint32_t t2,
                               uint32_t t3, uint32_t t4,
                               uint16_t sfn, uint16_t slot)
{
    if (!tc) return -1;
    
    pthread_mutex_lock(&tc->lock);
    
    /* Calculate timing metrics from the t1-t4 tuple */
    int32_t offset, owd_fwd, owd_rev, owd_min;
    calculate_timing_metrics(t1, t2, t3, t4, &offset, &owd_fwd, &owd_rev, &owd_min);
    
    /*=========================================================================
     * CONGESTION DETECTION
     * 
     * Track min RTT as baseline. If current RTT >> min RTT, the network is
     * congested and we should NOT adjust timing (adjustments during congestion
     * make things worse).
     * 
     * From user analysis: RTT spikes from 134µs to 9639µs (70x!) during iperf
     * Adjusting timing during this transient congestion caused timing collapse.
     *=========================================================================*/
    int64_t current_rtt = (int64_t)offset;
    tc->recent_rtt_us = current_rtt;
    
    /* Update minimum RTT if this is a new low */
    if (current_rtt > 0 && current_rtt < tc->min_rtt_us) {
        tc->min_rtt_us = current_rtt;
        /* Set spike threshold at 5x min RTT (e.g., 134µs * 5 = 670µs) */
        tc->rtt_spike_threshold = tc->min_rtt_us * 5;
        if (tc->rtt_spike_threshold < 500) {
            tc->rtt_spike_threshold = 500;  /* Minimum 500µs threshold */
        }
    }
    
    /* Detect congestion: RTT > 5x baseline means network is congested */
    uint8_t was_congested = tc->network_congested;
    if (tc->min_rtt_us < INT64_MAX && tc->rtt_spike_threshold > 0) {
        if (current_rtt > tc->rtt_spike_threshold) {
            /* RTT spike detected - network congested */
            tc->network_congested = 1;
            tc->congestion_count++;
            tc->stable_count = 0;
            
            if (!was_congested) {
                NFAPI_TRACE(NFAPI_TRACE_WARN,
                    "[VNF_TC] CONGESTION DETECTED! RTT=%ldus >> min_rtt=%ldus (threshold=%ldus) "
                    "- PAUSING ADJUSTMENTS\n",
                    (long)current_rtt, (long)tc->min_rtt_us, (long)tc->rtt_spike_threshold);
            }
        } else {
            /* RTT is normal */
            tc->stable_count++;
            /* Require 10 consecutive stable samples to exit congestion state */
            if (tc->stable_count >= 10) {
                if (tc->network_congested) {
                    NFAPI_TRACE(NFAPI_TRACE_INFO,
                        "[VNF_TC] Network stable again. RTT=%ldus (min=%ldus). "
                        "Resuming normal operation after %u congested samples.\n",
                        (long)current_rtt, (long)tc->min_rtt_us, tc->congestion_count);
                }
                tc->network_congested = 0;
                tc->congestion_count = 0;
            }
        }
    }
    
    /* Log the RTT calculation for debugging (first few samples or every 100th) */
    if (tc->sample_count < 10 || (tc->sample_count % 100) == 0) {
        int32_t delta_vnf = (int32_t)(t4 - t1);
        int32_t delta_pnf = (int32_t)(t3 - t2);
        NFAPI_TRACE(NFAPI_TRACE_INFO,
            "[VNF_TC] RTT Calc #%u: delta_vnf=%d (t4-t1), delta_pnf=%d (t3-t2), "
            "RTT=%d, OWD=%d, min_rtt=%ld, congested=%d\n",
            tc->sample_count, delta_vnf, delta_pnf, offset, owd_min,
            (long)tc->min_rtt_us, tc->network_congested);
    }
    
    /* Store sample in circular buffer */
    vnf_tc_timing_sample_t* sample = &tc->samples[tc->sample_head];
    sample->t1 = t1;
    sample->t2 = t2;
    sample->t3 = t3;
    sample->t4 = t4;
    sample->offset = offset;
    sample->owd_fwd = owd_fwd;
    sample->owd_rev = owd_rev;
    sample->owd_min = owd_min;
    sample->timestamp_us = get_monotonic_time_us();
    sample->sfn = sfn;
    sample->slot = slot;
    sample->valid = 1;
    
    /* Advance circular buffer head */
    tc->sample_head = (tc->sample_head + 1) % VNF_TC_WINDOW_SIZE;
    tc->sample_count++;
    
    /* Update statistics */
    tc->stats.offset_raw = offset;
    
    /* Find the sample with minimum OWD - this is our "clean" reference */
    find_min_owd_sample(tc);
    
    /* Calculate OWD statistics and update jitter estimate */
    calculate_owd_stats(tc);
    
    tc->stats.offset_filtered = tc->baseline_offset;
    
    /* Only apply adjustments if we have enough samples and not locked */
    if (tc->sample_count >= VNF_TC_MIN_SAMPLES && !tc->sync_locked) {
        
        /*
         * RTT-BASED SYNCHRONIZATION STRATEGY
         * 
         * Since we can't measure absolute clock offset between machines,
         * we use RTT (round-trip time) to determine network delay.
         * 
         * baseline_owd = min(RTT/2) over the window = minimum network delay
         * 
         * The adjustment strategy is:
         * 1. Track RTT stability (jitter)
         * 2. When RTT stabilizes to reasonable values, we're "synced"
         * 3. No slot/time adjustments needed - just use baseline_owd for scheduling
         * 
         * "Sync locked" means: we have stable RTT measurements
         */
        
        /* Check RTT stability: min RTT should be reasonable (10µs - 10ms) */
        int32_t rtt_min = tc->baseline_offset;  /* Note: offset now contains RTT */
        int32_t owd_min = tc->baseline_owd;
        
        /* RTT is considered stable if it's in a reasonable range */
        int rtt_valid = (rtt_min >= 10 && rtt_min <= 10000);
        
        /* Jitter should be low for stable sync (< 1ms) */
        int jitter_stable = (tc->jitter_ema_us < 1000.0f);
        
        if (rtt_valid && jitter_stable) {
            tc->consecutive_converged++;
            if (tc->consecutive_converged >= VNF_TC_LOCK_COUNT_THRESHOLD) {
                tc->sync_locked = 1;
                tc->us_adjustment = 0;
                tc->slot_adjustment = 0;
                NFAPI_TRACE(NFAPI_TRACE_INFO,
                    "[VNF_TC] RTT STABLE! rtt_min=%dus owd_min=%dus jitter=%.1fus\n",
                    rtt_min, owd_min, tc->jitter_ema_us);
            }
        } else {
            tc->consecutive_converged = 0;
            
            /*
             * No slot/us adjustments for clock offset - we can't know it!
             * Instead, the vnf_tc_calculate_tx_advance() function will use
             * baseline_owd to schedule packets early enough.
             */
            tc->us_adjustment = 0;
            tc->slot_adjustment = 0;
        }
    }
    
    /* Update stats */
    tc->stats.consecutive_converged = tc->consecutive_converged;
    tc->stats.sync_locked = tc->sync_locked;
    
    NFAPI_TRACE(NFAPI_TRACE_DEBUG,
        "[VNF_TC] Sample: t1=%u t2=%u t3=%u t4=%u delta_vnf=%d delta_pnf=%d "
        "rtt=%d owd=%d min_rtt=%d min_owd=%d jitter=%.1f locked=%d\n",
        t1, t2, t3, t4, (int32_t)(t4-t1), (int32_t)(t3-t2),
        offset, owd_min, tc->baseline_offset, tc->baseline_owd, 
        tc->jitter_ema_us, tc->sync_locked);
    
    pthread_mutex_unlock(&tc->lock);
    
    return 0;
}

/*===========================================================================
 * vnf_tc_calculate_tx_advance - Calculate dynamic transmission advance time
 * 
 * ALGORITHM: Traffic-Aware Scheduling
 * 
 * Formula:
 *   Required_Advance = Base_Network_Delay + (Coeff_A * rb_size) +
 *                      (Coeff_B * current_jitter_estimate) + Safety_Guard
 * 
 * Where:
 *   Base_Network_Delay = baseline_owd (from min-filter)
 *   Coeff_A = rb_size_coeff (processing time per RB)
 *   Coeff_B = jitter_coeff (headroom multiplier for jitter)
 *   Safety_Guard = fixed additional margin
 * 
 * Cross-Slot Boundary:
 *   If Required_Advance > (current_slot_remaining_time + slot_duration * N),
 *   we need to schedule in slot (current - N - 1)
 *===========================================================================*/
int vnf_tc_calculate_tx_advance(vnf_timing_controller_t* tc,
                                uint16_t target_sfn, uint16_t target_slot,
                                uint16_t rb_size,
                                vnf_tc_direction_t direction,
                                int32_t* out_advance_us,
                                int32_t* out_slot_offset)
{
    if (!tc || !out_advance_us || !out_slot_offset) return -1;
    
    pthread_mutex_lock(&tc->lock);
    
    /* Select profile based on direction */
    vnf_tc_direction_profile_t* profile = (direction == VNF_TC_DIR_DL) ? 
                                          &tc->dl_profile : &tc->ul_profile;
    
    /* Calculate dynamic advance time */
    int32_t base_delay = tc->baseline_owd;
    if (base_delay < 0) base_delay = 0;  /* Sanity check */
    
    /* RB-size dependent processing time */
    int32_t rb_processing = (int32_t)(profile->rb_size_coeff * (float)rb_size);
    
    /* Jitter headroom */
    int32_t jitter_headroom = (int32_t)(profile->jitter_coeff * tc->jitter_ema_us);
    
    /* Total required advance */
    int32_t required_advance = base_delay + rb_processing + jitter_headroom + 
                               profile->base_margin_us + VNF_TC_SAFETY_GUARD_US;
    
    /* Clamp to valid range */
    if (required_advance < VNF_TC_MIN_ADVANCE_US) {
        required_advance = VNF_TC_MIN_ADVANCE_US;
    }
    if (required_advance > VNF_TC_MAX_ADVANCE_US) {
        required_advance = VNF_TC_MAX_ADVANCE_US;
    }
    
    /* Calculate slot offset for cross-slot boundary handling */
    int32_t slot_offset = 0;
    if (required_advance > tc->slot_duration_us) {
        /* Need to send in earlier slot(s) */
        slot_offset = (required_advance + tc->slot_duration_us - 1) / tc->slot_duration_us;
        /* Adjust advance to be relative to the earlier slot */
        required_advance = required_advance - (slot_offset * tc->slot_duration_us);
        if (required_advance < 0) required_advance = 0;
    }
    
    *out_advance_us = required_advance;
    *out_slot_offset = slot_offset;
    
    /* Update stats */
    tc->stats.last_calculated_advance_us = required_advance;
    tc->stats.last_slot_offset = slot_offset;
    
    NFAPI_TRACE(NFAPI_TRACE_DEBUG,
        "[VNF_TC] TX Advance: target=%d.%d dir=%s rb=%d base_owd=%d rb_proc=%d "
        "jitter_hdrm=%d total=%d slot_off=%d\n",
        target_sfn, target_slot, (direction == VNF_TC_DIR_DL) ? "DL" : "UL",
        rb_size, base_delay, rb_processing, jitter_headroom,
        *out_advance_us, *out_slot_offset);
    
    pthread_mutex_unlock(&tc->lock);
    
    return 0;
}

/*===========================================================================
 * vnf_tc_get_adjustments - Get and clear pending adjustments
 *===========================================================================*/
void vnf_tc_get_adjustments(vnf_timing_controller_t* tc,
                            int32_t* out_us_adj,
                            int32_t* out_slot_adj)
{
    if (!tc || !out_us_adj || !out_slot_adj) return;
    
    pthread_mutex_lock(&tc->lock);
    
    *out_us_adj = tc->us_adjustment;
    *out_slot_adj = tc->slot_adjustment;
    
    /* Clear adjustments after reading */
    tc->us_adjustment = 0;
    tc->slot_adjustment = 0;
    
    pthread_mutex_unlock(&tc->lock);
}

/*===========================================================================
 * vnf_tc_get_stats - Get current statistics snapshot
 *===========================================================================*/
void vnf_tc_get_stats(vnf_timing_controller_t* tc, vnf_tc_stats_t* out_stats)
{
    if (!tc || !out_stats) return;
    
    pthread_mutex_lock(&tc->lock);
    memcpy(out_stats, &tc->stats, sizeof(vnf_tc_stats_t));
    pthread_mutex_unlock(&tc->lock);
}

/*===========================================================================
 * vnf_tc_is_locked - Check if sync is locked
 *===========================================================================*/
int vnf_tc_is_locked(vnf_timing_controller_t* tc)
{
    if (!tc) return 0;
    return tc->sync_locked;
}

/*===========================================================================
 * vnf_tc_process_timing_info - Process TIMING_INFO feedback from PNF
 * 
 * This is the KEY function for closed-loop timing control!
 * 
 * The PNF sends TIMING_INFO messages telling us:
 *   - latest_delay: How many microseconds LATE our packets arrived
 *   - delta_slots: PNF slot - VNF slot (how far ahead PNF is)
 * 
 * CRITICAL: Prevent oscillation with one-shot adjustment + cooldown
 * 1. After applying an adjustment, wait for it to take effect
 * 2. Only adjust again if packets are still late after cooldown
 * 3. Use damping to prevent overshoot
 *===========================================================================*/
int vnf_tc_process_timing_info(vnf_timing_controller_t* tc,
                               uint16_t pnf_sfn, uint16_t pnf_slot,
                               uint16_t vnf_sfn, uint16_t vnf_slot,
                               int32_t dl_delay, int32_t tx_delay, int32_t ul_delay,
                               int32_t* out_slot_adj, int32_t* out_us_adj)
{
    if (!tc || !out_slot_adj || !out_us_adj) return -1;
    
    pthread_mutex_lock(&tc->lock);
    
    /* Store the delay values */
    tc->latest_dl_delay_us = dl_delay;
    tc->latest_tx_delay_us = tx_delay;
    tc->latest_ul_delay_us = ul_delay;
    tc->timing_info_count++;
    
    /* Calculate delta_slots (PNF ahead of VNF) */
    int32_t pnf_dec = (pnf_sfn * (1 << tc->mu) * 10) + pnf_slot;
    int32_t vnf_dec = (vnf_sfn * (1 << tc->mu) * 10) + vnf_slot;
    int32_t max_dec = 1024 * (1 << tc->mu) * 10;
    
    tc->delta_slots = pnf_dec - vnf_dec;
    if (tc->delta_slots > max_dec / 2) tc->delta_slots -= max_dec;
    if (tc->delta_slots < -max_dec / 2) tc->delta_slots += max_dec;
    
    /*=========================================================================
     * FRAME SLIP DETECTION
     * 
     * From analysis: In high load case (d500), delta_slots spiked to 20000!
     * This indicates the VNF timing thread was starved (CPU starvation) and
     * woke up to find the world had changed dramatically.
     * 
     * When delta_slots is abnormally large (> 100 or < -50), this is NOT a
     * timing offset we can correct - it's a system failure. Making adjustments
     * in this state will only make things worse.
     * 
     * SOLUTION: Detect and skip adjustment, let system naturally recover.
     *=========================================================================*/
    #define DELTA_SLOTS_MAX_SANE 100   /* Normal range: 0 to ~30 slots ahead */
    #define DELTA_SLOTS_MIN_SANE -50   /* Should never go this negative */
    
    if (tc->delta_slots > DELTA_SLOTS_MAX_SANE || tc->delta_slots < DELTA_SLOTS_MIN_SANE) {
        NFAPI_TRACE(NFAPI_TRACE_ERROR,
            "[VNF_TC] FRAME SLIP DETECTED! delta_slots=%d is outside sane range [%d, %d]. "
            "This indicates CPU starvation or timing thread blocked. "
            "SKIPPING all adjustments - system must recover naturally.\n",
            tc->delta_slots, DELTA_SLOTS_MIN_SANE, DELTA_SLOTS_MAX_SANE);
        
        /* Reset adjustment state to prevent stale adjustments */
        tc->adjustment_settle_count = 20;  /* Extended cooldown after frame slip */
        *out_slot_adj = 0;
        *out_us_adj = 0;
        pthread_mutex_unlock(&tc->lock);
        return -1;  /* Return error to indicate abnormal state */
    }
    
    /* Find maximum delay across all message types */
    tc->max_delay_us = dl_delay;
    if (tx_delay > tc->max_delay_us) tc->max_delay_us = tx_delay;
    if (ul_delay > tc->max_delay_us) tc->max_delay_us = ul_delay;
    
    /* Initialize outputs - no adjustment by default */
    *out_slot_adj = 0;
    *out_us_adj = 0;
    
    /* Get current time for cooldown check */
    uint32_t now_ms = get_monotonic_time_us() / 1000;
    
    /*
     * COOLDOWN CHECK: After an adjustment, wait for it to take effect
     * 
     * The TIMING_INFO from PNF reflects packets sent BEFORE our adjustment.
     * We need to wait for:
     * 1. The adjustment to be applied by timing thread
     * 2. New packets to be sent with adjusted timing
     * 3. PNF to receive and report on those new packets
     * 
     * This takes at least 50-100ms, so we use a cooldown period.
     */
    if (tc->adjustment_settle_count > 0) {
        tc->adjustment_settle_count--;
        NFAPI_TRACE(NFAPI_TRACE_DEBUG,
            "[VNF_TC] TIMING_INFO: Cooldown %u remaining, ignoring (delay=%d, delta=%d)\n",
            tc->adjustment_settle_count, tc->max_delay_us, tc->delta_slots);
        pthread_mutex_unlock(&tc->lock);
        return 0;
    }
    
    /* Check time-based cooldown */
    uint32_t time_since_adj = now_ms - tc->last_adjustment_time;
    if (time_since_adj < tc->adjustment_cooldown_ms && tc->last_adjustment_time > 0) {
        NFAPI_TRACE(NFAPI_TRACE_DEBUG,
            "[VNF_TC] TIMING_INFO: Time cooldown %ums remaining\n",
            tc->adjustment_cooldown_ms - time_since_adj);
        pthread_mutex_unlock(&tc->lock);
        return 0;
    }
    
    /*=========================================================================
     * CONGESTION CHECK: Do NOT adjust timing during network congestion!
     * 
     * From analysis: During iperf test, RTT spiked from 134µs to 9639µs (70x!)
     * Adjusting timing during this transient event caused timing collapse:
     *   - Delta Slots crashed from 21 to -2
     *   - Margin went negative
     *   - System never recovered
     * 
     * KEY INSIGHT: During congestion, delay is TRANSIENT, not systematic.
     * Adjusting based on transient delay makes things WORSE.
     *=========================================================================*/
    if (tc->network_congested) {
        NFAPI_TRACE(NFAPI_TRACE_WARN,
            "[VNF_TC] TIMING_INFO: NETWORK CONGESTED (RTT=%ldus >> min=%ldus) - "
            "SKIPPING adjustment despite delay=%dus. Wait for network to stabilize.\n",
            (long)tc->recent_rtt_us, (long)tc->min_rtt_us, tc->max_delay_us);
        pthread_mutex_unlock(&tc->lock);
        return 0;
    }
    
    /*
     * ADJUSTMENT DECISION
     * 
     * Only adjust if packets are significantly late.
     * Target: packets should arrive ~2 slots early for safety margin.
     */
    int32_t slot_duration = tc->slot_duration_us;
    int32_t target_margin = 2 * slot_duration;  /* Target: 2 slots early */
    
    /* Packets are late if max_delay > 0 */
    /* We consider "acceptable" if delay is negative (early) or less than half a slot */
    if (tc->max_delay_us > slot_duration / 2) {
        /*
         * Packets are late! Calculate ONE-SHOT adjustment
         * 
         * We adjust by exactly the observed lateness plus a small margin.
         * This is applied once, then we wait for cooldown.
         */
        
        /* Lateness in slots (round up) */
        int32_t late_slots = (tc->max_delay_us + slot_duration - 1) / slot_duration;
        
        /* Add 1-slot safety margin (reduced from 2 to prevent too-early warnings) */
        int32_t total_adj = late_slots + 1;
        
        /* Cap maximum single adjustment to prevent huge jumps */
        if (total_adj > 30) {
            total_adj = 30;
            NFAPI_TRACE(NFAPI_TRACE_WARN,
                "[VNF_TC] Capping adjustment to 30 slots (was %d)\n", late_slots + 2);
        }
        
        *out_slot_adj = total_adj;
        *out_us_adj = 0;  /* Keep it simple - slot adjustment only */
        
        /* Mark adjustment in progress */
        tc->last_adjustment_time = now_ms;
        tc->adjustment_settle_count = 10;  /* Wait 10 TIMING_INFO messages */
        tc->pending_adjustment = total_adj;
        
        /* Store adjustment for timing thread */
        tc->slot_adjustment = *out_slot_adj;
        tc->us_adjustment = *out_us_adj;
        
        /* Unlock to allow adjustment */
        tc->sync_locked = 0;
        tc->consecutive_converged = 0;
        
        NFAPI_TRACE(NFAPI_TRACE_WARN,
            "[VNF_TC] TIMING_INFO: Packets late by %dus (%d slots). "
            "ONE-SHOT adjustment: +%d slots. Cooldown: 10 msgs\n",
            tc->max_delay_us, late_slots, total_adj);
            
    } else if (tc->max_delay_us < -target_margin) {
        /*
         * Packets are TOO EARLY (arriving before PNF is ready)
         * 
         * CRITICAL: Do NOT make negative adjustments!
         * 
         * From analysis: When jitter spikes, TIMING_INFO can report packets as
         * "too early" due to measurement errors. Negative adjustments caused:
         *   - Delta Slots crashed from 21 to -2
         *   - VNF started sending packets AFTER PNF deadline
         *   - Complete timing collapse
         * 
         * SOLUTION: Log the condition but DO NOT adjust.
         * The system is safer with packets arriving "too early" than risk
         * a negative adjustment during a transient jitter spike.
         */
        int32_t early_us = -tc->max_delay_us;
        int32_t early_slots = early_us / slot_duration;
        
        NFAPI_TRACE(NFAPI_TRACE_INFO,
            "[VNF_TC] TIMING_INFO: Packets early by %dus (%d slots). "
            "NO ADJUSTMENT - negative adjustments disabled for stability.\n",
            early_us, early_slots);
        
        /* DO NOT adjust - just track convergence */
        tc->consecutive_converged++;
        if (tc->consecutive_converged >= 5 && !tc->sync_locked) {
            tc->sync_locked = 1;
            NFAPI_TRACE(NFAPI_TRACE_INFO,
                "[VNF_TC] TIMING CONVERGED (early packets OK)! delay=%d delta_slots=%d\n",
                tc->max_delay_us, tc->delta_slots);
        }
    } else {
        /* Packets are on time (within acceptable window) */
        if (tc->max_delay_us <= 0) {
            /* Good - packets arriving early or on time */
            tc->consecutive_converged++;
            if (tc->consecutive_converged >= 5 && !tc->sync_locked) {
                tc->sync_locked = 1;
                NFAPI_TRACE(NFAPI_TRACE_INFO,
                    "[VNF_TC] TIMING CONVERGED! delay=%d delta_slots=%d\n",
                    tc->max_delay_us, tc->delta_slots);
            }
        }
    }
    
    NFAPI_TRACE(NFAPI_TRACE_DEBUG,
        "[VNF_TC] TIMING_INFO #%u: delta_slots=%d max_delay=%dus "
        "slot_adj=%d locked=%d cooldown=%u\n",
        tc->timing_info_count, tc->delta_slots, tc->max_delay_us,
        *out_slot_adj, tc->sync_locked, tc->adjustment_settle_count);
    
    pthread_mutex_unlock(&tc->lock);
    
    return 0;
}

/*===========================================================================
 * vnf_tc_reset - Reset the timing controller state
 *===========================================================================*/
void vnf_tc_reset(vnf_timing_controller_t* tc)
{
    if (!tc) return;
    
    pthread_mutex_lock(&tc->lock);
    
    /* Clear sample buffer */
    memset(tc->samples, 0, sizeof(tc->samples));
    tc->sample_head = 0;
    tc->sample_count = 0;
    
    /* Reset filter state */
    tc->min_owd_idx = 0;
    tc->baseline_offset = 0;
    tc->baseline_owd = 0;
    tc->jitter_ema_us = 100.0f;
    
    /* Reset sync state */
    tc->sync_locked = 0;
    tc->consecutive_converged = 0;
    tc->us_adjustment = 0;
    tc->slot_adjustment = 0;
    
    /* Reset TIMING_INFO feedback tracking */
    tc->latest_dl_delay_us = 0;
    tc->latest_tx_delay_us = 0;
    tc->latest_ul_delay_us = 0;
    tc->max_delay_us = 0;
    tc->delta_slots = 0;
    tc->timing_info_count = 0;
    
    /* Reset adjustment control */
    tc->last_adjustment_time = 0;
    tc->pending_adjustment = 0;
    tc->adjustment_settle_count = 0;
    
    /* Clear stats */
    memset(&tc->stats, 0, sizeof(tc->stats));
    
    pthread_mutex_unlock(&tc->lock);
    
    NFAPI_TRACE(NFAPI_TRACE_INFO, "[VNF_TC] Reset complete\n");
}

/*===========================================================================
 * vnf_tc_enable_csv_logging - Enable CSV logging to file
 *===========================================================================*/
int vnf_tc_enable_csv_logging(vnf_timing_controller_t* tc, const char* filepath)
{
    if (!tc || !filepath) return -1;
    
    pthread_mutex_lock(&tc->lock);
    
    if (tc->csv_logging_enabled && tc->csv_log_fd >= 0) {
        close(tc->csv_log_fd);
    }
    
    tc->csv_log_fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tc->csv_log_fd < 0) {
        NFAPI_TRACE(NFAPI_TRACE_ERROR, "[VNF_TC] Failed to open CSV log: %s\n", filepath);
        pthread_mutex_unlock(&tc->lock);
        return -1;
    }
    
    /* Write CSV header */
    const char* header = "timestamp_us,sfn,slot,rb_size,direction,owd_fwd,owd_rev,owd_min,"
                         "offset_raw,baseline_offset,baseline_owd,jitter_est,"
                         "calculated_advance,slot_offset,measured_margin,sync_locked\n";
    write(tc->csv_log_fd, header, strlen(header));
    
    tc->csv_logging_enabled = 1;
    
    NFAPI_TRACE(NFAPI_TRACE_INFO, "[VNF_TC] CSV logging enabled: %s\n", filepath);
    
    pthread_mutex_unlock(&tc->lock);
    
    return 0;
}

/*===========================================================================
 * vnf_tc_disable_csv_logging - Disable CSV logging
 *===========================================================================*/
void vnf_tc_disable_csv_logging(vnf_timing_controller_t* tc)
{
    if (!tc) return;
    
    pthread_mutex_lock(&tc->lock);
    
    if (tc->csv_logging_enabled && tc->csv_log_fd >= 0) {
        close(tc->csv_log_fd);
        tc->csv_log_fd = -1;
    }
    
    tc->csv_logging_enabled = 0;
    
    pthread_mutex_unlock(&tc->lock);
}

/*===========================================================================
 * vnf_tc_log_csv - Log timing data in CSV format
 * 
 * Format: timestamp_us,sfn,slot,rb_size,direction,owd_fwd,owd_rev,owd_min,
 *         offset_raw,baseline_offset,baseline_owd,jitter_est,
 *         calculated_advance,slot_offset,measured_margin,sync_locked
 *===========================================================================*/
void vnf_tc_log_csv(vnf_timing_controller_t* tc,
                    uint16_t sfn, uint16_t slot,
                    uint16_t rb_size,
                    vnf_tc_direction_t direction,
                    int32_t measured_margin)
{
    if (!tc || !tc->csv_logging_enabled || tc->csv_log_fd < 0) return;
    
    pthread_mutex_lock(&tc->lock);
    
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "%u,%u,%u,%u,%s,%d,%d,%d,%d,%d,%d,%.1f,%d,%d,%d,%d\n",
        get_monotonic_time_us(),
        sfn, slot, rb_size,
        (direction == VNF_TC_DIR_DL) ? "DL" : "UL",
        tc->stats.owd_min,  /* Use stats since we may not have current sample */
        tc->stats.owd_max,
        tc->baseline_owd,
        tc->stats.offset_raw,
        tc->baseline_offset,
        tc->baseline_owd,
        tc->jitter_ema_us,
        tc->stats.last_calculated_advance_us,
        tc->stats.last_slot_offset,
        measured_margin,
        tc->sync_locked);
    
    if (len > 0 && len < (int)sizeof(buf)) {
        write(tc->csv_log_fd, buf, len);
    }
    
    pthread_mutex_unlock(&tc->lock);
}

/*===========================================================================
 * vnf_tc_allocate - Allocate and initialize a timing controller
 *===========================================================================*/
vnf_timing_controller_t* vnf_tc_allocate(int mu)
{
    vnf_timing_controller_t* tc = (vnf_timing_controller_t*)calloc(1, sizeof(vnf_timing_controller_t));
    if (tc) {
        vnf_tc_init(tc, mu);
    }
    return tc;
}

/*===========================================================================
 * vnf_tc_free - Free a timing controller
 *===========================================================================*/
void vnf_tc_free(vnf_timing_controller_t* tc)
{
    if (tc) {
        vnf_tc_destroy(tc);
        free(tc);
    }
}
