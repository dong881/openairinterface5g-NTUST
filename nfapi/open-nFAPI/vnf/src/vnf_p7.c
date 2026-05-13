/*
 * Copyright 2017 Cisco Systems, Inc.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <time.h>

#include <stdio.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>
#ifdef ENABLE_AERIAL
#include "nfapi/oai_integration/aerial/fapi_nvIPC.h"
#endif
#include "vnf_p7.h"
#include "nfapi_vnf.h"
#ifdef ENABLE_WLS
#include <wls_integration/include/wls_vnf.h>
#endif
#include "nr_fapi_p7_utils.h"

extern void log_mmap_entry(const char *log_name, uint64_t value);

#ifdef NDEBUG
#  warning assert is disabled
#endif

#define SYNC_CYCLE_COUNT 2

static inline int64_t timehr_diff_us(uint32_t time_hr_a, uint32_t time_hr_b);

/* ============================================================================
 * DYNAMIC SLOT SLEEP TIMING CONTROL
 * ============================================================================ */

/* Helper function to calculate packet slot from timing value */
static inline uint32_t calc_packet_slot(int32_t timing_us, uint32_t current_slot_dec, 
                                         uint32_t slot_duration_us, uint32_t max_slot_dec)
{
  // Calculate slot offset based on timing value (in microseconds)
  // Positive timing = late, negative timing = early
  int32_t slot_offset = timing_us / (int32_t)slot_duration_us;
  return (current_slot_dec - slot_offset - (timing_us > 0) + max_slot_dec) % SLOT_ARRAY_SIZE;
}

int vnf_p7_extract_timing_info(const nfapi_nr_timing_info_t *ind,
                               nfapi_vnf_p7_connection_info_t *p7_info,
                               vnf_timing_stats_t *out_stats,
                               int max_stats)
{
    if (ind == NULL || p7_info == NULL || out_stats == NULL) {
        return 0;
    }

    if (max_stats <= 0) {
        return 0;
    }

    /*
     * slot_duration_us:
     *   mu=0 -> 1000us
     *   mu=1 -> 500us
     *   mu=2 -> 250us
     *
     * Guard against invalid mu producing zero.
     */
    int32_t slot_duration_us = 1000 >> p7_info->mu;

    if (slot_duration_us <= 0) {
        return 0;
    }

    nfapi_vnf_config_t *config = get_config();

    if (config == NULL) {
        return 0;
    }

    /*
     * Build a dynamic validity window from runtime configuration.
     *
     * Do not use hardcoded microsecond thresholds here.
     *
     * The timing info should normally be within timing_window plus
     * several slot durations.  We allow one frame duration as the
     * maximum dynamic span so that valid large delay/jitter experiments
     * are not accidentally discarded.
     */
    int32_t slots_per_frame = 10 << p7_info->mu;
    int64_t frame_duration_us =
            (int64_t)slots_per_frame * (int64_t)slot_duration_us;

    int64_t timing_window_us = (int64_t)config->timing_window;

    if (timing_window_us < 0) {
        timing_window_us = 0;
    }

    int64_t valid_span_us =
            timing_window_us + frame_duration_us;

    if (valid_span_us <= 0) {
        valid_span_us = frame_duration_us;
    }

    /*
     * Latest delay values:
     *
     * These are the most important values for no-drop policy.
     * A positive latest_delay means the message was late.
     * A negative latest_delay means the message arrived before deadline.
     */
    int32_t latest_delay_values[4] = {
        ind->dl_tti_latest_delay,
        ind->tx_data_latest_delay,
        ind->ul_tti_latest_delay,
        ind->ul_dci_latest_delay
    };

    /*
     * Earliest arrival values:
     *
     * These are useful to know how early messages are arriving.
     * They should not override a positive latest_delay.
     */
    int32_t earliest_arrival_values[4] = {
        ind->dl_tti_earliest_arrival,
        ind->tx_data_earliest_arrival,
        ind->ul_tti_earliest_arrival,
        ind->ul_dci_earliest_arrival
    };

    int32_t worst_late = INT32_MIN;
    int32_t worst_early = INT32_MAX;

    bool have_latest_delay = false;
    bool have_any_sample = false;

    /*
     * First pass:
     *   use latest_delay fields as primary control input.
     *
     * This avoids an early-arrival value masking a real late sample.
     */
    for (int i = 0; i < 4; ++i) {
        int32_t value = latest_delay_values[i];

        /*
         * In current nFAPI timing_info usage, zero is treated as
         * "not reported".  If the PNF implementation later defines
         * zero as an explicit exact-deadline sample, this condition
         * should be revisited.
         */
        if (value == 0) {
            continue;
        }

        if ((int64_t)value > valid_span_us ||
            (int64_t)value < -valid_span_us) {
            continue;
        }

        have_latest_delay = true;
        have_any_sample = true;

        if (value > worst_late) {
            worst_late = value;
        }

        if (value < worst_early) {
            worst_early = value;
        }
    }

    /*
     * Second pass:
     *   collect earliest_arrival for diagnostics / fallback.
     *
     * If there were no latest_delay samples at all, the closest
     * earliest_arrival becomes worst_late.  This keeps the controller
     * informed that packets are early, without inventing late pressure.
     */
    int32_t closest_early_to_deadline = INT32_MIN;

    for (int i = 0; i < 4; ++i) {
        int32_t value = earliest_arrival_values[i];

        if (value == 0) {
            continue;
        }

        if ((int64_t)value > valid_span_us ||
            (int64_t)value < -valid_span_us) {
            continue;
        }

        have_any_sample = true;

        if (value < worst_early) {
            worst_early = value;
        }

        /*
         * For early samples, the largest value is closest to deadline.
         * Example:
         *   -100us is closer / riskier than -900us.
         */
        if (value > closest_early_to_deadline) {
            closest_early_to_deadline = value;
        }
    }

    if (!have_any_sample) {
        return 0;
    }

    if (!have_latest_delay) {
        if (closest_early_to_deadline == INT32_MIN) {
            return 0;
        }

        worst_late = closest_early_to_deadline;
    }

    if (worst_late == INT32_MIN) {
        return 0;
    }

    if (worst_early == INT32_MAX) {
        worst_early = worst_late;
    }

    uint32_t max_jitter = 0;

    if (ind->dl_tti_jitter > max_jitter) {
        max_jitter = ind->dl_tti_jitter;
    }

    if (ind->tx_data_jitter > max_jitter) {
        max_jitter = ind->tx_data_jitter;
    }

    if (ind->ul_tti_jitter > max_jitter) {
        max_jitter = ind->ul_tti_jitter;
    }

    if (ind->ul_dci_jitter > max_jitter) {
        max_jitter = ind->ul_dci_jitter;
    }

    /*
     * One indication produces exactly one merged timing stat.
     *
     * packet_slot is intentionally set to current slot modulo local
     * history size only for compatibility/logging.  The controller
     * should not use packet_slot for decision making.
     */
    out_stats[0].packet_slot =
            NFAPI_SFNSLOT2DEC(p7_info->mu,
                               p7_info->sfn,
                               p7_info->slot) %
            SLOT_ARRAY_SIZE;

    out_stats[0].worst_late = worst_late;
    out_stats[0].worst_early = worst_early;
    out_stats[0].pnf_reported_jitter = max_jitter;

    return 1;
}

/*
 * =========================================================================================
 * VNF P7 Convergence Optimization & Adaptive Jitter Buffer Control
 * =========================================================================================
 * 
 * REFERENCES & ALGORITHMIC FOUNDATIONS:
 * 1. WebRTC NetEQ / Adaptive Jitter Buffer Strategy:
 *    - Logic: Aggressively expand the buffer (increase s_ahead_env) upon delay spikes or 
 *      deadline violations to prevent packet drops (Fast Attack). Shrink the buffer extremely 
 *      slowly ("Slow Decay") during stable periods to minimize latency without risking underruns.
 * 2. TCP RTO Jacobson/Karels Algorithm (RFC 6298):
 *    - Logic: Averages are insufficient for delay predictions in bursty networks. Reaction
 *      must be anchored to the estimated mean and the variance of measured extremes (worst_late).
 * 3. BBR-style Probing (Bottleneck Bandwidth and RTT):
 *    - Logic: Continuous probing of the lower delay bounds. When the network is quiet and operates 
 *      within an absolute safe boundary, the pacing smoothly drifts toward the minimum slot-ahead.
 *
 * CURRENT IMPLEMENTATION CHALLENGES & SOLUTIONS:
 * [Challenge 1: Catastrophic Jitter & Deadline Violations]
 *   - Solution: Fast Attack Panic. Instead of slow incremental adjustments, we instantly jump 
 *     `target_s_ahead` by at least 2 slots (or strictly calculated based on the `worst_late` 
 *     severity) when hitting statistical anomalies or strict deadline violations.
 * [Challenge 2: Oscillation & State Bouncing]
 *   - Solution: Phase Direction & Penalty Dampening. We use `last_phase_delta_us` tracking 
 *     (comparing previous absolute `total_advanced_us` adjustments) to verify if a phase shift 
 *     is helpful. A harsh penalty counter (`reduction_penalty_counter`) and a 30-second time-lock 
 *     (`last_increase_timestamp_hr`) are applied after panic expansions to prevent rapid bounce-backs.
 * [Challenge 3: Maximizing Low Latency in Safe Zones]
 *   - Solution: Proactive Latency Reduction (Safe Decay). We monitor `absolute_safe_boundary` 
 *     (ensuring at least a 1-slot safety margin). Once `consecutive_late_spikes` safely meets the 
 *     leaky bucket penalty threshold, and the 30s lock has expired, we safely drop `s_ahead_env`.
 * =========================================================================================
 */

static int32_t global_max_s_ahead = 14;
static int32_t global_raw_worst_late_control = 0;
static int32_t global_ewma_alpha_denom = 8;    // 1/8 default
static int32_t global_ewma_beta_denom = 4;     // 1/4 default

__attribute__((constructor)) static void initialize_max_s_ahead(void) {
    char *env_val = getenv("MAX_S_AHEAD");
    if (env_val != NULL) {
        global_max_s_ahead = atoi(env_val);
    }

	char *raw_ctrl = getenv("RAW_WORST_LATE_CONTROL");
	if (raw_ctrl != NULL) {
		global_raw_worst_late_control = atoi(raw_ctrl) != 0;
	}
	char *alpha_denom = getenv("EWMA_ALPHA");
	if (alpha_denom != NULL) {
		int val = atoi(alpha_denom);
		if (val > 0 && val <= 256) {
			global_ewma_alpha_denom = val;
		}
	}

	char *beta_denom = getenv("EWMA_BETA");
	if (beta_denom != NULL) {
		int val = atoi(beta_denom);
		if (val > 0 && val <= 256) {
			global_ewma_beta_denom = val;
		}
	}
}

/*
 * Calculate the number of slots between two (SFN, slot) pairs.
 * Accounts for SFN wrap-around (SFN 0-1023).
 * Result: positive if (current_sfn, current_slot) > (prev_sfn, prev_slot)
 */
static inline int32_t calculate_slot_distance(int32_t current_sfn, int32_t current_slot,
                                               int32_t prev_sfn, int32_t prev_slot,
                                               int32_t slots_per_frame)
{
	// Convert to absolute slot numbers within a frame boundary
	int32_t current_absolute = current_sfn * slots_per_frame + current_slot;
	int32_t prev_absolute = prev_sfn * slots_per_frame + prev_slot;
	
	// Handle wrap-around: if current < prev, add one full hyperframe cycle
	if (current_absolute < prev_absolute) {
		current_absolute += 1024 * slots_per_frame;  // 1024 SFNs per hyperframe
	}
	
	return current_absolute - prev_absolute;
}

static int32_t ceil_div_pos_i32(int32_t num, int32_t den)
{
    if (den <= 0)
        return 0;

    if (num <= 0)
        return 0;

    return (num + den - 1) / den;
}

static int32_t abs_i32(int32_t v)
{
    return v < 0 ? -v : v;
}
static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo)
        return lo;

    if (v > hi)
        return hi;

    return v;
}
static inline int32_t p7_max_i32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

/*
 * Integer EWMA helper.
 *
 * Avoids integer EWMA dead-zone:
 *
 *   cur += (target - cur) / denom
 *
 * would otherwise stop changing when abs(target - cur) < denom.
 *
 * This is not a policy hyperparameter.
 */
static inline int32_t p7_ewma_step_i32(
    int32_t cur,
    int32_t target,
    int32_t denom)
{
    int32_t diff;
    int32_t step;

    if (denom <= 1)
        return target;

    diff = target - cur;

    if (diff == 0)
        return cur;

    step = diff / denom;

    if (step == 0)
        step = diff > 0 ? 1 : -1;

    return cur + step;
}

/*
 * EWMA integer zero-resolution.
 *
 * Not a tunable threshold.
 * It is derived from integer EWMA alpha denominator.
 */
static inline int p7_ewma_effectively_zero_i32(
    int32_t value,
    int32_t denom)
{
    if (value <= 0)
        return 1;

    if (denom <= 1)
        return value == 0;

    return value <= denom;
}

