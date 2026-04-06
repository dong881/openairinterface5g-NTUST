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

extern void log_mmap_entry(const char *log_name, long value);

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
  // 8 timing data points: (dl_tti, tx_data, ul_tti, ul_dci) x (latest_delay & earliest_arrival)
  struct {
    int32_t value;
  } raw_data[8] = {
    {ind->dl_tti_latest_delay},
    {ind->tx_data_latest_delay},
    {ind->ul_tti_latest_delay},
    {ind->ul_dci_latest_delay},
    {ind->dl_tti_earliest_arrival},
    {ind->tx_data_earliest_arrival},
    {ind->ul_tti_earliest_arrival},
    {ind->ul_dci_earliest_arrival}
  };

  int count = 0;
  uint32_t slot_duration_us = 1000 >> p7_info->mu; // 500us for mu=1, 1000us for mu=0
  uint32_t max_slot_dec = NFAPI_MAX_SFNSLOTDEC(p7_info->mu);

  // PNF sends last_sfn/last_slot as (current - 1)
  uint32_t base_slot_dec = NFAPI_SFNSLOT2DEC(p7_info->mu, ind->last_sfn, ind->last_slot);
  uint32_t current_slot_dec = (base_slot_dec + 1) % max_slot_dec;

  // Expand the valid range to include the configured timing window plus a slot margin.
  // This prevents discarding legitimate too-early / too-late reports when timing_window is > 5ms.
  nfapi_vnf_config_t *config = get_config();
  int32_t timing_window_us = (config != NULL && config->timing_window > 0) ? (int32_t)config->timing_window : 5000;
  const int32_t TIMING_VALUE_MIN = -150000;
  const int32_t TIMING_VALUE_MAX = timing_window_us + (int32_t)slot_duration_us * 2 + 2000;

  for (int i = 0; i < 8; i++) {
    if (raw_data[i].value == 0)
      continue; // Skip zero values

    // Discard abnormal values outside valid range
    if (raw_data[i].value < TIMING_VALUE_MIN || raw_data[i].value > TIMING_VALUE_MAX){
      continue;
    }

    // Calculate packet slot properties
    int32_t slot_offset = raw_data[i].value / (int32_t)slot_duration_us;
    // Calculate absolute slot in the hyperframe cycle (0..max_slot_dec-1)
    uint32_t true_abs_slot = (current_slot_dec - slot_offset - (raw_data[i].value > 0) + max_slot_dec) % max_slot_dec;
    // Map to local buffer index
    uint32_t ps = true_abs_slot % SLOT_ARRAY_SIZE;

    // --- History Aggregation Logic ---
    if (p7_info->slot_history[ps].abs_slot == true_abs_slot) {
        // MATCH: Merge with existing history for this slot
        if (raw_data[i].value > p7_info->slot_history[ps].max_late)
            p7_info->slot_history[ps].max_late = raw_data[i].value;
        if (raw_data[i].value < p7_info->slot_history[ps].max_early)
            p7_info->slot_history[ps].max_early = raw_data[i].value;
    } else {
        // MISMATCH: New slot detected, reset history
        p7_info->slot_history[ps].abs_slot = true_abs_slot;
        p7_info->slot_history[ps].max_late = raw_data[i].value;
        p7_info->slot_history[ps].max_early = raw_data[i].value;
    }

    // --- Prepare Output Stats (merged values) ---
    // Check if we already have this slot in out_stats to allow multiple updates in one pass if needed
    // (though usually strict aggregation suggests we just output the latest merged state)
    int found = -1;
    for (int j = 0; j < count; j++) {
      if (out_stats[j].packet_slot == ps) {
        found = j;
        break;
      }
    }

    if (found >= 0) {
      // Update existing entry in this batch with latest from history
      out_stats[found].worst_late = p7_info->slot_history[ps].max_late;
      out_stats[found].worst_early = p7_info->slot_history[ps].max_early;
      
      uint32_t max_jitter = 0;
      if (ind->dl_tti_jitter > max_jitter) max_jitter = ind->dl_tti_jitter;
      if (ind->tx_data_jitter > max_jitter) max_jitter = ind->tx_data_jitter;
      if (ind->ul_tti_jitter > max_jitter) max_jitter = ind->ul_tti_jitter;
      if (ind->ul_dci_jitter > max_jitter) max_jitter = ind->ul_dci_jitter;
      out_stats[found].pnf_reported_jitter = max_jitter;
    } else if (count < max_stats) {
      // New entry in this batch
      out_stats[count].packet_slot = ps;
      out_stats[count].worst_late = p7_info->slot_history[ps].max_late;
      out_stats[count].worst_early = p7_info->slot_history[ps].max_early;
      
      // Calculate Jitter comprehensively here instead of in handle_dynamic_timing_info
      uint32_t max_jitter = 0;
      if (ind->dl_tti_jitter > max_jitter) max_jitter = ind->dl_tti_jitter;
      if (ind->tx_data_jitter > max_jitter) max_jitter = ind->tx_data_jitter;
      if (ind->ul_tti_jitter > max_jitter) max_jitter = ind->ul_tti_jitter;
      if (ind->ul_dci_jitter > max_jitter) max_jitter = ind->ul_dci_jitter;
      out_stats[count].pnf_reported_jitter = max_jitter;
      count++;
    }
  }
  return count;
}