static void p7_run_ewma_lab_control(
    nfapi_vnf_p7_connection_info_t *p7_info,
    const vnf_timing_stats_t *stats,
    int32_t slot_duration_us,
    int32_t max_s_ahead)
{
    nfapi_vnf_config_t *config = get_config();

    if (p7_info == NULL || stats == NULL || config == NULL)
        return;

    if (slot_duration_us <= 0 || max_s_ahead <= 0)
        return;

    /*
     * ============================================================
     * Control pacing
     * ============================================================
     *
     * No extra hyperparameter.
     *
     * Pacing is derived from:
     *   - previous movement magnitude
     *   - NFAPI timing_info_period
     */
    int32_t elapsed_slots = calculate_slot_distance(
            p7_info->sfn,
            p7_info->slot,
            p7_info->last_adjustment_sfn,
            p7_info->last_adjustment_slot,
            10 << p7_info->mu);

    int32_t required_wait_slots =
            p7_info->last_adjustment_steps +
            (int32_t)config->timing_info_period;

    if (required_wait_slots < 1)
        required_wait_slots = 1;

    if (elapsed_slots < required_wait_slots)
        return;

    /*
     * ============================================================
     * Single-input EWMA estimator
     * ============================================================
     *
     * The only runtime input:
     *
     *     stats->worst_late
     *
     * worst_late > 0:
     *     at least one P7 message was already late.
     *
     * worst_late <= 0:
     *     worst observed P7 message was still early by -worst_late us.
     */
    if (p7_info->estimated_mean_late == 0) {
        p7_info->estimated_mean_late = stats->worst_late;
        p7_info->estimated_jitter_var = abs_i32(stats->worst_late) / 2;

        p7_info->last_adjustment_sfn = p7_info->sfn;
        p7_info->last_adjustment_slot = p7_info->slot;
    }

    int32_t old_mean = p7_info->estimated_mean_late;
    int32_t diff = stats->worst_late - old_mean;
    int32_t abs_diff = abs_i32(diff);

    /*
     * EWMA mean.
     */
    p7_info->estimated_mean_late +=
            diff / global_ewma_alpha_denom;

    /*
     * Directional jitter EWMA.
     */
    if (diff > 0) {
        p7_info->late_jitter +=
                (diff - p7_info->late_jitter) /
                global_ewma_beta_denom;
    } else {
        p7_info->early_jitter +=
                ((-diff) - p7_info->early_jitter) /
                global_ewma_beta_denom;
    }

    /*
     * Absolute jitter EWMA.
     */
    p7_info->estimated_jitter_var +=
            (abs_diff - p7_info->estimated_jitter_var) /
            global_ewma_beta_denom;

    if (p7_info->estimated_jitter_var < 0)
        p7_info->estimated_jitter_var = 0;

    if (p7_info->late_jitter < 0)
        p7_info->late_jitter = 0;

    if (p7_info->early_jitter < 0)
        p7_info->early_jitter = 0;

    /*
     * The most dangerous timing representative.
     *
     * Use the later one between:
     *   - current worst_late
     *   - EWMA mean
     */
    int32_t closest_to_deadline_us =
            stats->worst_late > p7_info->estimated_mean_late ?
            stats->worst_late :
            p7_info->estimated_mean_late;

    /*
     * Late-side uncertainty:
     *   Used for UP risk.
     *
     * Early-side uncertainty:
     *   Used as DOWN guard, because high early jitter means timing
     *   is unstable and should not aggressively reduce s_ahead.
     */
    int32_t late_side_uncertainty_us =
            p7_info->late_jitter +
            p7_info->estimated_jitter_var;

    int32_t early_side_uncertainty_us =
            p7_info->early_jitter +
            p7_info->estimated_jitter_var;

    if (late_side_uncertainty_us < 0)
        late_side_uncertainty_us = 0;

    if (early_side_uncertainty_us < 0)
        early_side_uncertainty_us = 0;

    int32_t timing_uncertainty_us =
            late_side_uncertainty_us;

    /*
     * Adaptive jitter guard.
     *
     * No new hyperparameter:
     * guard is derived from EWMA late/early uncertainty.
     */
    int32_t adaptive_jitter_guard_us =
            p7_max_i32(late_side_uncertainty_us,
                       early_side_uncertainty_us);

    /*
     * ============================================================
     * Risk model
     * ============================================================
     *
     * predicted_risk_us > 0 means current boundary estimate plus
     * late-side uncertainty crosses the deadline.
     */
    int32_t predicted_risk_us =
            closest_to_deadline_us +
            timing_uncertainty_us;

    if (predicted_risk_us < 0)
        predicted_risk_us = 0;

    bool hard_late =
            stats->worst_late > 0 ||
            closest_to_deadline_us > 0;

    int32_t failure_sample_us = 0;

    if (stats->worst_late > 0)
        failure_sample_us = stats->worst_late;
    else if (closest_to_deadline_us > 0)
        failure_sample_us = closest_to_deadline_us;

    if (failure_sample_us < 0)
        failure_sample_us = 0;

    /*
     * Safe margin after considering late-side uncertainty.
     */
    int32_t safe_margin_sample_us =
            -(closest_to_deadline_us + timing_uncertainty_us);

    if (safe_margin_sample_us < 0)
        safe_margin_sample_us = 0;

    /*
     * ============================================================
     * EWMA debts / safe margin
     * ============================================================
     */
    p7_info->ewma_lab_failure_debt_us =
            p7_ewma_step_i32(
                    p7_info->ewma_lab_failure_debt_us,
                    failure_sample_us,
                    global_ewma_alpha_denom);

    p7_info->ewma_lab_risk_debt_us =
            p7_ewma_step_i32(
                    p7_info->ewma_lab_risk_debt_us,
                    predicted_risk_us,
                    global_ewma_alpha_denom);

    p7_info->ewma_lab_safe_margin_ewma_us =
            p7_ewma_step_i32(
                    p7_info->ewma_lab_safe_margin_ewma_us,
                    safe_margin_sample_us,
                    global_ewma_alpha_denom);

    if (p7_info->ewma_lab_failure_debt_us < 0)
        p7_info->ewma_lab_failure_debt_us = 0;

    if (p7_info->ewma_lab_risk_debt_us < 0)
        p7_info->ewma_lab_risk_debt_us = 0;

    if (p7_info->ewma_lab_safe_margin_ewma_us < 0)
        p7_info->ewma_lab_safe_margin_ewma_us = 0;

    bool failure_debt_free =
            p7_ewma_effectively_zero_i32(
                    p7_info->ewma_lab_failure_debt_us,
                    global_ewma_alpha_denom);

    bool risk_debt_free =
            p7_ewma_effectively_zero_i32(
                    p7_info->ewma_lab_risk_debt_us,
                    global_ewma_alpha_denom);

    bool soft_risk =
            !hard_late &&
            predicted_risk_us > 0;

    bool safe_sample =
            !hard_late &&
            predicted_risk_us == 0 &&
            safe_margin_sample_us > 0;

    /*
     * ============================================================
     * Fresh evidence counters
     * ============================================================
     *
     * These counters are derived only from worst_late classification.
     *
     * Important:
     *   After any actuation, these counters are reset.
     *   Therefore every later UP/DOWN decision must use fresh evidence
     *   collected at the new s_ahead.
     */
    int32_t pre_safe_count =
            p7_info->ewma_lab_safe_period_count;

    int32_t pre_late_count =
            p7_info->ewma_lab_late_period_count;

    int32_t pre_risk_count =
            p7_info->ewma_lab_risk_period_count;

    if (hard_late) {
        p7_info->ewma_lab_late_period_count++;
        p7_info->ewma_lab_safe_period_count = 0;
        p7_info->ewma_lab_risk_period_count = 0;
    } else if (soft_risk) {
        p7_info->ewma_lab_risk_period_count++;
        p7_info->ewma_lab_safe_period_count = 0;
        p7_info->ewma_lab_late_period_count = 0;
    } else if (safe_sample) {
        p7_info->ewma_lab_safe_period_count++;
        p7_info->ewma_lab_late_period_count = 0;
        p7_info->ewma_lab_risk_period_count = 0;
    } else {
        p7_info->ewma_lab_safe_period_count = 0;
        p7_info->ewma_lab_late_period_count = 0;
        p7_info->ewma_lab_risk_period_count = 0;
    }

    /*
     * Fresh evidence requirements.
     *
     * Not new hyperparameters:
     *   - DOWN evidence window comes from alpha denominator.
     *   - soft UP evidence window comes from beta denominator.
     */
    bool enough_fresh_safe_evidence_for_down =
            p7_info->ewma_lab_safe_period_count >=
            global_ewma_alpha_denom;

    bool enough_fresh_risk_evidence_for_soft_up =
            p7_info->ewma_lab_risk_period_count >=
            global_ewma_beta_denom;

    /*
     * ============================================================
     * Decision model
     * ============================================================
     *
     * Hard UP:
     *   Immediate, because actual lateness was observed.
     *
     * Soft UP:
     *   Requires fresh accumulated risk evidence to avoid one-sample
     *   jitter triggering 5<->6 / 6<->7 flapping.
     *
     * DOWN:
     *   Requires fresh accumulated safe evidence to avoid cascade down.
     */
    bool hard_up_required =
            hard_late;

    bool soft_up_required =
            !hard_late &&
            predicted_risk_us > 0 &&
            !risk_debt_free &&
            enough_fresh_risk_evidence_for_soft_up;

    bool up_required =
            hard_up_required ||
            soft_up_required;

    /*
     * Risk if we remove one slot.
     */
    int32_t post_down_predicted_risk_us =
            closest_to_deadline_us +
            slot_duration_us +
            timing_uncertainty_us;

    bool down_safe_after_one_slot =
            post_down_predicted_risk_us <= 0;

    /*
     * DOWN must leave one adaptive jitter guard after removing one slot.
     */
    bool enough_ewma_safe_margin_for_down =
            p7_info->ewma_lab_safe_margin_ewma_us >=
            slot_duration_us + adaptive_jitter_guard_us;

    bool post_down_guarded_safe =
            post_down_predicted_risk_us +
            adaptive_jitter_guard_us <= 0;

    bool debt_free =
            failure_sample_us == 0 &&
            predicted_risk_us == 0 &&
            failure_debt_free &&
            risk_debt_free;

    bool down_allowed =
            !up_required &&
            debt_free &&
            enough_fresh_safe_evidence_for_down &&
            enough_ewma_safe_margin_for_down &&
            down_safe_after_one_slot &&
            post_down_guarded_safe &&
            s_ahead_env > 1;

    int32_t target_s_ahead = s_ahead_env;
    int32_t up_reason = 0;
    int32_t down_reason = 0;

    if (hard_up_required) {
        /*
         * Hard UP may jump multiple slots because deadline was already
         * crossed.
         */
        int32_t pressure_for_up_us =
                predicted_risk_us +
                failure_sample_us;

        if (pressure_for_up_us < 1)
            pressure_for_up_us = 1;

        int32_t extra_slots =
                ceil_div_pos_i32(
                        pressure_for_up_us,
                        slot_duration_us);

        if (extra_slots < 1)
            extra_slots = 1;

        target_s_ahead =
                s_ahead_env + extra_slots;

        if (target_s_ahead > max_s_ahead)
            target_s_ahead = max_s_ahead;

        up_reason = 1;
    } else if (soft_up_required) {
        /*
         * Soft UP is predictive, not actual failure.
         * Move one slot only.
         */
        target_s_ahead = s_ahead_env + 1;

        if (target_s_ahead > max_s_ahead)
            target_s_ahead = max_s_ahead;

        up_reason = 2;
    } else if (down_allowed) {
        /*
         * DOWN is always one slot.
         */
        target_s_ahead = s_ahead_env - 1;

        if (target_s_ahead < 1)
            target_s_ahead = 1;

        down_reason = 1;
    }

    /*
     * ============================================================
     * No movement path
     * ============================================================
     */
    if (target_s_ahead == s_ahead_env) {
        if (p7_info->sfn % 256 == 0 && p7_info->slot == 0) {
            NFAPI_TRACE(NFAPI_TRACE_INFO,
                "[P7_SYNC][EWMA_LAB] stable s_ahead=%d "
                "worst_late=%d mean=%d var=%d diff=%d "
                "late_jitter=%d early_jitter=%d "
                "late_uncertainty=%d early_uncertainty=%d jitter_guard=%d "
                "closest=%d predicted_risk=%d failure_sample=%d "
                "failure_debt=%d risk_debt=%d "
                "failure_debt_free=%d risk_debt_free=%d "
                "safe_margin=%d safe_margin_ewma=%d "
                "hard_late=%d soft_risk=%d safe_sample=%d "
                "safe_cnt=%d late_cnt=%d risk_cnt=%d "
                "fresh_safe_down=%d fresh_risk_up=%d "
                "hard_up=%d soft_up=%d "
                "post_down_risk=%d post_down_guarded_risk=%d "
                "debt_free=%d enough_margin=%d guarded_safe=%d down_allowed=%d "
                "alpha=1/%d beta=1/%d",
                s_ahead_env,
                stats->worst_late,
                p7_info->estimated_mean_late,
                p7_info->estimated_jitter_var,
                diff,
                p7_info->late_jitter,
                p7_info->early_jitter,
                late_side_uncertainty_us,
                early_side_uncertainty_us,
                adaptive_jitter_guard_us,
                closest_to_deadline_us,
                predicted_risk_us,
                failure_sample_us,
                p7_info->ewma_lab_failure_debt_us,
                p7_info->ewma_lab_risk_debt_us,
                failure_debt_free,
                risk_debt_free,
                safe_margin_sample_us,
                p7_info->ewma_lab_safe_margin_ewma_us,
                hard_late,
                soft_risk,
                safe_sample,
                p7_info->ewma_lab_safe_period_count,
                p7_info->ewma_lab_late_period_count,
                p7_info->ewma_lab_risk_period_count,
                enough_fresh_safe_evidence_for_down,
                enough_fresh_risk_evidence_for_soft_up,
                hard_up_required,
                soft_up_required,
                post_down_predicted_risk_us,
                post_down_predicted_risk_us + adaptive_jitter_guard_us,
                debt_free,
                enough_ewma_safe_margin_for_down,
                post_down_guarded_safe,
                down_allowed,
                global_ewma_alpha_denom,
                global_ewma_beta_denom);
        }

        /*
         * Compatibility reset only.
         * These fields are not decision inputs in this controller.
         */
        p7_info->recent_p7_too_late_max_us = 0;
        p7_info->recent_rlc_reject_count = 0;
        p7_info->recent_harq_timeout_count = 0;
        p7_info->recent_p7_msg_count = 0;

        return;
    }

    /*
     * ============================================================
     * Apply actuation
     * ============================================================
     */
    int32_t old_s_ahead = s_ahead_env;

    if (target_s_ahead > max_s_ahead)
        target_s_ahead = max_s_ahead;

    if (target_s_ahead < 1)
        target_s_ahead = 1;

    int32_t delta_s_ahead =
            target_s_ahead - old_s_ahead;

    if (delta_s_ahead == 0) {
        p7_info->recent_p7_too_late_max_us = 0;
        p7_info->recent_rlc_reject_count = 0;
        p7_info->recent_harq_timeout_count = 0;
        p7_info->recent_p7_msg_count = 0;
        return;
    }

    /*
     * Compensate estimator after changing s_ahead.
     *
     * Increasing s_ahead makes future messages earlier.
     * Therefore estimated_mean_late shifts down.
     */
    int64_t mean_shift_64 =
            (int64_t)delta_s_ahead * slot_duration_us;

    int64_t compensated_mean_64 =
            (int64_t)p7_info->estimated_mean_late -
            mean_shift_64;

    if (compensated_mean_64 > INT32_MAX)
        compensated_mean_64 = INT32_MAX;

    if (compensated_mean_64 < INT32_MIN)
        compensated_mean_64 = INT32_MIN;

    p7_info->estimated_mean_late =
            (int32_t)compensated_mean_64;

    /*
     * Large actuation changes operating point.
     * Reduce transient jitter memory using beta denominator.
     *
     * No new hyperparameter.
     */
    if (abs_i32(delta_s_ahead) > 1) {
        p7_info->estimated_jitter_var =
                p7_info->estimated_jitter_var /
                global_ewma_beta_denom;

        p7_info->late_jitter =
                p7_info->late_jitter /
                global_ewma_beta_denom;

        p7_info->early_jitter =
                p7_info->early_jitter /
                global_ewma_beta_denom;
    }

    /*
     * Pacing based on actual movement size.
     */
    p7_info->last_adjustment_steps =
            abs_i32(delta_s_ahead);

    if (p7_info->last_adjustment_steps < 1)
        p7_info->last_adjustment_steps = 1;

    p7_info->last_adjustment_sfn = p7_info->sfn;
    p7_info->last_adjustment_slot = p7_info->slot;

    p7_info->ewma_lab_last_target_s_ahead =
            target_s_ahead;

    p7_info->ewma_lab_last_direction =
            delta_s_ahead > 0 ? 1 : -1;

    /*
     * Critical fix:
     *
     * Evidence collected at the old s_ahead is invalid after actuation.
     * Reset fresh counters to prevent cascade down and single-sample
     * oscillation based on stale evidence.
     */
    p7_info->ewma_lab_safe_period_count = 0;
    p7_info->ewma_lab_late_period_count = 0;
    p7_info->ewma_lab_risk_period_count = 0;

    NFAPI_TRACE(NFAPI_TRACE_INFO,
        "[P7_SYNC][EWMA_LAB] α=1/%d β=1/%d %s: %d→%d | "
        "worst_late=%d mean=%d var=%d diff=%d "
        "late_jitter=%d early_jitter=%d "
        "late_uncertainty=%d early_uncertainty=%d jitter_guard=%d "
        "closest=%d predicted_risk=%d failure_sample=%d "
        "failure_debt=%d risk_debt=%d "
        "failure_debt_free=%d risk_debt_free=%d "
        "safe_margin=%d safe_margin_ewma=%d "
        "hard_late=%d soft_risk=%d safe_sample=%d "
        "pre_safe=%d pre_late=%d pre_risk=%d "
        "safe_cnt=%d late_cnt=%d risk_cnt=%d "
        "fresh_safe_down=%d fresh_risk_up=%d "
        "hard_up=%d soft_up=%d "
        "post_down_risk=%d post_down_guarded_risk=%d "
        "debt_free=%d enough_margin=%d guarded_safe=%d down_allowed=%d "
        "up_reason=%d down_reason=%d "
        "delta=%d mean_shift=%ld wait_steps=%d",
        global_ewma_alpha_denom,
        global_ewma_beta_denom,
        delta_s_ahead > 0 ? "UP" : "DOWN",
        old_s_ahead,
        target_s_ahead,
        stats->worst_late,
        p7_info->estimated_mean_late,
        p7_info->estimated_jitter_var,
        diff,
        p7_info->late_jitter,
        p7_info->early_jitter,
        late_side_uncertainty_us,
        early_side_uncertainty_us,
        adaptive_jitter_guard_us,
        closest_to_deadline_us,
        predicted_risk_us,
        failure_sample_us,
        p7_info->ewma_lab_failure_debt_us,
        p7_info->ewma_lab_risk_debt_us,
        failure_debt_free,
        risk_debt_free,
        safe_margin_sample_us,
        p7_info->ewma_lab_safe_margin_ewma_us,
        hard_late,
        soft_risk,
        safe_sample,
        pre_safe_count,
        pre_late_count,
        pre_risk_count,
        p7_info->ewma_lab_safe_period_count,
        p7_info->ewma_lab_late_period_count,
        p7_info->ewma_lab_risk_period_count,
        enough_fresh_safe_evidence_for_down,
        enough_fresh_risk_evidence_for_soft_up,
        hard_up_required,
        soft_up_required,
        post_down_predicted_risk_us,
        post_down_predicted_risk_us + adaptive_jitter_guard_us,
        debt_free,
        enough_ewma_safe_margin_for_down,
        post_down_guarded_safe,
        down_allowed,
        up_reason,
        down_reason,
        delta_s_ahead,
        (long)mean_shift_64,
        p7_info->last_adjustment_steps);

    p7_info->last_total_advanced_us =
            p7_info->total_advanced_us;

    s_ahead_env = target_s_ahead;

    /*
     * Compatibility reset only.
     * This controller does not use these as decision inputs.
     */
    p7_info->recent_p7_too_late_max_us = 0;
    p7_info->recent_rlc_reject_count = 0;
    p7_info->recent_harq_timeout_count = 0;
    p7_info->recent_p7_msg_count = 0;

    return;
}


void vnf_p7_convergence_optimization(nfapi_vnf_p7_connection_info_t *p7_info, const vnf_timing_stats_t *stats)
{
    int32_t slot_duration_us = p7_info->slot_duration_us;

    int32_t worst_late = stats->worst_late;
    // uint32_t now_hr = vnf_get_current_time_hr();
    int32_t max_s_ahead = global_max_s_ahead;

    /*
     * A/B control group mode for instability reproduction:
     * - No EWMA, no variance-based damping, no slow-decay guardrails.
     * - Follow raw worst_late directly with aggressive up/down slot moves.
     * This intentionally makes slot-ahead prone to ping-pong oscillation.
     */
    if (false) {
        int target_s_ahead = s_ahead_env;

        if (worst_late >= 0) {
            int32_t late_slots = worst_late / slot_duration_us;
            target_s_ahead += late_slots + 1;
        } else {
            int32_t early_us = -worst_late;
            int32_t early_slots = early_us / slot_duration_us;
            target_s_ahead -= early_slots + 1;
        }

        if (target_s_ahead > max_s_ahead) target_s_ahead = max_s_ahead;
        if (target_s_ahead < 1) target_s_ahead = 1;

        p7_info->estimated_mean_late = worst_late;
        p7_info->estimated_jitter_var = 0;

        if (target_s_ahead != s_ahead_env) {
            NFAPI_TRACE(NFAPI_TRACE_WARN,
                        "[P7_SYNC][CONTROL_RAW] Direct worst_late control: %d -> %d (worst_late=%d, jitter=%u)",
                        s_ahead_env, target_s_ahead, worst_late, stats->pnf_reported_jitter);
            p7_info->last_total_advanced_us = p7_info->total_advanced_us;
            s_ahead_env = target_s_ahead;
        }
        return;
    }

	if (true) {
		p7_run_ewma_lab_control(p7_info, stats, slot_duration_us, max_s_ahead);
		return;
	}

  //   // ===================================================================
  //   // 1. 統計學突波偵測 (Jacobson/Karels TCP RTT Algorithm)
  //   // ===================================================================
  //   if (p7_info->estimated_mean_late == 0) {
  //       p7_info->estimated_mean_late = worst_late;
  //       p7_info->estimated_jitter_var = 100;
  //   }
    
  //   int32_t diff = worst_late - p7_info->estimated_mean_late;
  //   int32_t abs_diff = diff < 0 ? -diff : diff;
    
  //   p7_info->estimated_mean_late = p7_info->estimated_mean_late + (diff / 8);
  //   int32_t var_diff = abs_diff - p7_info->estimated_jitter_var;
  //   p7_info->estimated_jitter_var = p7_info->estimated_jitter_var + (var_diff / 4);

  //   // ===================================================================
  //   // 2. 動態安全邊界 (Dynamic Bounds)
  //   // ===================================================================
  //   int32_t base_margin_us = (slot_duration_us * 3) / 2;
  //   int32_t dynamic_panic_threshold = (p7_info->estimated_jitter_var * 3) / 2;
  //   int32_t safe_margin_us = dynamic_panic_threshold > base_margin_us ? dynamic_panic_threshold : base_margin_us;

  //   int target_s_ahead = s_ahead_env;
  //   bool in_panic = false;

  //   int32_t anomaly_threshold_us = p7_info->estimated_jitter_var * 3;
  //   if (anomaly_threshold_us < slot_duration_us * 2) {
  //       anomaly_threshold_us = slot_duration_us * 2;
  //   }
  //   bool statistical_anomaly = (abs_diff > anomaly_threshold_us);

  //   // Dynamic Safe Boundary: If we are N slots ahead, we only drop to N-1 if we have 
  //   // consistently Arrival at least (N-1) slots early, ensuring a 1-slot safety buffer after drop.
  //   int32_t absolute_safe_boundary = (s_ahead_env - 1) * slot_duration_us;

  //   if (worst_late < -absolute_safe_boundary) {
  //       if (abs_diff < (-worst_late - slot_duration_us)) {
  //           statistical_anomaly = false;
  //       }
  //   }

  //   /* Use s_ahead_env directly as the baseline for proposed adjustments to decouple
  //    * macro-shifts from micro-advancements (pending_us). This prevents the "chain effect"
  //    * where increased sleep causes a premature downward slot-ahead drop.
  //    */
  //   int32_t current_slot_estimate = s_ahead_env;
  //   if (current_slot_estimate < 1) current_slot_estimate = 1;
  //   if (current_slot_estimate > max_s_ahead) current_slot_estimate = max_s_ahead;

  //   bool positive_timing = (worst_late >= 0);
  //   bool near_deadline_edge = (worst_late > 0 && worst_late < slot_duration_us);
  //   bool jitter_activity = (stats->pnf_reported_jitter > 20);
  //   bool significant_variation = (abs_diff > slot_duration_us / 2);
  //   bool strong_packet_activity = jitter_activity || significant_variation;
  //   bool edge_activity_panic = positive_timing && (target_s_ahead <= current_slot_estimate) && near_deadline_edge && strong_packet_activity;
  //   bool strict_deadline_violation = (worst_late >= slot_duration_us);
  //   bool jitter_panic = positive_timing && (stats->pnf_reported_jitter > (uint32_t)safe_margin_us * 2) && (worst_late > -absolute_safe_boundary * 2);
  //   if (!positive_timing) {
  //       statistical_anomaly = false;
  //   }

  //   /* Use last_total_advanced_us to detect whether the previous phase move
  //    * helped or overshot the latency direction. This avoids repeated
  //    * oscillation around the boundary between 7 and 8.
  //    */
  //   int32_t last_phase_delta_us = p7_info->total_advanced_us - p7_info->last_total_advanced_us;
  //   bool phase_direction_helpful = false;
  //   if (last_phase_delta_us > 0 && worst_late > 0) phase_direction_helpful = true;
  //   if (last_phase_delta_us < 0 && worst_late < 0) phase_direction_helpful = true;

  //   /* Determine a discrete target slot ahead when the timing window clearly
  //    * supports a step change. This should be smoother than jumping to 8 every
  //    * deadline violation, and should prefer the smallest slot shift that still
  //    * resolves the late/early error.
  //    */
  //   int32_t proposed_slot_ahead = current_slot_estimate;
  //   if (worst_late >= slot_duration_us / 2) {
  //       proposed_slot_ahead = current_slot_estimate + 1;
  //   }
  //   // DO NOT aggressively push down! Only push down safely in section 3 (Proactive Latency Reduction)
  //   if (proposed_slot_ahead < 1) proposed_slot_ahead = 1;
  //   if (proposed_slot_ahead > max_s_ahead) proposed_slot_ahead = max_s_ahead;

  //   if (proposed_slot_ahead > target_s_ahead) {
  //       // Only allow upward moves here; downward moves are handled by the leaky bucket in section 3.
  //       int32_t move = proposed_slot_ahead - target_s_ahead;
  //       if (abs(move) > 1 && !phase_direction_helpful) {
  //           target_s_ahead += (move > 0 ? 1 : -1);
  //       } else {
  //           target_s_ahead = proposed_slot_ahead;
  //       }
  //   }

  //   if (edge_activity_panic || jitter_panic || statistical_anomaly || strict_deadline_violation) {
  //       in_panic = true;

  //       bool c1 = edge_activity_panic;
  //       bool c2 = jitter_panic;
  //       bool c3 = statistical_anomaly;
  //       bool c4 = strict_deadline_violation;

  //       NFAPI_TRACE(NFAPI_TRACE_WARN,
  //           "[P7_SYNC] FAST ATTACK PANIC TRIGGERED! Reasons:%s%s%s%s | Values: worst_late=%d (limit > %d), pnf_jitter=%u (limit > %d), abs_diff=%d (limit > %d), slot_us=%d\n",
  //           c1 ? " [worst_late edge]" : "",
  //           c2 ? " [PNF Jitter]" : "",
  //           c3 ? " [Statistical Anomaly MAD]" : "",
  //           c4 ? " [deadline violation]" : "",
  //           worst_late, -safe_margin_us,
  //           stats->pnf_reported_jitter, safe_margin_us * 2,
  //           abs_diff, anomaly_threshold_us,
  //           slot_duration_us);

  //       p7_info->peak_latency_timestamp_hr = now_hr;
        
  //       // Instead of jumping blindly to max, take a measured jump based on severity
  //       // Jump +2 slots, or more if strictly necessary, but bounded to max_s_ahead.
  //       int32_t step_up = 2;
  //       if (strict_deadline_violation) {
  //           step_up = (worst_late / slot_duration_us) + 3;
  //           // Apply a harsh penalty on the reduction threshold to avoid rapid bounce-back
  //           p7_info->reduction_penalty_counter += 50000;
  //           if (p7_info->reduction_penalty_counter > 1000000) {
  //               p7_info->reduction_penalty_counter = 1000000;
  //           }
  //       }
  //       target_s_ahead += step_up;
  //       if (target_s_ahead > max_s_ahead) target_s_ahead = max_s_ahead;

  //       p7_info->last_increase_timestamp_hr = now_hr;
  //       p7_info->consecutive_late_spikes = 0;
  //   } else {
  //       // ===================================================================
  //       // 3. 安全降檔 (Proactive Latency Reduction)
  //       // ===================================================================
  //       int64_t diff_last_increase_us = timehr_diff_us(now_hr, p7_info->last_increase_timestamp_hr);
  //       bool reduction_locked = (p7_info->last_increase_timestamp_hr != 0 && diff_last_increase_us < 30000000); // 30s lock

  //       if (worst_late < -absolute_safe_boundary && !reduction_locked) {
  //           p7_info->consecutive_late_spikes++;
            
  //           // Leaky bucket decay for the penalty when operating safely
  //           if (p7_info->reduction_penalty_counter > 0) {
  //               p7_info->reduction_penalty_counter--;
  //           }
            
  //           int32_t current_threshold = 2000 + p7_info->reduction_penalty_counter;

  //           if (p7_info->consecutive_late_spikes > current_threshold) {
  //               int32_t step_down_target = target_s_ahead - 1;
  //               int32_t top_zone_threshold = 8;

  //               if (s_ahead_env == top_zone_threshold && step_down_target == top_zone_threshold - 1) {
  //                   if (p7_info->stable_top_pending_drop != step_down_target) {
  //                       p7_info->stable_top_pending_drop = step_down_target;
  //                       NFAPI_TRACE(NFAPI_TRACE_INFO,
  //                           "[P7_SYNC] TOP HOLD: keeping %d, pending lower target %d until next safe reduction",
  //                           s_ahead_env, step_down_target);
  //                   } else {
  //                       target_s_ahead -= 2;
  //                       p7_info->stable_top_pending_drop = 0;
  //                   }
  //               } else {
  //                   target_s_ahead -= 1;
  //                   p7_info->stable_top_pending_drop = 0;
  //               }

  //               p7_info->consecutive_late_spikes = 0;
  //           }
  //       } else {
  //           if (reduction_locked && worst_late < -absolute_safe_boundary && (p7_info->sfn % 1024 == 0)) {
  //                NFAPI_TRACE(NFAPI_TRACE_DEBUG, "[P7_SYNC] Reduction Locked: %d s ahead (time since last increase: %ld ms)\n", 
  //                            s_ahead_env, diff_last_increase_us / 1000);
  //           }
  //           p7_info->consecutive_late_spikes = 0;
  //           p7_info->stable_top_pending_drop = 0;
  //       }
  //   }

  //   if (target_s_ahead > max_s_ahead) target_s_ahead = max_s_ahead;
  //   if (target_s_ahead < 1) target_s_ahead = 1;

  //   if (target_s_ahead != s_ahead_env) {
  //       int32_t shift_us = (target_s_ahead - s_ahead_env) * slot_duration_us;
  //       p7_info->estimated_mean_late += shift_us;

  //       NFAPI_TRACE(NFAPI_TRACE_INFO, "[P7_SYNC] Dynamic Slot Ahead Adjusted: %d -> %d (worst_late: %d, mean: %d, in_panic: %d)",
  //                   s_ahead_env, target_s_ahead, worst_late, p7_info->estimated_mean_late, in_panic);
  //       p7_info->last_total_advanced_us = p7_info->total_advanced_us;
  //       s_ahead_env = target_s_ahead;
  //   } else if (p7_info->sfn % 256 == 0 && p7_info->slot == 0) {
	// 	NFAPI_TRACE(NFAPI_TRACE_INFO, "[P7_SYNC] Slot Ahead Maintained: %d (worst_late: %d, mean: %d, in_panic: %d)",
	// 				s_ahead_env, worst_late, p7_info->estimated_mean_late, in_panic);
	// }
}