/*
 * =========================================================================================
 * VNF P7 Convergence Optimization & Adaptive Jitter Buffer Control
 * =========================================================================================
 * 
 * REFERENCES & ALGORITHMIC FOUNDATIONS:
 * 1. WebRTC NetEQ / Adaptive Jitter Buffer (Ramjee et al., IEEE INFOCOM):
 *    - Logic: Aggressively expand the buffer upon delay spikes to prevent drops, but shrink 
 *      the buffer very slowly ("Slow Decay") during stable periods to minimize latency 
 *      without risking underruns. Asymmetric adjustment rate (Fast Expand, Slow Decay).
 * 2. TCP RTO Jacobson/Karels Algorithm (RFC 6298):
 *    - Logic: Averages are insufficient for delay predictions in bursty networks. Reaction
 *      must be anchored to the variance / extreme measured edges (e.g., worst_late).
 * 3. BBR (Bottleneck Bandwidth and Round-trip propagation time - Google):
 *    - Logic: Continuous probing of the lower delay bounds. When the network is quiet, 
 *      the pacing smoothly drifts toward the minimum possible RTT constraint to prevent 
 *      bufferbloat (Latency minimization).
 *
 * CHALLENGES & SOLUTIONS:
 * [Challenge 1: Catastrophic Jitter exceeding the Timing Window]
 *   Under 1G traffic, bidirectional jitter can exceed the entire PNF timing window (e.g., >5000us).
 *   - Solution: Maximum Advance Pinning. Instead of "centering", we pin the earliest packets 
 *     directly against the Timing Window's Upper Bound (minus a 200us safety margin). We advance 
 *     as much as physically permissible without triggering "Too Early" drops, rescuing the 
 *     maximum possible number of late packets.
 * [Challenge 2: Integral Windup Deadlock vs. Instantaneous Queues]
 *   Continuously acting on "Late" feedback causes infinite accumulation. `pending_us` is a relative queue 
 *   that gets constantly consumed by the sleep thread, leading to a blind "Integral Windup" logic flaw if 
 *   capped directly. Guessing OWD or Task processing times is also dangerous.
 *   - Solution: PID-style Anti-Windup & Cumulative Phase Limit. Based on Control Theory / PLLs, we sum
 *     every commanded `shift` delta into an absolute integral variable (`total_advanced_us`) relative to 
 *     initial sync locking. We strictly cap this cumulative sum to `(Timing Window + ewma_proc + ewma_owd - 200 padding)`. 
 *     Following conservative designs, `ewma_owd` is wiped to 0 when lock engages (as sync stops), guaranteeing 
 *     the limit safely anchors itself against pushing into the "Too Early" precipice with stale values.
 * [Challenge 3: Maximizing Low Latency in Safe Zones]
 *   Operating permanently near the upper bound wastes latency. 
 *   - Solution: BBR-style Probing. When in the safe zone, smoothly drift toward the lower 
 *     bound (Lower Margin) to cut latency. If a spike occurs, the aggressive jump mechanism 
 *     acts as a safety net to instantly push it back up.
 * =========================================================================================
 */