vnf_p7_rx_message_t* vnf_p7_rx_reassembly_queue_add_segment(vnf_p7_t* vnf_p7, vnf_p7_rx_reassembly_queue_t* queue, uint16_t sequence_number, uint16_t segment_number, uint8_t m, uint8_t* data, uint16_t data_len)
{
	vnf_p7_rx_message_t* msg = 0;
	// attempt to find a entry for this segment
	vnf_p7_rx_message_t* iterator = queue->msg_queue;
	while(iterator != 0)
	{
		if(iterator->sequence_number == sequence_number)
		{
			msg = iterator;
			break;
		}

		iterator = iterator->next;
	}
	
	// if found then copy data to message
	if(msg != 0)
	{
	
		msg->segments[segment_number].buffer = (uint8_t*)vnf_p7_malloc(vnf_p7, data_len);
		memcpy(msg->segments[segment_number].buffer, data, data_len);
		msg->segments[segment_number].length = data_len;

		msg->num_segments_received++;

		// set the segement number if we have the last segment
		if(m == 0)
			msg->num_segments_expected = segment_number + 1;
	}
	// else add new rx message entry
	else
	{
		// create a new message
		msg = (vnf_p7_rx_message_t*)(vnf_p7_malloc(vnf_p7, sizeof(vnf_p7_rx_message_t)));
		memset(msg, 0, sizeof(vnf_p7_rx_message_t));

		msg->sequence_number = sequence_number;
		msg->num_segments_expected = m ? 255 : segment_number + 1;
		msg->num_segments_received = 1;
		msg->rx_hr_time = vnf_get_current_time_hr();

		msg->segments[segment_number].buffer = (uint8_t*)vnf_p7_malloc(vnf_p7, data_len);
		memcpy(msg->segments[segment_number].buffer, data, data_len);
		msg->segments[segment_number].length = data_len;

		// place the message at the head of the queue
		msg->next = queue->msg_queue;
		queue->msg_queue = msg;
	}

	return msg;
}

void vnf_p7_rx_reassembly_queue_remove_msg(vnf_p7_t* vnf_p7, vnf_p7_rx_reassembly_queue_t* queue, vnf_p7_rx_message_t* msg)
{
	// remove message if it has the same sequence number
	vnf_p7_rx_message_t* iterator = queue->msg_queue;
	vnf_p7_rx_message_t* previous = 0;

	while(iterator != 0)
	{
		if(iterator->sequence_number == msg->sequence_number)
		{
			if(previous == 0)
			{
				queue->msg_queue = iterator->next;
			}
			else
			{
				previous->next = iterator->next;
			}

			//NFAPI_TRACE(NFAPI_TRACE_INFO, "Deleting reassembly message\n");
			// delete the message
			uint16_t i;
			for(i = 0; i < 128; ++i)
			{
				if(iterator->segments[i].buffer)
					vnf_p7_free(vnf_p7, iterator->segments[i].buffer);
			}
			vnf_p7_free(vnf_p7, iterator);

			break;
		}

		previous = iterator;
		iterator = iterator->next;
	}
}

void vnf_p7_rx_reassembly_queue_remove_old_msgs(vnf_p7_t* vnf_p7, vnf_p7_rx_reassembly_queue_t* queue, uint32_t delta)
{
	// remove all messages that are too old
	vnf_p7_rx_message_t* iterator = queue->msg_queue;
	vnf_p7_rx_message_t* previous = 0;

	uint32_t rx_hr_time = vnf_get_current_time_hr();

	while(iterator != 0)
	{
		if(rx_hr_time - iterator->rx_hr_time > delta)
		{
			if(previous == 0)
			{
				queue->msg_queue = iterator->next;
			}
			else
			{
				previous->next = iterator->next;
			}
			
			NFAPI_TRACE(NFAPI_TRACE_WARN, "Deleting stale reassembly message (packet rx_hr_time %u current rx_hr_time %u delta %d us)\n", iterator->rx_hr_time, rx_hr_time, delta);

			vnf_p7_rx_message_t* to_delete = iterator;
			iterator = iterator->next;

			// delete the message
			uint16_t i;
			for(i = 0; i < 128; ++i)
			{
				if(to_delete->segments[i].buffer)
					vnf_p7_free(vnf_p7, to_delete->segments[i].buffer);
			}
			vnf_p7_free(vnf_p7, to_delete);

		}
		else
		{
			previous = iterator;
			iterator = iterator->next;
		}
	}
}

void* vnf_p7_malloc(vnf_p7_t* vnf_p7, size_t size)
{
	(void)vnf_p7;
	return malloc(size);
}

void vnf_p7_free(vnf_p7_t* vnf_p7, void* ptr)
{
	(void)vnf_p7;
	free(ptr);
}

void vnf_p7_connection_info_list_add(vnf_p7_t* vnf_p7, nfapi_vnf_p7_connection_info_t* node)
{
	if (vnf_p7 == NULL || node == NULL)
		return;

	node->next = vnf_p7->p7_connections;
	vnf_p7->p7_connections = node;
}

nfapi_vnf_p7_connection_info_t* vnf_p7_connection_info_list_find(vnf_p7_t* vnf_p7, uint16_t phy_id)
{
	if (vnf_p7 == NULL)
		return NULL;

	nfapi_vnf_p7_connection_info_t* iterator = vnf_p7->p7_connections;
	while (iterator != NULL) {
		if (iterator->phy_id == phy_id)
			return iterator;
		iterator = iterator->next;
	}

	return NULL;
}

nfapi_vnf_p7_connection_info_t* vnf_p7_connection_info_list_delete(vnf_p7_t* vnf_p7, uint16_t phy_id)
{
	if (vnf_p7 == NULL)
		return NULL;

	nfapi_vnf_p7_connection_info_t* iterator = vnf_p7->p7_connections;
	nfapi_vnf_p7_connection_info_t* previous = NULL;

	while (iterator != NULL) {
		if (iterator->phy_id == phy_id) {
			if (previous == NULL) {
				vnf_p7->p7_connections = iterator->next;
			} else {
				previous->next = iterator->next;
			}
			iterator->next = NULL;
			return iterator;
		}
		previous = iterator;
		iterator = iterator->next;
	}

	return NULL;
}

void vnf_p7_codec_free(vnf_p7_t* vnf_p7, void* ptr)
{
	if (ptr != NULL) {
		vnf_p7_free(vnf_p7, ptr);
	}
}

void handle_dynamic_timing_info(nfapi_vnf_p7_connection_info_t *p7_info,
                                void *void_ind)
{
    if (p7_info == NULL || void_ind == NULL) {
        return;
    }

    const nfapi_nr_timing_info_t *ind =
            (const nfapi_nr_timing_info_t *)void_ind;

    /*
     * The extractor is intentionally simplified:
     *
     *   one timing_info indication -> one merged timing sample
     *
     * Do not create multiple per-slot samples here.  The controller
     * should react to the worst timing condition observed in this
     * reporting period only.
     */
    vnf_timing_stats_t stats[1];

    int count = vnf_p7_extract_timing_info(ind, p7_info, stats, 1);

    if (count <= 0) {
        return;
    }

    vnf_timing_stats_t merged;

    merged.worst_late = INT32_MIN;
    merged.worst_early = INT32_MAX;
    merged.packet_slot = 0;
    merged.pnf_reported_jitter = 0;

    for (int i = 0; i < count; ++i) {
        if (stats[i].worst_late > merged.worst_late) {
            merged.worst_late = stats[i].worst_late;
        }

        if (stats[i].worst_early < merged.worst_early) {
            merged.worst_early = stats[i].worst_early;
        }

        if (stats[i].pnf_reported_jitter > merged.pnf_reported_jitter) {
            merged.pnf_reported_jitter = stats[i].pnf_reported_jitter;
        }
    }

    if (merged.worst_late == INT32_MIN) {
        return;
    }

    if (merged.worst_early == INT32_MAX) {
        merged.worst_early = merged.worst_late;
    }

    /*
     * Important:
     *
     * p7_run_ewma_lab_control() must treat merged.worst_late as the
     * only authoritative timing input for this period.
     *
     * Positive worst_late means reliability failure / no-drop guardrail
     * should dominate.
     *
     * Negative worst_late means there is timing headroom, but DOWN
     * should still be conservative to avoid oscillation.
     */
    int32_t slot_duration_us = 1000 >> p7_info->mu;

    if (slot_duration_us <= 0) {
        return;
    }

    nfapi_vnf_config_t *config = get_config();

    if (config == NULL) {
        return;
    }

    p7_run_ewma_lab_control(
            p7_info,
            &merged,
            slot_duration_us,
            global_max_s_ahead);

    return;
}

uint32_t vnf_get_current_time_hr()
{
	struct timeval now;
	(void)gettimeofday(&now, NULL);
	uint32_t time_hr = TIME2TIMEHR(now);
	return time_hr;
}

uint16_t increment_sfn_sf(uint16_t sfn_sf)
{
	if((sfn_sf & 0xF) == 9)
	{
		sfn_sf += 0x0010;
		sfn_sf &= 0x3FF0;
	}
	else if((sfn_sf & 0xF) > 9)
	{
		// error should not happen
	}
	else
	{
		sfn_sf++;
	}

	return sfn_sf;
}

struct timespec timespec_delta(struct timespec start, struct timespec end)
{
	struct timespec temp;
	if ((end.tv_nsec-start.tv_nsec)<0) 
	{
		temp.tv_sec = end.tv_sec-start.tv_sec-1;
		temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
	} 
	else 
	{
		temp.tv_sec = end.tv_sec-start.tv_sec;
		temp.tv_nsec = end.tv_nsec-start.tv_nsec;
	}
	return temp;
}

/*! Compute signed difference between two TIMEHR timestamps in microseconds.
 *  Handles 12-bit second wrap-around (every 4096 seconds) correctly
 *  for differences up to ~2048 seconds.
 */
static inline int64_t timehr_diff_us(uint32_t time_hr_a, uint32_t time_hr_b)
{
    // Extract seconds and microseconds
    int32_t sec_a = TIMEHR_SEC(time_hr_a);
    int32_t sec_b = TIMEHR_SEC(time_hr_b);
    int32_t usec_a = TIMEHR_USEC(time_hr_a);
    int32_t usec_b = TIMEHR_USEC(time_hr_b);
    
    // Handle 12-bit second wrap-around
    // sec_a - sec_b should be in range [-2048, 2047] for valid comparisons
    int32_t sec_diff = sec_a - sec_b;
    if (sec_diff > 2048) sec_diff -= 4096;   // sec_a wrapped, sec_b didn't
    if (sec_diff < -2048) sec_diff += 4096;  // sec_b wrapped, sec_a didn't
    
    return (int64_t)sec_diff * 1000000 + (usec_a - usec_b);
}

static uint32_t get_sf_time(uint32_t now_hr, uint32_t sf_start_hr)
{
	if(now_hr < sf_start_hr)
	{
		NFAPI_TRACE(NFAPI_TRACE_INFO, "now is earlier than start of subframe\n");
		return 0;
	}
	else
	{
		uint32_t now_us = TIMEHR_USEC(now_hr);
		uint32_t sf_start_us = TIMEHR_USEC(sf_start_hr);

		// if the us have wrapped adjust for it
		if(now_hr < sf_start_us)
		{
			now_us += 1000000;
		}

		return now_us - sf_start_us;
	}
}

static uint32_t get_slot_time(uint32_t now_hr, uint32_t slot_start_hr)
{
	// Use proper signed difference to handle wrap-around
	int64_t diff_us = timehr_diff_us(now_hr, slot_start_hr);
	if (diff_us < 0) {
		NFAPI_TRACE(NFAPI_TRACE_INFO, "now is earlier than start of slot\n");
		return 0;
	}
	return (uint32_t)diff_us;
}

uint32_t calculate_t1(uint16_t sfn_sf, uint32_t sf_start_time_hr)
{
	uint32_t now_time_hr = vnf_get_current_time_hr();

	uint32_t sf_time_us = get_sf_time(now_time_hr, sf_start_time_hr);

	uint32_t t1 = (NFAPI_SFNSF2DEC(sfn_sf) * 1000) + sf_time_us;

	return t1;
}

uint32_t calculate_nr_t1(int mu, uint16_t sfn, uint16_t slot, uint32_t slot_start_time_hr)
{
	uint32_t now_time_hr = vnf_get_current_time_hr();

	uint32_t slot_time_us = get_slot_time(now_time_hr, slot_start_time_hr);

	uint32_t t1 = NFAPI_SFNSLOT2DEC(mu, sfn,slot) * NFAPI_SLOTLEN(mu) + slot_time_us;
	
	return t1;
}


uint32_t calculate_t4(uint32_t now_time_hr, uint16_t sfn_sf, uint32_t sf_start_time_hr)
{
	uint32_t sf_time_us = get_sf_time(now_time_hr, sf_start_time_hr);

	uint32_t t4 = (NFAPI_SFNSF2DEC(sfn_sf) * 1000) + sf_time_us;

	return t4;

}

uint32_t calculate_nr_t4(uint32_t now_time_hr, int mu, uint16_t sfn, uint16_t slot, uint32_t slot_start_time_hr)
{
	uint32_t slot_time_us = get_slot_time(now_time_hr, slot_start_time_hr);

	uint32_t t4 = NFAPI_SFNSLOT2DEC(mu, sfn,slot) * NFAPI_SLOTLEN(mu) + slot_time_us;
	
	return t4;

}


uint32_t calculate_transmit_timestamp(int mu, uint16_t sfn, uint16_t slot, uint32_t slot_start_time_hr)
{
	uint32_t now_time_hr = vnf_get_current_time_hr();

	uint32_t slot_time_us = get_slot_time(now_time_hr, slot_start_time_hr);

	uint32_t tt = NFAPI_SFNSLOT2DEC(mu, sfn, slot) * NFAPI_SLOTLEN(mu) + slot_time_us;
	
	return tt;
}


uint16_t increment_sfn_sf_by(uint16_t sfn_sf, uint8_t increment)
{
	while(increment > 0)
	{
		sfn_sf = increment_sfn_sf(sfn_sf);
		--increment;
	}

	return sfn_sf;
}

int send_mac_slot_indications(vnf_p7_t* vnf_p7)
{
	nfapi_vnf_p7_connection_info_t* curr = vnf_p7->p7_connections;
	while(curr != 0)
	{
		if(curr->in_sync == 1)
		{
			// ask for subframes in the future
			//uint16_t sfn_sf_adv = increment_sfn_sf_by(curr->sfn_sf, 2);

			//vnf_p7->_public.subframe_indication(&(vnf_p7->_public), curr->phy_id, sfn_sf_adv);
            // suggestion fix by Haruki NAOI
			//printf("\nsfn:%d, slot:%d\n",curr->sfn,curr->slot);
			vnf_p7->_public.slot_indication(&(vnf_p7->_public), curr->phy_id, curr->sfn,curr->slot);
		}

		curr = curr->next;
	}

	return 0;
}

int send_mac_subframe_indications(vnf_p7_t* vnf_p7)
{
	nfapi_vnf_p7_connection_info_t* curr = vnf_p7->p7_connections;
	while(curr != 0)
	{
		if(curr->in_sync == 1)
		{
			// ask for subframes in the future
			//uint16_t sfn_sf_adv = increment_sfn_sf_by(curr->sfn_sf, 2);

			//vnf_p7->_public.subframe_indication(&(vnf_p7->_public), curr->phy_id, sfn_sf_adv);
            // suggestion fix by Haruki NAOI
			vnf_p7->_public.subframe_indication(&(vnf_p7->_public), curr->phy_id, curr->sfn_sf);
		}

		curr = curr->next;
	}

	return 0;
}

int vnf_send_p7_msg(vnf_p7_t* vnf_p7, nfapi_vnf_p7_connection_info_t* p7_info, uint8_t* msg, const uint32_t len)
{
	int sendto_result = sendto(vnf_p7->socket, msg, len, 0, (struct sockaddr*)&(p7_info->remote_addr), sizeof(p7_info->remote_addr)); 
	//printf("P7 msg sent \n");
	if(sendto_result != len)
	{
		NFAPI_TRACE(NFAPI_TRACE_INFO, "%s() sendto_result %d %d\n", __FUNCTION__, sendto_result, errno);
	}

	return 0;
}

int vnf_p7_pack_and_send_p7_msg(vnf_p7_t* vnf_p7, nfapi_p7_message_header_t* header)
{

	nfapi_vnf_p7_connection_info_t* p7_connection = vnf_p7_connection_info_list_find(vnf_p7, header->phy_id);
	if(p7_connection)
	{
		int send_result = 0;
		uint8_t  buffer[1024 * 32];

		header->m_segment_sequence = NFAPI_P7_SET_MSS(0, 0, p7_connection->sequence_number);
		
		int len = nfapi_p7_message_pack(header, buffer, sizeof(buffer), &vnf_p7->_public.codec_config);
		
                //NFAPI_TRACE(NFAPI_TRACE_INFO, "%s() phy_id:%d nfapi_p7_message_pack()=len=%d vnf_p7->_public.segment_size:%u\n", __FUNCTION__, header->phy_id, len, vnf_p7->_public.segment_size);

		if(len < 0) 
		{
			NFAPI_TRACE(NFAPI_TRACE_INFO, "%s() failed to pack p7 message phy_id:%d\n", __FUNCTION__, header->phy_id);
			return -1;
		}

		if(len > vnf_p7->_public.segment_size)
		{
			// todo : consider replacing with the sendmmsg call
			// todo : worry about blocking writes?
		
			// segmenting the transmit
			int msg_body_len = len - NFAPI_P7_HEADER_LENGTH ; 
			int seg_body_len = vnf_p7->_public.segment_size - NFAPI_P7_HEADER_LENGTH ; 
			int segment_count = (msg_body_len / (seg_body_len)) + ((msg_body_len % seg_body_len) ? 1 : 0); 
				
			int segment = 0;
			int offset = NFAPI_P7_HEADER_LENGTH;
			uint8_t tx_buffer[vnf_p7->_public.segment_size];
                        NFAPI_TRACE(NFAPI_TRACE_INFO, "%s() MORE THAN ONE SEGMENT phy_id:%d nfapi_p7_message_pack()=len=%d vnf_p7->_public.segment_size:%u\n", __FUNCTION__, header->phy_id, len, vnf_p7->_public.segment_size);
			for(segment = 0; segment < segment_count; ++segment)
			{
				uint8_t last = 0;
				uint16_t size = vnf_p7->_public.segment_size - NFAPI_P7_HEADER_LENGTH;
				if(segment + 1 == segment_count)
				{
					last = 1;
					size = (msg_body_len) - (seg_body_len * segment);
				}

				uint16_t segment_size = size + NFAPI_P7_HEADER_LENGTH;

				// Update the header with the m and segement 
				memcpy(&tx_buffer[0], buffer, NFAPI_P7_HEADER_LENGTH);

				// set the segment length
				tx_buffer[4] = (segment_size & 0xFF00) >> 8;
				tx_buffer[5] = (segment_size & 0xFF);

				// set the m & segment number
				tx_buffer[6] = ((!last) << 7) + segment;

				memcpy(&tx_buffer[NFAPI_P7_HEADER_LENGTH], &buffer[0] + offset, size);
				offset += size;

				if(vnf_p7->_public.checksum_enabled)
				{
					nfapi_p7_update_checksum(tx_buffer, segment_size);
				}
			
				nfapi_p7_update_transmit_timestamp(buffer, calculate_transmit_timestamp(p7_connection->mu, p7_connection->sfn, p7_connection->slot, vnf_p7->slot_start_time_hr));	

				send_result = vnf_send_p7_msg(vnf_p7, p7_connection,  &tx_buffer[0], segment_size);
			}
		}
		else
		{
			if(vnf_p7->_public.checksum_enabled)
			{
				nfapi_p7_update_checksum(buffer, len);
			}

			nfapi_p7_update_transmit_timestamp(buffer, calculate_transmit_timestamp(p7_connection->mu, p7_connection->sfn, p7_connection->slot, vnf_p7->slot_start_time_hr));	

			// simple case that the message fits in a single segement
			send_result = vnf_send_p7_msg(vnf_p7, p7_connection, &buffer[0], len);
		}

		p7_connection->sequence_number++;

		return send_result;
	}
	else
	{
		NFAPI_TRACE(NFAPI_TRACE_INFO, "%s() cannot find p7 connection info for phy_id:%d\n", __FUNCTION__, header->phy_id);
		return -1;
	}
}
int vnf_build_send_dl_node_sync(vnf_p7_t* vnf_p7, nfapi_vnf_p7_connection_info_t* p7_info)
{
	nfapi_dl_node_sync_t dl_node_sync;
	memset(&dl_node_sync, 0, sizeof(dl_node_sync));

	dl_node_sync.header.phy_id = p7_info->phy_id;
	dl_node_sync.header.message_id = NFAPI_DL_NODE_SYNC;
	dl_node_sync.t1 = calculate_t1(p7_info->sfn_sf, vnf_p7->sf_start_time_hr);
	dl_node_sync.delta_sfn_sf = 0;

	return vnf_p7_pack_and_send_p7_msg(vnf_p7, &dl_node_sync.header);	
}

int vnf_nr_build_send_dl_node_sync(vnf_p7_t* vnf_p7, nfapi_vnf_p7_connection_info_t* p7_info)
{
  nfapi_vnf_p7_config_t* config = (nfapi_vnf_p7_config_t*)vnf_p7;
	nfapi_nr_dl_node_sync_t dl_node_sync;
	memset(&dl_node_sync, 0, sizeof(dl_node_sync));

	dl_node_sync.header.phy_id = p7_info->phy_id;
	dl_node_sync.header.message_id = NFAPI_NR_PHY_MSG_TYPE_DL_NODE_SYNC;
	//dl_node_sync.t1 = calculate_t1(p7_info->sfn_sf, vnf_p7->sf_start_time_hr);
	dl_node_sync.t1 = calculate_nr_t1(p7_info->mu, p7_info->sfn,p7_info->slot, vnf_p7->slot_start_time_hr);
	dl_node_sync.delta_sfn_slot = 0; // Do not shift PNF slot; keep PNF timing stable.

	return config->send_p7_msg(vnf_p7, &dl_node_sync.header);
}

int vnf_nr_sync(vnf_p7_t* vnf_p7, nfapi_vnf_p7_connection_info_t* p7_info)
{

	if(p7_info->in_sync == 1)
	{
		uint16_t dl_sync_period_mask = p7_info->dl_in_sync_period-1;
		uint16_t sfn_slot_dec = NFAPI_SFNSLOT2DEC(p7_info->mu, p7_info->sfn,p7_info->slot);

		if ((((sfn_slot_dec + p7_info->dl_in_sync_offset) % NFAPI_MAX_SFNSLOTDEC(p7_info->mu)) & dl_sync_period_mask) == 0)
		{
			vnf_nr_build_send_dl_node_sync(vnf_p7, p7_info);
		}
	}
	else
	{
		uint16_t dl_sync_period_mask = p7_info->dl_out_sync_period-1;
		//uint16_t sfn_sf_dec = NFAPI_SFNSF2DEC(p7_info->sfn_sf);
		uint16_t sfn_slot_dec = NFAPI_SFNSLOT2DEC(p7_info->mu, p7_info->sfn, p7_info->slot);

		if ((((sfn_slot_dec + p7_info->dl_out_sync_offset) % NFAPI_MAX_SFNSLOTDEC(p7_info->mu)) & dl_sync_period_mask) == 0) 
		{
			vnf_nr_build_send_dl_node_sync(vnf_p7, p7_info);
		}
	}
	return 0;
}


int vnf_sync(vnf_p7_t* vnf_p7, nfapi_vnf_p7_connection_info_t* p7_info)
{

	if(p7_info->in_sync == 1)
	{
		uint16_t dl_sync_period_mask = p7_info->dl_in_sync_period-1;
		uint16_t sfn_sf_dec = NFAPI_SFNSF2DEC(p7_info->sfn_sf);

		if ((((sfn_sf_dec + p7_info->dl_in_sync_offset) % NFAPI_MAX_SFNSFDEC) & dl_sync_period_mask) == 0)
		{
			vnf_build_send_dl_node_sync(vnf_p7, p7_info);
		}
	}
	else
	{
		uint16_t dl_sync_period_mask = p7_info->dl_out_sync_period-1;
		uint16_t sfn_sf_dec = NFAPI_SFNSF2DEC(p7_info->sfn_sf);

		if ((((sfn_sf_dec + p7_info->dl_out_sync_offset) % NFAPI_MAX_SFNSFDEC) & dl_sync_period_mask) == 0)
		{
			vnf_build_send_dl_node_sync(vnf_p7, p7_info);
		}
	}
	return 0;
}


void vnf_handle_harq_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_harq_indication_t ind;
	
		if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config) < 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
			if(vnf_p7->_public.harq_indication)
			{
				(vnf_p7->_public.harq_indication)(&(vnf_p7->_public), &ind);
			}
		}
	
		vnf_p7_codec_free(vnf_p7, ind.harq_indication_body.harq_pdu_list);
		vnf_p7_codec_free(vnf_p7, ind.vendor_extension);
	}
}

void vnf_handle_crc_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_crc_indication_t ind;
	
		if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config) < 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to message\n", __FUNCTION__);
		}
		else
		{
			if(vnf_p7->_public.crc_indication)
			{
				(vnf_p7->_public.crc_indication)(&(vnf_p7->_public), &ind);
			}
		}
	
		vnf_p7_codec_free(vnf_p7, ind.crc_indication_body.crc_pdu_list);
		vnf_p7_codec_free(vnf_p7, ind.vendor_extension);
	}
}

void vnf_handle_rx_ulsch_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_rx_indication_t ind;
	
		if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config) < 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
			if(vnf_p7->_public.rx_indication)
			{
				(vnf_p7->_public.rx_indication)(&(vnf_p7->_public), &ind);
			}
		}

		vnf_p7_codec_free(vnf_p7, ind.rx_indication_body.rx_pdu_list);
		vnf_p7_codec_free(vnf_p7, ind.vendor_extension);
	}

}

void vnf_handle_rach_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_rach_indication_t ind;
	
		if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config) < 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to message\n", __FUNCTION__);
		}
		else
		{
			if(vnf_p7->_public.rach_indication)
			{
				(vnf_p7->_public.rach_indication)(&vnf_p7->_public, &ind);
			}
		}
	
		vnf_p7_codec_free(vnf_p7, ind.rach_indication_body.preamble_list);
		vnf_p7_codec_free(vnf_p7, ind.vendor_extension);

	}
}

void vnf_handle_srs_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_srs_indication_t ind;

		if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config) < 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
			if(vnf_p7->_public.srs_indication)
			{
				(vnf_p7->_public.srs_indication)(&(vnf_p7->_public), &ind);
			}
		}
	
		vnf_p7_codec_free(vnf_p7, ind.srs_indication_body.srs_pdu_list);
		vnf_p7_codec_free(vnf_p7, ind.vendor_extension);	
	}
}

void vnf_handle_rx_sr_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_sr_indication_t ind;
	
		if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config) < 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
			if(vnf_p7->_public.sr_indication)
			{
				(vnf_p7->_public.sr_indication)(&(vnf_p7->_public), &ind);
			}
		}
	
		vnf_p7_codec_free(vnf_p7, ind.sr_indication_body.sr_pdu_list);
		vnf_p7_codec_free(vnf_p7, ind.vendor_extension);	
	}
}
void vnf_handle_rx_cqi_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_cqi_indication_t ind;
	
		if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config) < 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
			if(vnf_p7->_public.cqi_indication)
			{
				(vnf_p7->_public.cqi_indication)(&(vnf_p7->_public), &ind);
			}
		}
	
		vnf_p7_codec_free(vnf_p7, ind.cqi_indication_body.cqi_pdu_list);
		vnf_p7_codec_free(vnf_p7, ind.cqi_indication_body.cqi_raw_pdu_list);
		vnf_p7_codec_free(vnf_p7, ind.vendor_extension);	
		
	}

}

void vnf_handle_lbt_dl_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_lbt_dl_indication_t ind;

		if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config) < 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
			if(vnf_p7->_public.lbt_dl_indication)
			{
				(vnf_p7->_public.lbt_dl_indication)(&(vnf_p7->_public), &ind);
			}
		}
	
		vnf_p7_codec_free(vnf_p7, ind.lbt_dl_indication_body.lbt_indication_pdu_list);
		vnf_p7_codec_free(vnf_p7, ind.vendor_extension);
	}
}

void vnf_handle_nb_harq_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_nb_harq_indication_t ind;
	
		if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config) < 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
			if(vnf_p7->_public.nb_harq_indication)
			{
				(vnf_p7->_public.nb_harq_indication)(&(vnf_p7->_public), &ind);
			}
		}
	
		vnf_p7_codec_free(vnf_p7, ind.nb_harq_indication_body.nb_harq_pdu_list);
		vnf_p7_codec_free(vnf_p7, ind.vendor_extension);
	}
}

void vnf_handle_nrach_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_nrach_indication_t ind;
	
		if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config) < 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
			if(vnf_p7->_public.nrach_indication)
			{
				(vnf_p7->_public.nrach_indication)(&(vnf_p7->_public), &ind);
			}
		}
	
		vnf_p7_codec_free(vnf_p7, ind.nrach_indication_body.nrach_pdu_list);
		vnf_p7_codec_free(vnf_p7, ind.vendor_extension);
	}
}

void vnf_handle_ue_release_resp(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{

	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_ue_release_response_t resp;

		if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &resp, sizeof(resp), &vnf_p7->_public.codec_config) < 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}


		vnf_p7_codec_free(vnf_p7, resp.vendor_extension);
	}
}