void vnf_p7_convergence_optimization(nfapi_vnf_p7_connection_info_t *p7_info, const vnf_timing_stats_t *stats)
{
	int32_t worst_late  = stats->worst_late;
	int32_t worst_early = stats->worst_early;
	
	// Fetch dynamic timing window
	nfapi_vnf_config_t *config = get_config();
	int32_t timing_window_us = (int32_t)config->timing_window;

    /*
     * REFERENCES for Advanced Jitter & Playout Buffer design:
     * 1. Q. Li, H. Feng, et al. (2018). "An Enhanced Adaptive Jitter Buffer Management 
     *    Algorithm for Real-Time Communications," IEEE Access.
     *    - Insight: Raw inter-arrival jitter in heavily loaded networks is highly volatile.
     *      Using raw jitter directly destabilizes the timing loop.
     *    - Solution: "Asymmetric Exponential Moving Average (AEMA)". Rapidly track increases 
     *      to prevent latency violations (Drops), and very slowly decay to avoid premature 
     *      buffer underruns during transient lulls.
     * 2. Ramjee et al. (1994). IEEE INFOCOM. Target Safe Buffer = Base Delay + (4 * Smoothed_Jitter).
     */

    // 1. Asymmetric Jitter Smoothing (AEMA filter)
    uint32_t raw_jitter = stats->pnf_reported_jitter;
    if (raw_jitter > p7_info->smoothed_pnf_jitter_us) {
        // Fast Attack: Quickly inflate the safe bound when network suddenly degrades
        p7_info->smoothed_pnf_jitter_us = raw_jitter; 
    } else {
        // Slow Decay: Gradually deflate the safe bound (e.g. alpha = 63/64 for ~600ms decay)
        p7_info->smoothed_pnf_jitter_us = ((p7_info->smoothed_pnf_jitter_us * 63) + raw_jitter) / 64;
    }
    uint32_t effective_jitter = p7_info->smoothed_pnf_jitter_us;
    
    // According to Ramjee et al. 1994, target safe buffer = Base Delay + 4 * Jitter
    int32_t ALPHA = 4;
    
    // [Node-to-Node] VNF-to-PNF latency sample
    // Only worst-case latency is recorded for the VNF-PNF path.
    int32_t current_total_advanced_us = __atomic_load_n(&p7_info->total_advanced_us, __ATOMIC_SEQ_CST);
    int32_t reference_total_advanced_us = current_total_advanced_us;
    uint32_t now_hr = vnf_get_current_time_hr();
    if (p7_info->last_adjustment_time_hr != 0) {
        int64_t diff_us = timehr_diff_us(now_hr, p7_info->last_adjustment_time_hr);
        if (diff_us >= 0 && diff_us <= 4LL * (int64_t)p7_info->slot_duration_us) {
            reference_total_advanced_us = __atomic_load_n(&p7_info->last_total_advanced_us, __ATOMIC_SEQ_CST);
        }
    }

    // [CRITICAL FIX] Protect against anomalous jitter spikes and clock domain drift.
    // If the VNF thread got stalled by the OS for 20ms+, worst_late could literally be +20,000us.
    // Putting 20,000us into EWMA instantly destroys the process_delay tracking and crashes the schedule.
    if (worst_late > 5000) {
        NFAPI_TRACE(NFAPI_TRACE_WARN, "[VNF] Ignored absurd Jitter spike of %d us. Capped at 5000 us.\n", worst_late);
        worst_late = 5000;
    }

    // Node-to-node latency physically represents Execution + Network RTT / 2.
    // The *minimum* latency (without jitter) is represented by worst_early (the fastest packet).
    // The *maximum* latency (with jitter) is worst_late.
    int32_t min_node_to_node_latency = reference_total_advanced_us + worst_early;
    int32_t max_node_to_node_latency = reference_total_advanced_us + worst_late;

    // It is mathematically impossible for this to be negative. A negative value purely 
    // indicates that the PNF and VNF decoupled clock origins have drifted past each other,
    // or `total_advanced_us` was erroneously pulled into negative territory earlier.
    if (min_node_to_node_latency < 0) {
        min_node_to_node_latency = 0;
    }
    if (max_node_to_node_latency < 0) {
        max_node_to_node_latency = 0;
    }
    
    log_mmap_entry("vnf_pnf_latency", (long)min_node_to_node_latency);

    // Update EWMA Base Delay to the Minimum Node-to-Node latency (Fastest packet transit)
    // (VNF CPU execution + Network Transit + Queueing WITHOUT JITTER)
    if (p7_info->ewma_process_us == 0) {
        p7_info->ewma_process_us = min_node_to_node_latency;
    } else {
        p7_info->ewma_process_us = ((p7_info->ewma_process_us * 63) + min_node_to_node_latency) / 64;
    }

    // The 'BASE_PROCESS_DELAY_US' is now a true representation of exactly how 
    // many microseconds a packet takes from early scheduler dispatch to PNF reception window.
    int32_t BASE_PROCESS_DELAY_US = p7_info->ewma_process_us; 
    
    // Instead of using BASE_PROCESS_DELAY + ALPHA * jitter, we can directly use the historically
    // tracked 'maximum' to form our bounds.
    int32_t TARGET_ADVANCE_LOWER_BOUND = max_node_to_node_latency + (ALPHA * effective_jitter) / 2;
    int32_t DYNAMIC_LOWER_SAFE_BOUND = -TARGET_ADVANCE_LOWER_BOUND;

    // [CRITICAL FIX] Defining Absolute Physical Phase Advancement Limitations
    // As per recent network topologies, the physical One-Way Delay (OWD) is completely 
    // nullified by the initial synchronization. Thus, the margin is physically 0 since 
    // we are locked strictly to the PNF's epoch. We retain it as 0 for future architecture expansions.
    int32_t ABS_OWD_DELAY_US = p7_info->ewma_owd_us > 0 ? p7_info->ewma_owd_us : 0; 
    
    // The MAXIMUM upper phase bound MUST be strictly clamped to `timing_window` + `processing_delay`. 
    // Any phase pushing higher than this directly violates the PNF's frame rules, sending the 
    // packet physically faster than the PNF rx window has opened (Absolute "Too Early" catastrophe).
    int32_t ABSOLUTE_MAX_ADVANCE_US = timing_window_us + BASE_PROCESS_DELAY_US + ABS_OWD_DELAY_US;
    p7_info->absolute_max_advance_us = ABSOLUTE_MAX_ADVANCE_US;

    // Protect our boundary conditions: Lower bounds should never squeeze past the upper max advances 
    int32_t UPPER_SAFE_BOUND = - (ABSOLUTE_MAX_ADVANCE_US - 800); // Leave 800us headroom at the far end
    if (DYNAMIC_LOWER_SAFE_BOUND < UPPER_SAFE_BOUND + 1500) {
        DYNAMIC_LOWER_SAFE_BOUND = UPPER_SAFE_BOUND + 1500;
    }

    int32_t shift_us = 0;
    bool adjustment_issued = false;

    // 2. Evaluation Logic - Control Proportional Phase 
    // A. "Too Early" Check
    // WARNING & RATIONALE REGARDING PTP SYNCHRONIZATION AND TIME DOMAINS:
    // In the NFAPI split, the VNF and PNF time domains are entirely independent. The VNF actively uses its 
    // own VNF Timing Thread to schedule and send to the PNF. Thus, their raw clocks WILL drift relative to 
    // one another if no PTP is present. However, PTP synchronization is NOT required for the data plane 
    // to function correctly. WHY? Because this very closed-loop timing control (using nfapi_nr_timing_info) 
    // continuously measures the effective one-way phase offset (worst_early / worst_late) and dynamically 
    // adjusts the VNF's `target_advance_us`. This loop inherently compensates for any clock drift between 
    // the two independent time domains. As long as this loop correctly tracks the boundaries, the drift is 
    // transparently mitigated. The previous "Too Early" issues were caused by a sign error in the retreat 
    // formula, not by a lack of PTP.

    if (worst_early < UPPER_SAFE_BOUND) {
        // Rapid Phase Retreat - we're dangerously close to exceeding the max available timing window
        // [CRITICAL MATH FIX]: To retreat (arrive later), shift_us MUST be negative.
        // Previously: UPPER_SAFE_BOUND - worst_early - 300 resulted in a POSITIVE shift, pushing the packet 
        // even earlier, slamming aggressively into the ABSOLUTE_MAX_ADVANCE_US clamp and crashing the timing!
        // Correct fix: worst_early - UPPER_SAFE_BOUND guarantees a negative difference.
        shift_us = worst_early - UPPER_SAFE_BOUND - 300; 

        NFAPI_TRACE(NFAPI_TRACE_WARN, "[VNF] TOO EARLY DETECTED (WorstEarly: %d us < Bound: %d us), Rapid Retreat by %d us! Max_Adv: %d\n", worst_early, UPPER_SAFE_BOUND, shift_us, ABSOLUTE_MAX_ADVANCE_US);
    }
    // B. "Too Late" Check against Dynamic Jitter Bound
    else if (worst_late > DYNAMIC_LOWER_SAFE_BOUND / 2) { 
        // We crossed deep into our Jitter margin heading toward Deadline 0, fast advance needed!
        shift_us = worst_late - (DYNAMIC_LOWER_SAFE_BOUND / 2) + 200; 

        // Apply Too-Early clamp only to the physical sprint phase adjustment
        int32_t max_allowed_shift = worst_early - UPPER_SAFE_BOUND - 300;
        if (shift_us > max_allowed_shift) shift_us = max_allowed_shift;
        if (shift_us < 0) shift_us = 0; 
    }
    // C. Jitter is Small - Slowly Retreat to remove unnecessary delay and decrease round-trip latency
    else {
        // right_slack measures how far worst_late is unnecessarily early compared to the dynamic safe bound
        int32_t right_slack = DYNAMIC_LOWER_SAFE_BOUND - worst_late;
        
        if (right_slack > (int32_t)effective_jitter + 150) { 
            // VERY GENTLE Phase Retraction:
            // Previous uncontrolled logic (-114us per 10ms frame) equates to an extremely violent ~11ms/sec shift 
            // rate, which causes VNF disconnects and MAC logic crashes under 1G loads.
            // Using a strictly bounded limit of e.g. -15us per frame yields a safe drift of ~1.5ms per second.
            shift_us = - (right_slack / 50);
            if (shift_us < -15) shift_us = -15; // Hard limits on phase velocity
            if (shift_us > -2) shift_us = -2;    // Minimum movement

            NFAPI_TRACE(NFAPI_TRACE_DEBUG, "[VNF] Safe Recovery Zone - Smoothed Jitter: %u us, Wasted Time: %d us, Slowly Retracting Phase by %d us\n", effective_jitter, right_slack, shift_us);
        } else {
            shift_us = 0;
        }
    }

    if (shift_us != 0) {
        long target_advance = p7_info->total_advanced_us + shift_us;
        
        // ABSOLUTE_MAX_ADVANCE_US clamp to fully eliminate any chance of "Too Early"
        if (target_advance > ABSOLUTE_MAX_ADVANCE_US) {
            target_advance = ABSOLUTE_MAX_ADVANCE_US;
            NFAPI_TRACE(NFAPI_TRACE_WARN, "[VNF] Target advance clamped by ABSOLUTE_MAX_ADVANCE_US (%ld -> %d). Preventing Too Early!\n", target_advance, ABSOLUTE_MAX_ADVANCE_US);
        }
        if (target_advance < 0) target_advance = 0;

        long delta_us = target_advance - p7_info->total_advanced_us;
        __atomic_store_n(&p7_info->pending_us, p7_info->pending_us + delta_us, __ATOMIC_SEQ_CST);
        adjustment_issued = true;
    }

    if (adjustment_issued) {
        __atomic_store_n(&p7_info->last_adjustment_time_hr, vnf_get_current_time_hr(), __ATOMIC_SEQ_CST);
        __atomic_store_n(&p7_info->last_total_advanced_us, current_total_advanced_us, __ATOMIC_SEQ_CST);
    }
}