void vnf_handle_p7_vendor_extension(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7, uint16_t message_id)
{
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else if(vnf_p7->_public.allocate_p7_vendor_ext)
	{
		uint16_t msg_size;
		nfapi_p7_message_header_t* msg = vnf_p7->_public.allocate_p7_vendor_ext(message_id, &msg_size);

		if(msg == 0)
		{
			NFAPI_TRACE(NFAPI_TRACE_INFO, "%s failed to allocate vendor extention structure\n", __FUNCTION__);
			return;
		}

		int unpack_result = nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, msg, msg_size, &vnf_p7->_public.codec_config);

		if(unpack_result == 0)
		{
			if(vnf_p7->_public.vendor_ext)
				vnf_p7->_public.vendor_ext(&(vnf_p7->_public), msg);
		}
		
		if(vnf_p7->_public.deallocate_p7_vendor_ext)
			vnf_p7->_public.deallocate_p7_vendor_ext(msg);
		
	}
}


void vnf_nr_handle_p7_vendor_extension(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7, uint16_t message_id)
{
  if (pRecvMsg == NULL || vnf_p7 == NULL)
  {
    NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
  }
  else if(vnf_p7->_public.allocate_p7_vendor_ext)
  {
    uint16_t msg_size;
    nfapi_nr_p7_message_header_t* msg = vnf_p7->_public.allocate_p7_vendor_ext(message_id, &msg_size);

    if(msg == 0)
    {
      NFAPI_TRACE(NFAPI_TRACE_INFO, "%s failed to allocate vendor extention structure\n", __FUNCTION__);
      return;
    }
    const bool result = vnf_p7->_public.unpack_func(pRecvMsg, recvMsgLen, msg, msg_size, &vnf_p7->_public.codec_config);
    if(!result)
    {
      if(vnf_p7->_public.vendor_ext)
        vnf_p7->_public.vendor_ext(&(vnf_p7->_public), msg);
    }

    if(vnf_p7->_public.deallocate_p7_vendor_ext)
      vnf_p7->_public.deallocate_p7_vendor_ext(msg);

  }
}

void vnf_handle_ul_node_sync(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	uint32_t now_time_hr = vnf_get_current_time_hr();

	if (pRecvMsg == NULL || vnf_p7  == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "vnf_handle_ul_node_sync: NULL parameters\n");
		return;
	}

	nfapi_ul_node_sync_t ind;
	if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(nfapi_ul_node_sync_t), &vnf_p7->_public.codec_config) < 0)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "Failed to unpack ul_node_sync\n");
		return;
	}

	//NFAPI_TRACE(NFAPI_TRACE_INFO, "Received UL_NODE_SYNC phy_id:%d t1:%d t2:%d t3:%d\n", ind.header.phy_id, ind.t1, ind.t2, ind.t3);

	nfapi_vnf_p7_connection_info_t* phy = vnf_p7_connection_info_list_find(vnf_p7, ind.header.phy_id);
	uint32_t t4 = calculate_t4(now_time_hr, phy->sfn_sf, vnf_p7->sf_start_time_hr);

	uint32_t tx_2_rx = t4>ind.t1 ? t4 - ind.t1 : t4 + NFAPI_MAX_SFNSFDEC - ind.t1 ;
	uint32_t pnf_proc_time = ind.t3 - ind.t2;

	// divide by 2 using shift operator
	uint32_t latency =  (tx_2_rx - pnf_proc_time) >> 1;

	if(!(phy->filtered_adjust))
	{
		phy->latency[phy->min_sync_cycle_count] = latency;

		NFAPI_TRACE(NFAPI_TRACE_NOTE, "(%4d/%d) PNF to VNF !sync phy_id:%d (t1/2/3/4:%8u, %8u, %8u, %8u) txrx:%4u procT:%3u latency(us):%4d\n",
				NFAPI_SFNSF2SFN(phy->sfn_sf), NFAPI_SFNSF2SF(phy->sfn_sf), ind.header.phy_id, ind.t1, ind.t2, ind.t3, t4, 
				tx_2_rx, pnf_proc_time, latency);
	}
	else
	{
		phy->latency[phy->min_sync_cycle_count] = latency;

		//if(phy->min_sync_cycle_count != SYNC_CYCLE_COUNT)
		{
			if (ind.t2 < phy->previous_t2 && ind.t1 > phy->previous_t1)
			{
				// Only t2 wrap has occurred!!!
				phy->sf_offset = (NFAPI_MAX_SFNSFDEC + ind.t2) - ind.t1 - latency;
			}
			else if (ind.t2 > phy->previous_t2 && ind.t1 < phy->previous_t1)
			{
				// Only t1 wrap has occurred
				phy->sf_offset = ind.t2 - ( ind.t1 + NFAPI_MAX_SFNSFDEC) - latency;
			}
			else
			{
				// Either no wrap or both have wrapped
				phy->sf_offset = ind.t2 - ind.t1 - latency;
			}

			if (phy->sf_offset_filtered == 0)
			{
				phy->sf_offset_filtered = phy->sf_offset;
			}
			else
			{
				int32_t oldFilteredValueShifted = phy->sf_offset_filtered << 5;
				int32_t newOffsetShifted = phy->sf_offset << 5;

				// 1/8 of new and 7/8 of old
				phy->sf_offset_filtered = ((newOffsetShifted >> 3) + ((oldFilteredValueShifted * 7) >> 3)) >> 5;
			}
		}

		if(1)
		{
                  struct timespec ts;
                  clock_gettime(CLOCK_MONOTONIC, &ts);

			NFAPI_TRACE(NFAPI_TRACE_INFO, "(%4d/%1d) %ld.%ld PNF to VNF phy_id:%2d (t1/2/3/4:%8u, %8u, %8u, %8u) txrx:%4u procT:%3u latency(us):%4d(avg:%4d) offset(us):%8d filtered(us):%8d wrap[t1:%u t2:%u]\n",
					NFAPI_SFNSF2SFN(phy->sfn_sf), NFAPI_SFNSF2SF(phy->sfn_sf), ts.tv_sec, ts.tv_nsec, ind.header.phy_id,
					ind.t1, ind.t2, ind.t3, t4, 
					tx_2_rx, pnf_proc_time, latency, phy->average_latency, phy->sf_offset, phy->sf_offset_filtered,
					(ind.t1<phy->previous_t1), (ind.t2<phy->previous_t2));
		}

	}

        if (phy->filtered_adjust && (phy->sf_offset_filtered > 1e6 || phy->sf_offset_filtered < -1e6))
        {
          phy->filtered_adjust = 0;
          phy->zero_count=0;
          phy->min_sync_cycle_count = 2;
          phy->in_sync = 0;
          NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s - ADJUST TOO BAD - go out of filtered phy->sf_offset_filtered:%d\n", __FUNCTION__, phy->sf_offset_filtered);
        }

	if(phy->min_sync_cycle_count)
		phy->min_sync_cycle_count--;

	if(phy->min_sync_cycle_count == 0)
	{
		uint32_t curr_sfn_sf = phy->sfn_sf;
		int32_t sfn_sf_dec = NFAPI_SFNSF2DEC(phy->sfn_sf);

		if(!phy->filtered_adjust)
		{
			int i = 0;
			//phy->average_latency = 0;
			for(i = 0; i < SYNC_CYCLE_COUNT; ++i)
			{
				phy->average_latency += phy->latency[i];

			}
			phy->average_latency /= SYNC_CYCLE_COUNT;

			phy->sf_offset = ind.t2 - (ind.t1 - phy->average_latency);

			sfn_sf_dec += (phy->sf_offset / 1000);
		}
		else
		{
			sfn_sf_dec += ((phy->sf_offset_filtered + 500) / 1000);	//Round up go from microsecond to subframe(1ms)
		}

		if(sfn_sf_dec < 0)
		{
			sfn_sf_dec += NFAPI_MAX_SFNSFDEC;
		}
		else if( sfn_sf_dec >= NFAPI_MAX_SFNSFDEC)
		{
			sfn_sf_dec -= NFAPI_MAX_SFNSFDEC;
		}

		uint16_t new_sfn_sf = NFAPI_SFNSFDEC2SFNSF(sfn_sf_dec);


		{
			phy->adjustment = NFAPI_SFNSF2DEC(new_sfn_sf) - NFAPI_SFNSF2DEC(curr_sfn_sf);

			NFAPI_TRACE(NFAPI_TRACE_INFO, "PNF to VNF phy_id:%d adjustment%d phy->previous_sf_offset_filtered:%d phy->previous_sf_offset_filtered:%d phy->sf_offset_trend:%d\n", ind.header.phy_id, phy->adjustment, phy->previous_sf_offset_filtered, phy->previous_sf_offset_filtered, phy->sf_offset_trend);

			phy->previous_t1 = 0;
			phy->previous_t2 = 0;

			if(phy->previous_sf_offset_filtered > 0)
			{
				if( phy->sf_offset_filtered > phy->previous_sf_offset_filtered)
				{
					// pnf is getting futher ahead of vnf
					//phy->sf_offset_trend = phy->sf_offset_filtered - phy->previous_sf_offset_filtered;
					phy->sf_offset_trend = (phy->sf_offset_filtered + phy->previous_sf_offset_filtered)/2;
				}
				else
				{
					// pnf is getting back in sync
				}
			}
			else if(phy->previous_sf_offset_filtered < 0)
			{
				if(phy->sf_offset_filtered < phy->previous_sf_offset_filtered)
				{
					// vnf is getting future ahead of pnf
					//phy->sf_offset_trend = -(phy->sf_offset_filtered - phy->previous_sf_offset_filtered);
					phy->sf_offset_trend = (-(phy->sf_offset_filtered + phy->previous_sf_offset_filtered)) /2;
				}
				else
				{
					//  vnf is getting back in sync
				}
			}

			
			int insync_minor_adjustment_1 = phy->sf_offset_trend / 6;
			int insync_minor_adjustment_2 = phy->sf_offset_trend / 2;


			if(insync_minor_adjustment_1 == 0)
				insync_minor_adjustment_1 = 2;

			if(insync_minor_adjustment_2 == 0)
				insync_minor_adjustment_2 = 10;

			if(!phy->filtered_adjust)
			{
				if(phy->adjustment < 10)
				{
					phy->zero_count++;

					if(phy->zero_count >= 10)
					{
						phy->filtered_adjust = 1;
						phy->zero_count = 0;

						NFAPI_TRACE(NFAPI_TRACE_NOTE, "***** Adjusting VNF SFN/SF switching to filtered mode\n");
					}
				}
				else
				{
					phy->zero_count = 0;
				}
			}
			else
			{
				// Fine level of adjustment
				if (phy->adjustment == 0)
				{
					if (phy->zero_count >= 10)
					{
						if(phy->in_sync == 0)
						{
							//NFAPI_TRACE(NFAPI_TRACE_NOTE, "VNF P7 In Sync with phy (phy_id:%d)\n", phy->phy_id); 

							if(vnf_p7->_public.sync_indication)
								(vnf_p7->_public.sync_indication)(&(vnf_p7->_public), phy->in_sync);
						}

						phy->in_sync = 1;
					}
					else
					{
						phy->zero_count++;
					}

					if(phy->in_sync)
					{
						// in sync
						if(phy->sf_offset_filtered > 250)
						{
							// VNF is slow
							phy->insync_minor_adjustment = insync_minor_adjustment_1; //25;
							phy->insync_minor_adjustment_duration = ((phy->sf_offset_filtered) / insync_minor_adjustment_1);
						}
						else if(phy->sf_offset_filtered < -250)
						{
							// VNF is fast
							phy->insync_minor_adjustment = -(insync_minor_adjustment_1); //25;
							phy->insync_minor_adjustment_duration = (((phy->sf_offset_filtered) / -(insync_minor_adjustment_1)));
						}
						else
						{
							phy->insync_minor_adjustment = 0;
						}

						if(phy->insync_minor_adjustment != 0)
						{
							NFAPI_TRACE(NFAPI_TRACE_NOTE, "(%4d/%d) VNF phy_id:%d Apply minor insync adjustment %dus for %d subframes (sf_offset_filtered:%d) %d %d %d NEW:%d CURR:%d adjustment:%d\n", 
										NFAPI_SFNSF2SFN(phy->sfn_sf), NFAPI_SFNSF2SF(phy->sfn_sf), ind.header.phy_id,
										phy->insync_minor_adjustment, phy->insync_minor_adjustment_duration, 
                                                                                phy->sf_offset_filtered, 
                                                                                insync_minor_adjustment_1, insync_minor_adjustment_2, phy->sf_offset_trend,
                                                                                NFAPI_SFNSF2DEC(new_sfn_sf),
                                                                                NFAPI_SFNSF2DEC(curr_sfn_sf),
                                                                                phy->adjustment); 
						}
					}
				}
				else
				{
					if (phy->in_sync)
					{
						if(phy->adjustment == 0)
						{
						}
						else if(phy->adjustment > 0)
						{
							// VNF is slow
							//if(phy->adjustment == 1)
							{
								//
								if(phy->sf_offset_filtered > 250)
								{
									// VNF is slow
									phy->insync_minor_adjustment = insync_minor_adjustment_2;
									phy->insync_minor_adjustment_duration = 2 * ((phy->sf_offset_filtered - 250) / insync_minor_adjustment_2);
								}
								else if(phy->sf_offset_filtered < -250)
								{
									// VNF is fast
									phy->insync_minor_adjustment = -(insync_minor_adjustment_2);
									phy->insync_minor_adjustment_duration = 2 * ((phy->sf_offset_filtered + 250) / -(insync_minor_adjustment_2));
								}
							
							}
							//else
							{
								// out of sync?
							}
							
							NFAPI_TRACE(NFAPI_TRACE_NOTE, "(%4d/%d) VNF phy_id:%d Apply minor insync adjustment %dus for %d subframes (adjustment:%d sf_offset_filtered:%d) %d %d %d NEW:%d CURR:%d adj:%d\n", 
										NFAPI_SFNSF2SFN(phy->sfn_sf), NFAPI_SFNSF2SF(phy->sfn_sf), ind.header.phy_id,
										phy->insync_minor_adjustment, phy->insync_minor_adjustment_duration, phy->adjustment, phy->sf_offset_filtered,
										insync_minor_adjustment_1, insync_minor_adjustment_2, phy->sf_offset_trend,
                                                                                NFAPI_SFNSF2DEC(new_sfn_sf),
                                                                                NFAPI_SFNSF2DEC(curr_sfn_sf),
                                                                                phy->adjustment); 
							
						}
						else if(phy->adjustment < 0)
						{
							// VNF is fast
							//if(phy->adjustment == -1)
							{
								//
								if(phy->sf_offset_filtered > 250)
								{
									// VNF is slow
									phy->insync_minor_adjustment = insync_minor_adjustment_2;
									phy->insync_minor_adjustment_duration = 2 * ((phy->sf_offset_filtered - 250) / insync_minor_adjustment_2);
								}
								else if(phy->sf_offset_filtered < -250)
								{
									// VNF is fast
									phy->insync_minor_adjustment = -(insync_minor_adjustment_2);
									phy->insync_minor_adjustment_duration = 2 * ((phy->sf_offset_filtered + 250) / -(insync_minor_adjustment_2));
								}
							}
							//else
							{
								// out of sync?
							}

							NFAPI_TRACE(NFAPI_TRACE_NOTE, "(%d/%d) VNF phy_id:%d Apply minor insync adjustment %dus for %d subframes (adjustment:%d sf_offset_filtered:%d) %d %d %d\n", 
										NFAPI_SFNSF2SFN(phy->sfn_sf), NFAPI_SFNSF2SF(phy->sfn_sf), ind.header.phy_id,
										phy->insync_minor_adjustment, phy->insync_minor_adjustment_duration, phy->adjustment, phy->sf_offset_filtered,
										insync_minor_adjustment_1, insync_minor_adjustment_2, phy->sf_offset_trend); 
						}

						/*
						if (phy->adjustment > 10 || phy->adjustment < -10)
						{
							phy->zero_count++;		// Add one to the getting out of sync counter
						}
						else
						{
							phy->zero_count = 0;		// Small error - zero the out of sync counter
						}

						if (phy->zero_count >= 10)	// If we have had 10 consecutive large errors - drop out of sync
						{
							NFAPI_TRACE(NFAPI_TRACE_NOTE, "we have fallen out of sync...\n");
							//pP7SockInfo->syncAchieved = 0;
						}
						*/
					}
				}
			}


			if(phy->in_sync == 0)
			{
				/*NFAPI_TRACE(NFAPI_TRACE_NOTE, "***** Adjusting VNF phy_id:%d SFN/SF (%s) from %d to %d (%d) mode:%s zeroCount:%u sync:%s\n",
					ind.header.phy_id, (phy->in_sync ? "via sfn" : "now"),
					NFAPI_SFNSF2DEC(curr_sfn_sf), NFAPI_SFNSF2DEC(new_sfn_sf), phy->adjustment, 
					phy->filtered_adjust ? "FILTERED" : "ABSOLUTE",
					phy->zero_count,
					phy->in_sync ? "IN_SYNC" : "OUT_OF_SYNC");*/

				phy->sfn_sf = new_sfn_sf;
			}
		}

		// reset for next cycle
		phy->previous_sf_offset_filtered = phy->sf_offset_filtered;
		phy->min_sync_cycle_count = 2;
		phy->sf_offset_filtered = 0;
		phy->sf_offset = 0;
	}
	else
	{
		phy->previous_t1 = ind.t1;
		phy->previous_t2 = ind.t2;
	}
}

//NR HANDLES FOR UPLINK MESSAGES
void vnf_handle_nr_slot_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_nr_slot_indication_scf_t ind = {0};
	  const bool result = vnf_p7->_public.unpack_func(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config);
		if(!result)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
			NFAPI_TRACE(NFAPI_TRACE_DEBUG, "%s: Handling NR SLOT Indication\n", __FUNCTION__);
                        if(vnf_p7->_public.nr_slot_indication)
			{
				(vnf_p7->_public.nr_slot_indication)(&ind);
			}
      free_slot_indication(&ind);
		}

	}
}
void vnf_handle_nr_rx_data_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_nr_rx_data_indication_t ind;
	  const bool result = vnf_p7->_public.unpack_func(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config);
		if(!result)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
			NFAPI_TRACE(NFAPI_TRACE_DEBUG, "%s: Handling RX Indication\n", __FUNCTION__);
                        if(vnf_p7->_public.nr_rx_data_indication)
			{
				(vnf_p7->_public.nr_rx_data_indication)(&ind);
			}
      free_rx_data_indication(&ind);
		}
	}
}

void vnf_handle_nr_crc_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_nr_crc_indication_t ind;
	  const bool result = vnf_p7->_public.unpack_func(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config);
		if(!result)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
		        NFAPI_TRACE(NFAPI_TRACE_DEBUG, "%s: Handling CRC Indication\n", __FUNCTION__);
			if(vnf_p7->_public.nr_crc_indication)
			{
				(vnf_p7->_public.nr_crc_indication)(&ind);
			}
      free_crc_indication(&ind);
		}
	}
}

void vnf_handle_nr_srs_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_nr_srs_indication_t ind;
	  const bool result = vnf_p7->_public.unpack_func(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config);
		if(!result)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
			if(vnf_p7->_public.nr_srs_indication)
			{
				(vnf_p7->_public.nr_srs_indication)(&ind);
			}
      free_srs_indication(&ind);
		}
	}
}

void vnf_handle_nr_uci_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_nr_uci_indication_t ind;
	  const bool result = vnf_p7->_public.unpack_func(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config);
		if(!result)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
		        NFAPI_TRACE(NFAPI_TRACE_DEBUG, "%s: Handling UCI Indication\n", __FUNCTION__);
			if(vnf_p7->_public.nr_uci_indication)
			{
				(vnf_p7->_public.nr_uci_indication)(&ind);
			}
      free_uci_indication(&ind);
		}
	}
}

void vnf_handle_nr_rach_indication(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	// ensure it's valid
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: NULL parameters\n", __FUNCTION__);
	}
	else
	{
		nfapi_nr_rach_indication_t ind;
	  const bool result = vnf_p7->_public.unpack_func(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config);
		if(!result)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: Failed to unpack message\n", __FUNCTION__);
		}
		else
		{
		        NFAPI_TRACE(NFAPI_TRACE_INFO, "%s: Handling RACH Indication\n", __FUNCTION__);
			if(vnf_p7->_public.nr_rach_indication)
			{
				(vnf_p7->_public.nr_rach_indication)(&ind);
			}
      free_rach_indication(&ind);
		}
	}
}

void vnf_nr_handle_ul_node_sync(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	uint32_t now_time_hr = vnf_get_current_time_hr();
	if (pRecvMsg == NULL || vnf_p7  == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "vnf_handle_ul_node_sync: NULL parameters\n");
		return;
	}

	nfapi_nr_ul_node_sync_t ind;
	if (!vnf_p7->_public.unpack_func(pRecvMsg, recvMsgLen, &ind, sizeof(ind), &vnf_p7->_public.codec_config)) {
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "Failed to unpack ul_node_sync\n");
		return;
	}

	nfapi_vnf_p7_connection_info_t* p7_info = vnf_p7_connection_info_list_find(vnf_p7, ind.header.phy_id);
	if (!p7_info) {
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "PHY instance not found for phy_id:%d\n", ind.header.phy_id);
		return;
	}
	uint32_t t4 = calculate_nr_t4(now_time_hr, p7_info->mu, p7_info->sfn, p7_info->slot, vnf_p7->slot_start_time_hr);
	/*
	* IEEE 1588 (PTP) Time Synchronization Algorithm
	* 
	* T1 = VNF Transmit Time (t1)    |   T2 = PNF Receive Time (t2)
	* T3 = PNF Transmit Time (t3)    |   T4 = VNF Receive Time (t4)
	*
	* Assuming symmetric network delay:
	* T2 - T1 = Delay + Offset
	* T4 - T3 = Delay - Offset
	* Offset = ((T2 - T1) - (T4 - T3)) / 2
	*/
	int64_t diff1 = (int64_t)ind.t2 - (int64_t)ind.t1;
	int64_t diff2 = (int64_t)t4 - (int64_t)ind.t3;
	int64_t wrap_us = 10240000LL;
	int64_t half_wrap = 5120000LL;
	// 10.24s Wrap-around protection (nFAPI timestamps are constrained by 1024 SFN loop)
	while (diff1 > half_wrap) diff1 -= wrap_us;
	while (diff1 < -half_wrap) diff1 += wrap_us;
	while (diff2 > half_wrap) diff2 -= wrap_us;
	while (diff2 < -half_wrap) diff2 += wrap_us;
	int32_t offset = (int32_t)((diff1 - diff2) / 2);
	int32_t owd = (int32_t)((diff1 + diff2) / 2);
	
	// EWMA smoothing of OWD to avoid transient spikes ruining bounds estimation
	if (owd > 0) {
		if (p7_info->ewma_owd_us == 0) {
			p7_info->ewma_owd_us = owd;
		} else {
			// RFC 6298 inspired alpha (1/8) for delay integration
			p7_info->ewma_owd_us = ((p7_info->ewma_owd_us * 7) + owd) / 8;
		}
	}

	// Positive offset implies VNF is BEHIND PNF (VNF Master time = PNF Slave time - Offset)
	// VNF MUST INCREASE speed (reduce sleep time) to catch up -> requires pending_us to be NEGATIVE
	// Negative offset implies VNF is AHEAD of PNF
	// VNF MUST DECREASE speed (increase sleep time) to fall back -> requires pending_us to be POSITIVE

	int slot_ahead = 0;
	bool dynamic_timing_enabled = false;
	get_vnf_timing_envs(&slot_ahead, &dynamic_timing_enabled);

	int32_t total_correction = offset;
	// int32_t phase_delta_us = p7_info->total_advanced_us - p7_info->last_total_advanced_us;
	// int32_t adaptive_gain = 8;
	// if (phase_delta_us != 0) {
	// 	if ((phase_delta_us > 0 && offset > 0) || (phase_delta_us < 0 && offset < 0)) {
	// 		adaptive_gain = 4; // previous advance direction agrees with current offset, allow stronger correction
	// 	} else {
	// 		adaptive_gain = 12; // previous adjustment overshot or reversed, dampen correction
	// 	}
	// }

	pthread_mutex_lock(&p7_info->mutex);

	// if (false && p7_info->sync_locked && dynamic_timing_enabled) {
	// 	// Drift Monitoring: If we are locked but the offset exceeds the locked tolerance,
	// 	// we must unlock and re-synchronize to avoid long-term instability.
	// 	if (total_correction < -MARGIN_TOLERANCE_LOCKED_US || total_correction > MARGIN_TOLERANCE_LOCKED_US) {
	// 		p7_info->sync_locked = 0;
	// 		NFAPI_TRACE(NFAPI_TRACE_WARN, "[P7_SYNC] Drift detected (%d us). Unlocking sync for re-calibration.\n", total_correction);
	// 	}
	// }

	if (!p7_info->sync_locked) {
		if (total_correction >= -MARGIN_TOLERANCE_US && total_correction <= MARGIN_TOLERANCE_US) {
			p7_info->sync_locked = 1;
			p7_info->total_advanced_us = slot_ahead * p7_info->slot_duration_us; // Account for initial phase offset!
		} else {
			int32_t s_adj = total_correction / (int32_t)p7_info->slot_duration_us;
			int32_t p_adj = total_correction % (int32_t)p7_info->slot_duration_us;
			p7_info->slot_adjustment += s_adj;
			p7_info->pending_us -= p_adj;
			p7_info->last_adjustment_time_hr = vnf_get_current_time_hr(); // Mask stale timing info
			p7_info->last_total_advanced_us = p7_info->total_advanced_us;
		}
	}
	pthread_mutex_unlock(&p7_info->mutex);
	// NFAPI_TRACE(NFAPI_TRACE_DEBUG, 
	// 	"[P7_SYNC] ul_node_sync phy_id:%d (t1/2/3/4:%8u,%8u,%8u,%8u) offset:%d owd:%d pending_us:%d locked:%d s_adj:%d p_adj:%d\n",
	// 	ind.header.phy_id, ind.t1, ind.t2, ind.t3, t4,
	// 	offset, owd, p7_info->pending_us, p7_info->sync_locked, 
	// 	total_correction / p7_info->slot_duration_us, 
	// 	total_correction % p7_info->slot_duration_us);
}

void vnf_handle_timing_info(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "vnf_handle_timing_info: NULL parameters\n");
		return;
	}

	nfapi_timing_info_t ind;
	if(nfapi_p7_message_unpack(pRecvMsg, recvMsgLen, &ind, sizeof(nfapi_timing_info_t), &vnf_p7->_public.codec_config) < 0)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "Failed to unpack timing_info\n");
		return;
	}

        if (vnf_p7 && vnf_p7->p7_connections)
        {
          int16_t vnf_pnf_sfnsf_delta = NFAPI_SFNSF2DEC(vnf_p7->p7_connections[0].sfn_sf) - NFAPI_SFNSF2DEC(ind.last_sfn_sf);

          //NFAPI_TRACE(NFAPI_TRACE_INFO, "%s() PNF:SFN/SF:%d VNF:SFN/SF:%d deltaSFNSF:%d\n", __FUNCTION__, NFAPI_SFNSF2DEC(ind.last_sfn_sf), NFAPI_SFNSF2DEC(vnf_p7->p7_connections[0].sfn_sf), vnf_pnf_sfnsf_delta);

          // Panos: Careful here!!! Modification of the original nfapi-code
          //if (vnf_pnf_sfnsf_delta>1 || vnf_pnf_sfnsf_delta < -1)
          if (vnf_pnf_sfnsf_delta>0 || vnf_pnf_sfnsf_delta < 0)
          {
            NFAPI_TRACE(NFAPI_TRACE_INFO, "%s() LARGE SFN/SF DELTA between PNF and VNF delta:%d VNF:%d PNF:%d\n\n\n\n\n\n\n\n\n", __FUNCTION__, vnf_pnf_sfnsf_delta, NFAPI_SFNSF2DEC(vnf_p7->p7_connections[0].sfn_sf), NFAPI_SFNSF2DEC(ind.last_sfn_sf));
            // Panos: Careful here!!! Modification of the original nfapi-code
            vnf_p7->p7_connections[0].sfn_sf = ind.last_sfn_sf;
          }
        }
}

void vnf_nr_handle_timing_info(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	if (pRecvMsg == NULL || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "vnf_handle_timing_info: NULL parameters\n");
		return;
	}

	nfapi_nr_timing_info_t ind;
	const bool result = vnf_p7->_public.unpack_func(pRecvMsg, recvMsgLen, &ind, sizeof(nfapi_timing_info_t), &vnf_p7->_public.codec_config);
	if(!result)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "Failed to unpack timing_info\n");
		return;
	}
	nfapi_vnf_p7_connection_info_t *p7_con = &vnf_p7->p7_connections[0];

	// Integration Step
    int slot_ahead = 0;
    bool dynamic_timing_enabled = false;
    get_vnf_timing_envs(&slot_ahead, &dynamic_timing_enabled);

	pthread_mutex_lock(&p7_con->mutex);
	if (!p7_con->initial_timinginfo_received) {
		p7_con->sfn = ind.last_sfn;
		p7_con->slot = ind.last_slot;
		p7_con->initial_timinginfo_received = 1;
	}
	pthread_cond_signal(&p7_con->initial_timinginfo_cond);
	pthread_mutex_unlock(&p7_con->mutex);

	// Only process dynamic timing once the VNF timing thread has initialized mu/slot duration
	if (dynamic_timing_enabled && p7_con->mu >= 0 && vnf_p7->slot_start_time_hr != 0) {
		handle_dynamic_timing_info(p7_con, &ind);
	}
}