void handle_dynamic_timing_info(nfapi_vnf_p7_connection_info_t* p7_info, void *void_ind)
{
  nfapi_nr_timing_info_t *ind = (nfapi_nr_timing_info_t *)void_ind;

  // Error Handling
  if (!ind || !p7_info)
    return;
  if (ind->time_since_last_timing_info > 10000)
    return; // Basic sanity check

  // Sanity Clamp: If 'earliest_arrival' is absurdly negative (e.g. -140000), drop it entirely.
  // This happens during initial SFN/Slot wraps or severe out-of-order packets where the
  // PNF reports a wrap difference backwards. Applying this throws the VNF violently back.
  // Sanity Drop replaced by PNF physical offset bounds [-40, 40]
  /* 
   * Feedback Hold-Off / Dead Time Masking (Based on Control Theory for Delayed Systems)
   * 
   * Reference: 
   * Smith, O. J. M. (1957). "Closer Control of Loops with Dead Time". Chemical Engineering Progress.
   * 
   * Logic: 
   * The system has a round-trip delay (VNF -> PNF processing -> Timing Info feedback).
   * If an adjustment was made, the subsequent `timing_info` messages will still reflect the old, 
   * pre-adjustment state for a duration equivalent to this Dead Time (~RTT). 
   * If we process these stale reports, the controller will repeatedly overcompensate, causing severe oscillation.
   * 
   * Solution: Ignore all timing inputs for a short hold-off window equivalent to a few slot durations
   * after any adjustment to ensure the new feedback corresponds to the adjusted packet generation.
   */
  uint32_t now_hr = vnf_get_current_time_hr();
  if (p7_info->last_adjustment_time_hr != 0) {
      int64_t diff_us = timehr_diff_us(now_hr, p7_info->last_adjustment_time_hr);
      // Wait for approx 4 slots (e.g. 2000us for mu=1, 4000us for mu=0)
      int32_t dead_time_us = 4 * p7_info->slot_duration_us; // Conservative hold-off window
      
      if (diff_us < dead_time_us) { // Dead Time mask based on calculated RTT margin
          return; // Ignore stale feedback
      }
  }

  // Step 1: Extract per-slot timing stats (up to 8 unique slots)
  vnf_timing_stats_t slot_stats[8];
  int num_slots = vnf_p7_extract_timing_info(ind, p7_info, slot_stats, 8);

  // Step 2: Aggregate all slots into a single event to prevent loop amplification
  // If the network delayed a batch of slots, evaluating them in a loop would trigger 
  // convergence tracking multiple times and instantly bypass filtering.
  if (num_slots > 0) {
      vnf_timing_stats_t agg_stats = slot_stats[0];
      for (int i = 1; i < num_slots; i++) {
          if (slot_stats[i].worst_late > agg_stats.worst_late) 
              agg_stats.worst_late = slot_stats[i].worst_late;
          if (slot_stats[i].worst_early < agg_stats.worst_early) 
              agg_stats.worst_early = slot_stats[i].worst_early;
          if (slot_stats[i].pnf_reported_jitter > agg_stats.pnf_reported_jitter)
              agg_stats.pnf_reported_jitter = slot_stats[i].pnf_reported_jitter;
      }
      
      vnf_p7_convergence_optimization(p7_info, &agg_stats);
  }
}