void vnf_dispatch_p7_message(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
	nfapi_p7_message_header_t header;

	// validate the input params
	if(pRecvMsg == NULL || recvMsgLen < 4 || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: invalid input params\n", __FUNCTION__);
		return;
	}

	// unpack the message header
	if (nfapi_p7_message_header_unpack(pRecvMsg, recvMsgLen, &header, sizeof(header), &vnf_p7->_public.codec_config) < 0)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "Unpack message header failed, ignoring\n");
		return;
	}

	// ensure the message is sensible
	if (recvMsgLen < 8 || pRecvMsg == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_WARN, "Invalid message size: %d, ignoring\n", recvMsgLen);
		return;
	}

	switch (header.message_id)
	{
		case NFAPI_UL_NODE_SYNC:
			vnf_handle_ul_node_sync(pRecvMsg, recvMsgLen, vnf_p7);
			break;

		case NFAPI_TIMING_INFO:
			vnf_handle_timing_info(pRecvMsg, recvMsgLen, vnf_p7);
			break;
			
		case NFAPI_HARQ_INDICATION:
			vnf_handle_harq_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
	
		case NFAPI_CRC_INDICATION:
			vnf_handle_crc_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
	
		case NFAPI_RX_ULSCH_INDICATION:
			vnf_handle_rx_ulsch_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
	
		case NFAPI_RACH_INDICATION:
			vnf_handle_rach_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
	
		case NFAPI_SRS_INDICATION:
			vnf_handle_srs_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;

		case NFAPI_RX_SR_INDICATION:
			vnf_handle_rx_sr_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;

		case NFAPI_RX_CQI_INDICATION:
			vnf_handle_rx_cqi_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
			
		case NFAPI_LBT_DL_INDICATION:
			vnf_handle_lbt_dl_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
			
		case NFAPI_NB_HARQ_INDICATION:
			vnf_handle_nb_harq_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
			
		case NFAPI_NRACH_INDICATION:
			vnf_handle_nrach_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;			

		case NFAPI_UE_RELEASE_RESPONSE:
			vnf_handle_ue_release_resp(pRecvMsg, recvMsgLen, vnf_p7);
			break;

		default:
			{
				if(header.message_id >= NFAPI_VENDOR_EXT_MSG_MIN &&
				   header.message_id <= NFAPI_VENDOR_EXT_MSG_MAX)
				{
					vnf_handle_p7_vendor_extension(pRecvMsg, recvMsgLen, vnf_p7, header.message_id);
				}
				else
				{
					NFAPI_TRACE(NFAPI_TRACE_ERROR, "P7 Unknown message ID %d\n", header.message_id);
				}
			}
			break;
	}
}

void vnf_nr_handle_p7_message(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7)
{
  if (vnf_p7->terminate) {
    // VNF already terminated, ignore messages ( shouldn't get here, since the PNF doesn't send further messages after terminating as well )
    return;
  }
	nfapi_nr_p7_message_header_t header;

	// validate the input params
	if(pRecvMsg == NULL || recvMsgLen < 4 || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s: invalid input params\n", __FUNCTION__);
		return;
	}

	// unpack the message header
  const bool result = vnf_p7->_public.hdr_unpack_func(pRecvMsg, recvMsgLen, &header, sizeof(header), &vnf_p7->_public.codec_config);
	if (!result)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "Unpack message header failed, ignoring\n");
		return;
	}

	// ensure the message is sensible
	if (recvMsgLen < 8 || pRecvMsg == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_WARN, "Invalid message size: %d, ignoring\n", recvMsgLen);
		return;
	}

	switch (header.message_id)
	{
		case NFAPI_NR_PHY_MSG_TYPE_UL_NODE_SYNC:
			vnf_nr_handle_ul_node_sync(pRecvMsg, recvMsgLen, vnf_p7);
			break;

		case NFAPI_TIMING_INFO:
			vnf_nr_handle_timing_info(pRecvMsg, recvMsgLen, vnf_p7);
			break;
		
		case NFAPI_NR_PHY_MSG_TYPE_SLOT_INDICATION:
			vnf_handle_nr_slot_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
		
		case NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION:
			vnf_handle_nr_rx_data_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
	
		case NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION:
			vnf_handle_nr_crc_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
	
		case NFAPI_NR_PHY_MSG_TYPE_UCI_INDICATION:
			vnf_handle_nr_uci_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
	
		case NFAPI_NR_PHY_MSG_TYPE_SRS_INDICATION:
			vnf_handle_nr_srs_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;
	
		case NFAPI_NR_PHY_MSG_TYPE_RACH_INDICATION:
			vnf_handle_nr_rach_indication(pRecvMsg, recvMsgLen, vnf_p7);
			break;

		case NFAPI_UE_RELEASE_RESPONSE:
			vnf_handle_ue_release_resp(pRecvMsg, recvMsgLen, vnf_p7);
			break;

		default:
			{
				if(header.message_id >= NFAPI_VENDOR_EXT_MSG_MIN &&
				   header.message_id <= NFAPI_VENDOR_EXT_MSG_MAX)
				{
					vnf_handle_p7_vendor_extension(pRecvMsg, recvMsgLen, vnf_p7, header.message_id);
				}
				else
				{
					NFAPI_TRACE(NFAPI_TRACE_ERROR, "P7 Unknown message ID %d\n", header.message_id);
				}
			}
			break;
	}
}

void vnf_handle_p7_message(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7) 
{
	nfapi_p7_message_header_t messageHeader;

	// validate the input params
	if(pRecvMsg == NULL || recvMsgLen < 4 || vnf_p7 == NULL)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "vnf_handle_p7_message: invalid input params (%p %d %p)\n", pRecvMsg, recvMsgLen, vnf_p7);
		return;
	}

	// unpack the message header
	if (nfapi_p7_message_header_unpack(pRecvMsg, recvMsgLen, &messageHeader, sizeof(nfapi_p7_message_header_t), &vnf_p7->_public.codec_config) < 0)
	{
		NFAPI_TRACE(NFAPI_TRACE_ERROR, "Unpack message header failed, ignoring\n");
		return;
	}

	if(vnf_p7->_public.checksum_enabled)
	{
		uint32_t checksum = nfapi_p7_calculate_checksum(pRecvMsg, recvMsgLen);
		if(checksum != messageHeader.checksum)
		{
			NFAPI_TRACE(NFAPI_TRACE_ERROR, "Checksum verification failed %d %d msg:%d len:%d\n", checksum, messageHeader.checksum, messageHeader.message_id, recvMsgLen);
			return;
		}
	}

	uint8_t m = NFAPI_P7_GET_MORE(messageHeader.m_segment_sequence);
	uint8_t segment_num = NFAPI_P7_GET_SEGMENT(messageHeader.m_segment_sequence);
	uint8_t sequence_num = NFAPI_P7_GET_SEQUENCE(messageHeader.m_segment_sequence);


	if(m == 0 && segment_num == 0)
	{
		// we have a complete message
		// ensure the message is sensible
		if (recvMsgLen < 8 || pRecvMsg == NULL)
		{
			NFAPI_TRACE(NFAPI_TRACE_WARN, "Invalid message size: %d, ignoring\n", recvMsgLen);
			return;
		}

		//vnf_dispatch_p7_message(&messageHeader, pRecvMsg, recvMsgLen, vnf_p7);
		vnf_dispatch_p7_message(pRecvMsg, recvMsgLen, vnf_p7);
	}
	else
	{
		nfapi_vnf_p7_connection_info_t* phy = vnf_p7_connection_info_list_find(vnf_p7, messageHeader.phy_id);

		if(phy)
		{
			vnf_p7_rx_message_t* rx_msg = vnf_p7_rx_reassembly_queue_add_segment(vnf_p7, &(phy->reassembly_queue), sequence_num, segment_num, m, pRecvMsg, recvMsgLen);

			if(rx_msg->num_segments_received == rx_msg->num_segments_expected)
			{
				// send the buffer on
				uint16_t i = 0;
				uint16_t length = 0;
				for(i = 0; i < rx_msg->num_segments_expected; ++i)
				{
					length += rx_msg->segments[i].length - (i > 0 ? NFAPI_P7_HEADER_LENGTH : 0);
				}

				if(phy->reassembly_buffer_size < length)
				{
					vnf_p7_free(vnf_p7, phy->reassembly_buffer);
					phy->reassembly_buffer = 0;
				}

				if(phy->reassembly_buffer == 0)
				{
					NFAPI_TRACE(NFAPI_TRACE_NOTE, "Resizing VNF_P7 Reassembly buffer %d->%d\n", phy->reassembly_buffer_size, length);
					phy->reassembly_buffer = (uint8_t*)vnf_p7_malloc(vnf_p7, length);

					if(phy->reassembly_buffer == 0)
					{
						NFAPI_TRACE(NFAPI_TRACE_NOTE, "Failed to allocate VNF_P7 reassemby buffer len:%d\n", length);
						return;
					}
                                       memset(phy->reassembly_buffer, 0, length);
					phy->reassembly_buffer_size = length;
				}

				uint16_t offset = 0;
				for(i = 0; i < rx_msg->num_segments_expected; ++i)
				{
					if(i == 0)
					{
						memcpy(phy->reassembly_buffer, rx_msg->segments[i].buffer, rx_msg->segments[i].length);
						offset += rx_msg->segments[i].length;
					}
					else
					{
						memcpy(phy->reassembly_buffer + offset, rx_msg->segments[i].buffer + NFAPI_P7_HEADER_LENGTH, rx_msg->segments[i].length - NFAPI_P7_HEADER_LENGTH);
						offset += rx_msg->segments[i].length - NFAPI_P7_HEADER_LENGTH;
					}
				}


				//pnf_dispatch_p7_message(pnf_p7->reassemby_buffer, length, pnf_p7, rx_msg->rx_hr_time);
				vnf_dispatch_p7_message(phy->reassembly_buffer, length , vnf_p7);


				// delete the structure
				vnf_p7_rx_reassembly_queue_remove_msg(vnf_p7, &(phy->reassembly_queue), rx_msg);
			}

			vnf_p7_rx_reassembly_queue_remove_old_msgs(vnf_p7, &(phy->reassembly_queue), 1000);
		}
		else
		{

			NFAPI_TRACE(NFAPI_TRACE_INFO, "Unknown phy id %d\n", messageHeader.phy_id);
		}
	}
}


int vnf_p7_read_dispatch_message(vnf_p7_t* vnf_p7)
{
	int recvfrom_result = 0;
	struct sockaddr_in remote_addr;
	socklen_t remote_addr_size = sizeof(remote_addr);

	do
	{
		// peek the header
		uint8_t header_buffer[NFAPI_P7_HEADER_LENGTH];
		recvfrom_result = recvfrom(vnf_p7->socket, header_buffer, NFAPI_P7_HEADER_LENGTH, MSG_DONTWAIT | MSG_PEEK, (struct sockaddr*)&remote_addr, &remote_addr_size);

		if(recvfrom_result > 0)
		{
			// get the segment size
			nfapi_p7_message_header_t header;
			if(nfapi_p7_message_header_unpack(header_buffer, NFAPI_P7_HEADER_LENGTH, &header, sizeof(header), 0) < 0)
			{
				NFAPI_TRACE(NFAPI_TRACE_ERROR, "Unpack message header failed, ignoring\n");
				return -1;
			}

			// resize the buffer if we have a large segment
			if(header.message_length > vnf_p7->rx_message_buffer_size)
			{
				NFAPI_TRACE(NFAPI_TRACE_NOTE, "reallocing rx buffer %d\n", header.message_length);
				vnf_p7->rx_message_buffer = realloc(vnf_p7->rx_message_buffer, header.message_length);
				vnf_p7->rx_message_buffer_size = header.message_length;
			}

			// read the segment
			recvfrom_result = recvfrom(vnf_p7->socket, vnf_p7->rx_message_buffer, header.message_length, MSG_WAITALL | MSG_TRUNC, (struct sockaddr*)&remote_addr, &remote_addr_size);
			NFAPI_TRACE(NFAPI_TRACE_INFO, "recvfrom_result = %d from %s():%d\n", recvfrom_result, __FUNCTION__, __LINE__);

			// todo : how to handle incomplete readfroms, need some sort of buffer/select

			if (recvfrom_result > 0)
			{
				if (recvfrom_result != header.message_length)
				{
					NFAPI_TRACE(NFAPI_TRACE_ERROR, "(%d) Received unexpected number of bytes. %d != %d",
						    __LINE__, recvfrom_result, header.message_length);
					break;
				}
				NFAPI_TRACE(NFAPI_TRACE_INFO, "Calling vnf_nr_reassemble_p7_message from %d\n", __LINE__);
				vnf_handle_p7_message(vnf_p7->rx_message_buffer, recvfrom_result, vnf_p7);
				return 0;
			}
			else
			{
				NFAPI_TRACE(NFAPI_TRACE_ERROR, "recvfrom failed %d %d\n", recvfrom_result, errno);
			}
		}

		if(recvfrom_result == -1)
		{
			if(errno == EAGAIN || errno == EWOULDBLOCK)
			{
				// return to the select
				//NFAPI_TRACE(NFAPI_TRACE_WARN, "%s recvfrom would block :%d\n", __FUNCTION__, errno);
			}
			else
			{
				NFAPI_TRACE(NFAPI_TRACE_WARN, "%s recvfrom failed errno:%d\n", __FUNCTION__, errno);
			}
		}
	}
	while(recvfrom_result > 0);

	return 0;
}

void vnf_p7_release_msg(vnf_p7_t* vnf_p7, nfapi_p7_message_header_t* header)
{
	switch(header->message_id)
	{
		case NFAPI_HARQ_INDICATION:
			{
				vnf_p7_codec_free(vnf_p7, ((nfapi_harq_indication_t*)(header))->harq_indication_body.harq_pdu_list);
			}
			break;
		case NFAPI_CRC_INDICATION:
			{
				vnf_p7_codec_free(vnf_p7, ((nfapi_crc_indication_t*)(header))->crc_indication_body.crc_pdu_list);
			}
			break;
		case NFAPI_RX_ULSCH_INDICATION:
			{
				nfapi_rx_indication_t* rx_ind = (nfapi_rx_indication_t*)(header);
				size_t number_of_pdus = rx_ind->rx_indication_body.number_of_pdus;
                                assert(number_of_pdus <= NFAPI_RX_IND_MAX_PDU);
				for(size_t i = 0; i < number_of_pdus; ++i)
				{
					vnf_p7_codec_free(vnf_p7, rx_ind->rx_indication_body.rx_pdu_list[i].rx_ind_data);
				}

				vnf_p7_codec_free(vnf_p7, rx_ind->rx_indication_body.rx_pdu_list);
			}
			break;
		case NFAPI_RACH_INDICATION:
			{
				vnf_p7_codec_free(vnf_p7, ((nfapi_rach_indication_t*)(header))->rach_indication_body.preamble_list);
			}
			break;
		case NFAPI_SRS_INDICATION:
			{
				vnf_p7_codec_free(vnf_p7, ((nfapi_srs_indication_t*)(header))->srs_indication_body.srs_pdu_list);
			}
			break;
		case NFAPI_RX_SR_INDICATION:
			{
				vnf_p7_codec_free(vnf_p7, ((nfapi_sr_indication_t*)(header))->sr_indication_body.sr_pdu_list);
			}
			break;
		case NFAPI_RX_CQI_INDICATION:
			{
				vnf_p7_codec_free(vnf_p7, ((nfapi_cqi_indication_t*)(header))->cqi_indication_body.cqi_pdu_list);
				vnf_p7_codec_free(vnf_p7, ((nfapi_cqi_indication_t*)(header))->cqi_indication_body.cqi_raw_pdu_list);
			}
			break;
	}
				
	vnf_p7_free(vnf_p7, header);
	
}

void vnf_p7_release_pdu(vnf_p7_t* vnf_p7, void* pdu)
{
	vnf_p7_free(vnf_p7, pdu);
}