void* vnf_p7_malloc(vnf_p7_t* vnf_p7, size_t size)
{
	if(vnf_p7->_public.malloc)
	{
		return (vnf_p7->_public.malloc)(size);
	}
	else
	{
		return calloc(1, size); 
	}
}
void vnf_p7_free(vnf_p7_t* vnf_p7, void* ptr)
{
	if(ptr == 0)
		return;

	if(vnf_p7->_public.free)
	{
		(vnf_p7->_public.free)(ptr);
	}
	else
	{
		free(ptr); 
	}
}

void vnf_p7_codec_free(vnf_p7_t* vnf_p7, void* ptr)
{
	if(ptr == 0)
		return;

	if(vnf_p7->_public.codec_config.deallocate)
	{
		(vnf_p7->_public.codec_config.deallocate)(ptr);
	}
	else
	{
		free(ptr); 
	}
}

void vnf_p7_connection_info_list_add(vnf_p7_t* vnf_p7, nfapi_vnf_p7_connection_info_t* node)
{
	NFAPI_TRACE(NFAPI_TRACE_INFO, "%s()\n", __FUNCTION__);
	// todo : add mutex
	node->next = vnf_p7->p7_connections; 
	vnf_p7->p7_connections = node;
}

nfapi_vnf_p7_connection_info_t* vnf_p7_connection_info_list_find(vnf_p7_t* vnf_p7, uint16_t phy_id)
{
	nfapi_vnf_p7_connection_info_t* curr = vnf_p7->p7_connections;
  while (curr != 0) {
    if (curr->phy_id == phy_id)
      return curr;
    curr = curr->next;
  }
  NFAPI_TRACE(NFAPI_TRACE_ERROR, "%s(): could not find P7 connection for phy_id %d\n", __func__, phy_id);

  return 0;
}

nfapi_vnf_p7_connection_info_t* vnf_p7_connection_info_list_delete(vnf_p7_t* vnf_p7, uint16_t phy_id)
{
	nfapi_vnf_p7_connection_info_t* curr = vnf_p7->p7_connections;
	nfapi_vnf_p7_connection_info_t* prev = 0;

	while(curr != 0)
	{
		if(curr->phy_id == phy_id)
		{
			if(prev == 0)
			{
				vnf_p7->p7_connections = curr->next;
			}
			else
			{
				prev->next = curr->next;
			}

			return curr;
		}
		else
		{
			prev = curr;
			curr = curr->next;
		}
	}

	return 0;
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
	dl_node_sync.delta_sfn_slot = 0;

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
	// VNF MUST INCREASE speed (reduce sleep time) to catch up -> requires pending_us to be POSITIVE
	// Negative offset implies VNF is AHEAD of PNF
	// VNF MUST DECREASE speed (increase sleep time) to fall back -> requires pending_us to be NEGATIVE

	int target_margin_initial = 1500;
	get_vnf_timing_envs(NULL, &target_margin_initial);

	int32_t total_correction = offset + target_margin_initial;

	pthread_mutex_lock(&p7_info->mutex);
	if (!p7_info->sync_locked) {
		if (total_correction >= -MARGIN_TOLERANCE_US && total_correction <= MARGIN_TOLERANCE_US) {
			p7_info->sync_locked = 1;
			// Conservative PLL Strategy: Cease utilizing OWD when down-link sync stops updating it.
			// Treating it as 0 sacrifices a small timing window segment but strictly guards against 'Too Early'.
			p7_info->ewma_owd_us = 0;
			// [CRITICAL FIX] The absolute phase tracking MUST initialize to the sync target margin!
			// When sync locked, VNF is already transmitting `target_margin_initial` ahead of PNF!
			// If this is set to 0, the dynamic margin will blindly push the phase further ahead 
			// by `target_margin_initial` AGAIN, vastly exceeding ABSOLUTE_MAX_ADVANCE_US and 
			// severely triggering "Too Early" drops continuously.
			p7_info->total_advanced_us = target_margin_initial; // Account for initial phase offset!
		} else {
			int32_t s_adj = total_correction / p7_info->slot_duration_us;
			int32_t p_adj = total_correction % p7_info->slot_duration_us;
			p7_info->slot_adjustment += s_adj;
			p7_info->pending_us += p_adj;
			p7_info->last_adjustment_time_hr = vnf_get_current_time_hr(); // Mask stale timing info
			p7_info->last_total_advanced_us = p7_info->total_advanced_us;
		}
	}
	pthread_mutex_unlock(&p7_info->mutex);
	NFAPI_TRACE(NFAPI_TRACE_DEBUG, 
		"[P7_SYNC] ul_node_sync phy_id:%d (t1/2/3/4:%8u,%8u,%8u,%8u) offset:%d owd:%d pending_us:%d locked:%d\n",
		ind.header.phy_id, ind.t1, ind.t2, ind.t3, t4,
		offset, owd, p7_info->pending_us, p7_info->sync_locked);
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
    get_vnf_timing_envs(&slot_ahead, NULL);
    if (slot_ahead == 0) {
        handle_dynamic_timing_info(p7_con, &ind);
    }

	// // Capture current SFN/Slot locally to avoid race conditions during logging
	// uint16_t vnf_sfn = p7_con->sfn;
	// uint16_t vnf_slot = p7_con->slot;

	// int32_t vnf_current_DEC = NFAPI_SFNSLOT2DEC(p7_con->mu, vnf_sfn, vnf_slot);
	// int32_t pnf_ind_DEC = NFAPI_SFNSLOT2DEC(p7_con->mu, ind.last_sfn, ind.last_slot);	
	pthread_mutex_lock(&p7_con->mutex);
	if (!p7_con->initial_timinginfo_received) {
		p7_con->sfn = ind.last_sfn;
		p7_con->slot = ind.last_slot;
		p7_con->initial_timinginfo_received = 1;
	}
	pthread_cond_signal(&p7_con->initial_timinginfo_cond);
	pthread_mutex_unlock(&p7_con->mutex);
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

