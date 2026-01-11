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
#include <stdio.h>
#include <assert.h>
#ifdef ENABLE_AERIAL
#include "nfapi/oai_integration/aerial/fapi_nvIPC.h"
#endif
#include "vnf_p7.h"
#ifdef ENABLE_WLS
#include <wls_integration/include/wls_vnf.h>
#endif
#include "nr_fapi_p7_utils.h"

#ifdef NDEBUG
#  warning assert is disabled
#endif

#define SYNC_CYCLE_COUNT 2

/* ============================================================================
 * DYNAMIC SLOT SLEEP TIMING CONTROL
 * ============================================================================ */

/* External mmap logger function - level 5 for slot sleep telemetry */
extern void log_mmap_entry(int log_id, int frame_tx, int slot_tx, const char *custom_message);

/* Global dynamic slot sleep array - stores per-slot sleep durations in microseconds */
uint32_t dynamic_slot_sleep_us[SLOT_ARRAY_SIZE];

void init_dynamic_slot_sleep(void)
{
  for (int i = 0; i < SLOT_ARRAY_SIZE; ++i) {
    dynamic_slot_sleep_us[i] = DEFAULT_SLOT_SLEEP_US;
  }
  NFAPI_TRACE(NFAPI_TRACE_INFO, "[TIMING] Initialized dynamic_slot_sleep_us[%d] to %u us\n",
              SLOT_ARRAY_SIZE, DEFAULT_SLOT_SLEEP_US);
}

void dump_slot_sleep_states(uint32_t current_slot)
{
  char buffer[512];
  int offset = 0;

  offset += snprintf(buffer + offset, sizeof(buffer) - offset, "m=%u|[", current_slot);

  for (int i = 0; i < SLOT_ARRAY_SIZE; ++i) {
    if (i > 0) {
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, ",");
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%u", dynamic_slot_sleep_us[i]);
  }

  snprintf(buffer + offset, sizeof(buffer) - offset, "]");

  log_mmap_entry(5, 0, 0, buffer);
}

/* Global Separated Timing Statistics */
vnf_timing_stats_t vnf_dl_stats;
vnf_timing_stats_t vnf_ul_stats;

void vnf_p7_extract_timing_info(const void* void_ind)
{
    const nfapi_nr_timing_info_t* ind = (const nfapi_nr_timing_info_t*)void_ind;
    
    // Reset stats for this pass
    memset(&vnf_dl_stats, 0, sizeof(vnf_dl_stats));
    memset(&vnf_ul_stats, 0, sizeof(vnf_ul_stats));
    
    // 1. Scope Calculation
    // Assuming mu=1 (0.5ms slots) -> slots = time(ms) * 2
    uint32_t calc_slots = ind->time_since_last_timing_info * 2;
    
    // Edge case: 0ms -> 1 slot (current)
    if (ind->time_since_last_timing_info == 0) {
        calc_slots = 1;
    }
    
    // Safety cap
    if (calc_slots > SLOT_ARRAY_SIZE) {
        calc_slots = SLOT_ARRAY_SIZE;
    }
    
    // 2. Iterate Backwards through covered slots
    uint32_t current_slot_idx = ind->last_slot;
    
    // Temporary tracking for max/min aggregation
    int32_t dl_max_late = -999999, dl_min_early = 999999, dl_jitter = 0;
    int32_t ul_max_late = -999999, ul_min_early = 999999, ul_jitter = 0;
    int dl_found = 0, ul_found = 0;

    for (uint32_t i = 0; i < calc_slots; ++i) {
        // Handle circular wrapping (39->0)
        // Correct logic: (current - i + SIZE) % SIZE
        int slot_idx = (current_slot_idx - i + SLOT_ARRAY_SIZE) % SLOT_ARRAY_SIZE;
        
        // 3. Classify Slot Type (DDDSU Pattern)
        // modulo 5 pattern: 0,1,2=D, 3=S(treat as U for stats), 4=U
        int pattern_idx = slot_idx % 5;
        char slot_type = 'X';
        
        if (pattern_idx <= 2) slot_type = 'D';
        else slot_type = 'U'; // S (3) and U (4) -> U context
        
        // Mark context active for valid data extraction
        if (slot_type == 'D') dl_found = 1;
        else ul_found = 1;
    }

    // 4. Data Extraction 
    // Optimization: We extract the "session" stats provided in the indication
    // The indication aggregates stats since last report. We assign them to the identified contexts.
    
    if (dl_found) {
        // Extract MAX late, MIN early, MAX jitter from DL types
        // DLTTI
        if (ind->dl_tti_jitter) dl_jitter = ind->dl_tti_jitter;
        if (ind->dl_tti_latest_delay > dl_max_late) dl_max_late = ind->dl_tti_latest_delay;
        if (ind->dl_tti_earliest_arrival < dl_min_early) dl_min_early = ind->dl_tti_earliest_arrival;
        
        // TXDATA 
        if (ind->tx_data_request_jitter > dl_jitter) dl_jitter = ind->tx_data_request_jitter;
        if (ind->tx_data_latest_delay > dl_max_late) dl_max_late = ind->tx_data_latest_delay;
        if (ind->tx_data_earliest_arrival < dl_min_early && ind->tx_data_earliest_arrival != 0) 
            dl_min_early = ind->tx_data_earliest_arrival;
            
        // Store
        vnf_dl_stats.max_late = dl_max_late;
        vnf_dl_stats.min_early = (dl_min_early == 999999) ? 0 : dl_min_early;
        vnf_dl_stats.jitter = dl_jitter;
        vnf_dl_stats.sample_count = 1; // Mark as valid
    }
    
    if (ul_found) {
        // ULTTI
        if (ind->ul_tti_jitter) ul_jitter = ind->ul_tti_jitter;
        if (ind->ul_tti_latest_delay > ul_max_late) ul_max_late = ind->ul_tti_latest_delay;
        if (ind->ul_tti_earliest_arrival < ul_min_early) ul_min_early = ind->ul_tti_earliest_arrival;

        // ULDCI
        if (ind->ul_dci_jitter > ul_jitter) ul_jitter = ind->ul_dci_jitter;
        if (ind->ul_dci_latest_delay > ul_max_late) ul_max_late = ind->ul_dci_latest_delay;
        if (ind->ul_dci_earliest_arrival < ul_min_early && ind->ul_dci_earliest_arrival != 0) 
             ul_min_early = ind->ul_dci_earliest_arrival;

        // Store
        vnf_ul_stats.max_late = ul_max_late;
        vnf_ul_stats.min_early = (ul_min_early == 999999) ? 0 : ul_min_early;
        vnf_ul_stats.jitter = ul_jitter;
        vnf_ul_stats.sample_count = 1;
    }
}


static void apply_backward_borrow(uint32_t start_index, int64_t amount)
{
    int64_t remaining = amount;
    for (int depth = 0; depth < MAX_BORROW_DEPTH; ++depth) {
        // Safe circular decrement: (start - depth + SIZE) % SIZE
        int idx = (start_index - depth + SLOT_ARRAY_SIZE) % SLOT_ARRAY_SIZE;
        int64_t current = (int64_t)dynamic_slot_sleep_us[idx];
        int64_t available = current - MIN_SLEEP_US;

        if (available <= 0) continue;

        int64_t take = (available < remaining) ? available : remaining;
        dynamic_slot_sleep_us[idx] = (uint32_t)(current - take);
        remaining -= take;

        NFAPI_TRACE(NFAPI_TRACE_DEBUG, "[TIMING] Borrowed %ld us from slot %d (Depth %d)\n", take, idx, depth);

        if (remaining == 0) break;
    }
}

static void apply_forward_lending(uint32_t start_index, int64_t amount)
{
    int64_t remaining = amount;
    for (int depth = 0; depth < MAX_BORROW_DEPTH; ++depth) {
        int idx = (start_index + depth) % SLOT_ARRAY_SIZE;
        int64_t current = (int64_t)dynamic_slot_sleep_us[idx];
        int64_t capacity = MAX_SLEEP_US - current;

        if (capacity <= 0) continue;

        int64_t give = (capacity < remaining) ? capacity : remaining;
        dynamic_slot_sleep_us[idx] = (uint32_t)(current + give);
        remaining -= give;

        NFAPI_TRACE(NFAPI_TRACE_DEBUG, "[TIMING] Lent %ld us to slot %d (Depth %d)\n", give, idx, depth);

        if (remaining == 0) break;
    }
}

static void apply_backward_lending(uint32_t start_index, int64_t amount)
{
    int64_t remaining = amount;
    for (int depth = 0; depth < MAX_BORROW_DEPTH; ++depth) {
        // Safe circular decrement: (start - depth + SIZE) % SIZE
        int idx = (start_index - depth + SLOT_ARRAY_SIZE) % SLOT_ARRAY_SIZE;
        int64_t current = (int64_t)dynamic_slot_sleep_us[idx];
        int64_t capacity = MAX_SLEEP_US - current;

        if (capacity <= 0) continue;

        int64_t give = (capacity < remaining) ? capacity : remaining;
        dynamic_slot_sleep_us[idx] = (uint32_t)(current + give);
        remaining -= give;

        NFAPI_TRACE(NFAPI_TRACE_DEBUG, "[TIMING] Lent %ld us to backward slot %d (Depth %d)\n", give, idx, depth);

        if (remaining == 0) break;
    }
}


int64_t vnf_p7_critical_correction(uint32_t current_slot, int is_dl)
{
    int64_t pass2_correction = 0;
    
    // Select Context Stats
    int32_t max_late = is_dl ? vnf_dl_stats.max_late : vnf_ul_stats.max_late;
    int32_t min_early = is_dl ? vnf_dl_stats.min_early : vnf_ul_stats.min_early;
    
    // Trigger Conditions
    int trigger_late = (max_late > 0);
    
    // CRITICAL FIX: Only increase sleep if we're TRULY safe:
    // 1. No late packets at all (max_late <= 0, meaning arrival was on-time or early)
    // 2. AND we have excessive early margin (min_early < -400us, double the target)
    // 3. AND jitter is low (stable conditions)
    #define TIMING_WINDOW_US 2200  // Increased threshold: only act on very early
    uint32_t jitter = is_dl ? vnf_dl_stats.jitter : vnf_ul_stats.jitter;
    int trigger_early = (min_early < -TIMING_WINDOW_US) && (max_late <= 0) && (jitter < JITTER_THRESHOLD_US);

    if (!trigger_late && !trigger_early) {
        return 0;
    }

    int64_t current_sleep = (int64_t)dynamic_slot_sleep_us[current_slot];

    // --- CASE A: CRITICAL LATE (Insufficient Funds) ---
    if (trigger_late) {
        // Step 1: Calculate Penalty (1.02x - less aggressive)
        int64_t penalty = ((int64_t)(max_late + TARGET_MARGIN_US) * 51) / 50;
        
        // Step 2: Check Available Funds
        int64_t available = current_sleep - MIN_SLEEP_US;
        
        if (available > penalty) {
            // Scenario A1: Solvent
            int64_t new_sleep = current_sleep - penalty;
            // Ensure clamp (redundant due to 'available' check but safe)
            if (new_sleep < MIN_SLEEP_US) new_sleep = MIN_SLEEP_US; 
            
            dynamic_slot_sleep_us[current_slot] = (uint32_t)new_sleep;
            pass2_correction = -penalty; // Negative for reduction
        } else {
            // Scenario A2: Bankrupt
            dynamic_slot_sleep_us[current_slot] = MIN_SLEEP_US;
            pass2_correction = -available; // We only gave what we had
            
            int64_t deficit = penalty - available;
            int64_t borrow_request = (deficit * 9) / 10; // 0.9 damping
            
            if (borrow_request > 0) {
                // Borrow from previous slots (circular), skip current_slot
                uint32_t prev_slot = (current_slot - 1 + SLOT_ARRAY_SIZE) % SLOT_ARRAY_SIZE;
                apply_backward_borrow(prev_slot, borrow_request);
            }
        }
    }
    // --- CASE B: CRITICAL EARLY (Excess Funds) ---
    // NOTE: This only triggers when truly safe (no late packets, low jitter)
    else if (trigger_early) {
        // Step 1: Calculate Required Boost (more conservative)
        int64_t abs_min_early = llabs((int64_t)min_early);
        // Reduce boost by 50% to be conservative
        int64_t boost = ((abs_min_early - TIMING_WINDOW_US) + TARGET_MARGIN_US) / 2;
        
        // Step 2: Calculate Ideal Sleep
        int64_t ideal_sleep = current_sleep + boost;
        
        // Step 3: Check Capacity
        if (ideal_sleep <= MAX_SLEEP_US) {
            // Scenario B1: Within Capacity
            dynamic_slot_sleep_us[current_slot] = (uint32_t)ideal_sleep;
            pass2_correction = boost;
        } else {
            // Scenario B2: Overflow
            dynamic_slot_sleep_us[current_slot] = MAX_SLEEP_US;
            pass2_correction = MAX_SLEEP_US - current_sleep;
            
            int64_t surplus = ideal_sleep - MAX_SLEEP_US;
            int64_t lend_offer = (surplus * 9) / 10; // 0.9 damping
            
            if (lend_offer > 0) {
                // Lend to future slots (circular), skip current_slot
                uint32_t prev_slot = (current_slot - 1 + SLOT_ARRAY_SIZE) % SLOT_ARRAY_SIZE;
                apply_backward_lending(prev_slot, lend_offer);
            }
        }
    }
    
    return pass2_correction;
}

// Pass 3: Convergence Optimization / Fine-tuning
// Adjusts sleep to maintain optimal fleet margin and reduce jitter
void vnf_p7_convergence_optimization(const void* void_ind, int64_t pass2_correction, uint32_t nominal_slot_duration_us)
{
    const nfapi_nr_timing_info_t* ind = (const nfapi_nr_timing_info_t*)void_ind;
    
    // 0. Scope & Skip Logic
    // Re-calculate covered slots (same logic as Pass 1)
    uint32_t calc_slots = ind->time_since_last_timing_info * 2;
    if (ind->time_since_last_timing_info == 0) calc_slots = 1;
    if (calc_slots > SLOT_ARRAY_SIZE) calc_slots = SLOT_ARRAY_SIZE;
    
    // Skip Condition: Single point and stable max/min
    if (calc_slots == 1 &&
        vnf_dl_stats.max_late == vnf_dl_stats.min_early &&
        vnf_ul_stats.max_late == vnf_ul_stats.min_early) {
        return; 
    }
    
    uint32_t current_slot_idx = ind->last_slot;

    // --- PHASE 1: Deviation Calculation ---
    int64_t total_dev_dl = 0;
    int count_dl = 0;
    int64_t total_dev_ul = 0;
    int count_ul = 0;

    for (uint32_t i = 0; i < calc_slots; ++i) {
        int idx = (current_slot_idx - i + SLOT_ARRAY_SIZE) % SLOT_ARRAY_SIZE;
        int pattern_idx = idx % 5;
        char slot_type = (pattern_idx <= 2) ? 'D' : 'U';
        
        int64_t deviation = llabs((int64_t)dynamic_slot_sleep_us[idx] - 500);
        
        if (slot_type == 'D') {
            total_dev_dl += deviation;
            count_dl++;
        } else {
            total_dev_ul += deviation;
            count_ul++;
        }
    }
    
    // Safety check for divide-by-zero
    if (total_dev_dl == 0) total_dev_dl = 1;
    if (total_dev_ul == 0) total_dev_ul = 1;
    
    // --- PHASE 2: Adjustment ---
    for (uint32_t i = 0; i < calc_slots; ++i) {
        int k = (current_slot_idx - i + SLOT_ARRAY_SIZE) % SLOT_ARRAY_SIZE;
        
        // Cooldown Rule: Skip current slot if Pass 2 modified it
        if (k == current_slot_idx && pass2_correction != 0) {
            continue; 
        }

        int pattern_idx = k % 5;
        char slot_type = (pattern_idx <= 2) ? 'D' : 'U';
        int64_t current_sleep = (int64_t)dynamic_slot_sleep_us[k];
        int64_t new_sleep = current_sleep;
        
        if (slot_type == 'D') {
            // --- DL LOGIC ---
            // PRIORITY 1: Panic Brake for Runaway Early condition.
            // If we are massively early (e.g. > 5ms), we MUST brake, regardless of any "Late" noise.
            // "Late" indications when we are 1 second early are likely measurement artifacts or jitter.
            if (vnf_dl_stats.min_early > 5000) {
                new_sleep = MAX_SLEEP_US;
            }
            // PRIORITY 2: Late Correction
            // Only correct for Late if we are not "Very Early" (e.g. > Target + 400us),
            // OR if the Late amount is significant (> 50us) and demands attention despite being early.
            else if (vnf_dl_stats.max_late > 0 && 
                    (vnf_dl_stats.min_early < (TARGET_MARGIN_US + 400) || vnf_dl_stats.max_late > 50)) {
                // LATE: Need to REDUCE sleep to wake up earlier
                if (vnf_dl_stats.jitter < JITTER_THRESHOLD_US || count_dl <= 1) {
                    int64_t adjustment = (int64_t)vnf_dl_stats.max_late / 10;
                    if (adjustment > MAX_PASS3_ADJUST_US) adjustment = MAX_PASS3_ADJUST_US;
                    new_sleep = current_sleep - adjustment;
                } else {
                    int64_t deviation_k = llabs(current_sleep - (int64_t)nominal_slot_duration_us);
                    int64_t weight = (deviation_k * 100) / total_dev_dl;
                    int64_t adjustment = ((int64_t)vnf_dl_stats.max_late * weight * 5) / 1000;
                    if (adjustment > MAX_PASS3_ADJUST_US) adjustment = MAX_PASS3_ADJUST_US;
                    new_sleep = current_sleep - adjustment;
                }
            } 
            // PRIORITY 3: Standard Early Decay
            else if (vnf_dl_stats.min_early > (TARGET_MARGIN_US + MARGIN_TOLERANCE_US)) {
                // TOO EARLY: Must INCREASE sleep
                int64_t excess = vnf_dl_stats.min_early - TARGET_MARGIN_US;
                
                // Standard Decay
                int64_t adjustment = excess / 10;
                if (adjustment > MAX_PASS3_ADJUST_US) adjustment = MAX_PASS3_ADJUST_US;
                if (adjustment < 5) adjustment = 5;
                new_sleep = current_sleep + adjustment;
                
                // ANTI-RUNAWAY: If we are Early, we must NEVER sleep less than Nominal.
                // Sleeping less than nominal accelerates time relative to PNF.
                if (new_sleep < (int64_t)nominal_slot_duration_us) {
                    new_sleep = (int64_t)nominal_slot_duration_us;
                }
            }
        } else {
            // --- UL LOGIC ---
            // PRIORITY 1: Panic Brake
            if (vnf_ul_stats.min_early > 5000) {
                new_sleep = MAX_SLEEP_US;
            }
            // PRIORITY 2: Late Logic
            else if (vnf_ul_stats.max_late > 0 && 
                    (vnf_ul_stats.min_early < (TARGET_MARGIN_US + 400) || vnf_ul_stats.max_late > 50)) {
                // LATE: Need to REDUCE sleep to wake up earlier
                if (vnf_ul_stats.jitter < JITTER_THRESHOLD_US || count_ul <= 1) {
                    int64_t adjustment = (int64_t)vnf_ul_stats.max_late / 10;
                    if (adjustment > MAX_PASS3_ADJUST_US) adjustment = MAX_PASS3_ADJUST_US;
                    new_sleep = current_sleep - adjustment;
                } else {
                    int64_t deviation_k = llabs(current_sleep - (int64_t)nominal_slot_duration_us);
                    int64_t weight = (deviation_k * 100) / total_dev_ul;
                    int64_t adjustment = ((int64_t)vnf_ul_stats.max_late * weight * 5) / 1000;
                    if (adjustment > MAX_PASS3_ADJUST_US) adjustment = MAX_PASS3_ADJUST_US;
                    new_sleep = current_sleep - adjustment;
                }
            } 
            // PRIORITY 3: Standard Early Decay
            else if (vnf_ul_stats.min_early > (TARGET_MARGIN_US + MARGIN_TOLERANCE_US)) {
                // TOO EARLY: Must INCREASE sleep
                int64_t excess = vnf_ul_stats.min_early - TARGET_MARGIN_US;
                
                // Standard Decay
                int64_t adjustment = excess / 10;
                if (adjustment > MAX_PASS3_ADJUST_US) adjustment = MAX_PASS3_ADJUST_US;
                
                new_sleep = current_sleep + adjustment;
                
                // ANTI-RUNAWAY
                if (new_sleep < (int64_t)nominal_slot_duration_us) {
                        new_sleep = (int64_t)nominal_slot_duration_us;
                }
            }
        }
        
        // Clamp
        if (new_sleep < MIN_SLEEP_US) new_sleep = MIN_SLEEP_US;
        if (new_sleep > MAX_SLEEP_US) new_sleep = MAX_SLEEP_US;
        
        dynamic_slot_sleep_us[k] = (uint32_t)new_sleep;
    }
}

void handle_dynamic_timing_info(void *void_ind, uint32_t current_slot, uint32_t nominal_slot_duration_us, const char *slot_pattern)
{
    nfapi_nr_timing_info_t *ind = (nfapi_nr_timing_info_t *)void_ind;
    
    // Lazy initialization - ensure array is initialized on first call
    static int initialized = 0;
    static uint32_t last_nominal = 0;
    if (!initialized || (nominal_slot_duration_us != last_nominal && nominal_slot_duration_us > 0)) {
        // Initialize with the provided nominal duration
        for (int i = 0; i < SLOT_ARRAY_SIZE; i++) {
            dynamic_slot_sleep_us[i] = (nominal_slot_duration_us > 0) ? nominal_slot_duration_us : 500;
        }
        initialized = 1;
        last_nominal = nominal_slot_duration_us;
        NFAPI_TRACE(NFAPI_TRACE_INFO, "[TIMING] Initialized dynamic_slot_sleep_us with nominal %u us\n", last_nominal);
    }
    
    // Error Handling
    if (!ind || !slot_pattern) return;
    if (current_slot >= SLOT_ARRAY_SIZE) return;
    if (ind->time_since_last_timing_info > 10000) return; // Basic sanity check

    // Step 1: Extract Data (Pass 1)
    vnf_p7_extract_timing_info(ind);

    // Step 2: Determine Current Context
    int pattern_len = strlen(slot_pattern); // Likely 5 for "DDDSU"
    if (pattern_len == 0) pattern_len = 5; 
    
    int pattern_idx = current_slot % pattern_len;
    char slot_type = slot_pattern[pattern_idx];
    int is_dl = (slot_type == 'D');
    
    // Step 3: Execute Pass 2 (Emergency Correction)
    int64_t pass2_correction = vnf_p7_critical_correction(current_slot, is_dl);

    // Step 4: Execute Pass 3 (Fine-tuning)
    // vnf_p7_convergence_optimization(ind, pass2_correction, nominal_slot_duration_us);
    
    // Step 5: RECOVERY MECHANISM - When mostly stable, recover toward baseline
    // Allow recovery if late is minor (< 50us) - don't wait for perfect 0
    int32_t max_late = is_dl ? vnf_dl_stats.max_late : vnf_ul_stats.max_late;
    if (pass2_correction == 0 && max_late < 50) {
        int64_t current_sleep = (int64_t)dynamic_slot_sleep_us[current_slot];
        if (current_sleep < DEFAULT_SLOT_SLEEP_US) {
            // Faster recovery: +20us per cycle toward baseline
            int64_t new_sleep = current_sleep + 20;
            if (new_sleep > DEFAULT_SLOT_SLEEP_US) new_sleep = DEFAULT_SLOT_SLEEP_US;
            dynamic_slot_sleep_us[current_slot] = (uint32_t)new_sleep;
        }
    }

    // Step 6: Dump Telemetry
    dump_slot_sleep_states(current_slot);
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

    }

    int32_t t4 = calculate_nr_t4(now_time_hr, p7_info->mu, p7_info->sfn, p7_info->slot, vnf_p7->slot_start_time_hr);
    
    // Calculate offset using int64_t for proper handling of large values
    // formula: offset = ((t2 - t1) - (t4 - t3)) / 2
    // Positive offset means VNF clock is BEHIND PNF (VNF needs to speed up / reduce delay)
    // Negative offset means VNF clock is AHEAD of PNF (VNF needs to slow down / add delay)
    int32_t offset = (int32_t)( ((int64_t)ind.t2 - (int64_t)ind.t1 - ((int64_t)t4 - (int64_t)ind.t3)) / 2 );
    int32_t owd = (int32_t)( ((int64_t)t4 - (int64_t)ind.t1 - ((int64_t)ind.t3 - (int64_t)ind.t2)) / 2 );
    
	int32_t TARGET_PNF_MARGIN_US = 150*(1 << p7_info->mu); // 500us for mu0, 1000us for mu1, 2000us for mu2, 4000us for mu3
    int32_t slot_us = (int32_t)p7_info->slot_duration_us;
    
	// CRITICAL: Negate the adjustment direction!
    // If offset < 0, VNF is ahead -> we need to ADD delay (positive adjustment to next_slot_time)
    // If offset > 0, VNF is behind -> we need to REDUCE delay (negative adjustment)
    // So: us_adjustment = -offset
	int32_t offsetslot = (offset + TARGET_PNF_MARGIN_US) / slot_us;
	int32_t offsetus = (offset  + TARGET_PNF_MARGIN_US) % slot_us;
	
    // Check if sync has converged (offset within ±10) - once locked, permanently stop adjusting
    pthread_mutex_lock(&p7_info->mutex);
    if (!p7_info->sync_locked) {
        if (offset + TARGET_PNF_MARGIN_US >= -10 && offset + TARGET_PNF_MARGIN_US <= 10) {
            // Offset converged within ±10, permanently lock sync and stop adjustments
            p7_info->sync_locked = 1;
            p7_info->us_adjustment = 0;
            p7_info->slot_adjustment = 0;
            NFAPI_TRACE(NFAPI_TRACE_INFO, 
                "[P7_SYNC] SYNC LOCKED! phy_id:%d offset:%d within ±10, permanently stopping adjustments\n",
                ind.header.phy_id, offset);
        } else {
            // Still converging, apply adjustments
            p7_info->us_adjustment = -offsetus;
            p7_info->slot_adjustment = offsetslot;
        }
    }
    pthread_mutex_unlock(&p7_info->mutex);

	char print_info[128];
    snprintf(print_info, sizeof(print_info), "offset=%d, owd=%d, t(%8u,%8u,%8u,%8u) slot_adj:%d us_adj:%d", offset, owd, ind.t1, ind.t2, ind.t3, t4, p7_info->slot_adjustment, p7_info->us_adjustment);
    log_mmap_entry(3, p7_info->sfn , p7_info->slot , print_info);	
    NFAPI_TRACE(NFAPI_TRACE_DEBUG, 
        "[P7_SYNC] ul_node_sync phy_id:%d (t1/2/3/4:%8u,%8u,%8u,%8u) offset:%d owd:%d slot_adj:%d us_adj:%d locked:%d\n",
        ind.header.phy_id, ind.t1, ind.t2, ind.t3, t4,
        offset, owd, p7_info->slot_adjustment, p7_info->us_adjustment, p7_info->sync_locked);
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

	// Integration Step (Prompt 5)
	handle_dynamic_timing_info(&ind, p7_con->slot, p7_con->slot_duration_us, "DDDSU");

	int32_t vnf_current_DEC = NFAPI_SFNSLOT2DEC(p7_con->mu, p7_con->sfn, p7_con->slot);
	int32_t pnf_ind_DEC = NFAPI_SFNSLOT2DEC(p7_con->mu, ind.last_sfn, ind.last_slot);

	// Only print if any jitter/delay/arrival value is non-zero
	if (
		ind.dl_tti_jitter != 0 ||
		ind.tx_data_request_jitter != 0 ||
		ind.ul_tti_jitter != 0 ||
		ind.ul_dci_jitter != 0 ||
		ind.dl_tti_latest_delay != 0 ||
		ind.tx_data_latest_delay != 0 ||
		ind.ul_tti_latest_delay != 0 ||
		ind.ul_dci_latest_delay != 0 ||
		ind.dl_tti_earliest_arrival != 0 ||
		ind.tx_data_earliest_arrival != 0 ||
		ind.ul_tti_earliest_arrival != 0 ||
		ind.ul_dci_earliest_arrival != 0
	) {
		char print_info[256];
		snprintf(print_info, sizeof(print_info), "ds=%d, last=%u, jitter(dl:%u,tx:%u,ul:%u,dci:%u) delay(dl:%d,tx:%d,ul:%d,dci:%d) early(dl:%d,tx:%d,ul:%d,dci:%d)",
			pnf_ind_DEC - vnf_current_DEC,
			ind.time_since_last_timing_info,
			ind.dl_tti_jitter,
			ind.tx_data_request_jitter,
			ind.ul_tti_jitter,
			ind.ul_dci_jitter,
			ind.dl_tti_latest_delay,
			ind.tx_data_latest_delay,
			ind.ul_tti_latest_delay,
			ind.ul_dci_latest_delay,
			ind.dl_tti_earliest_arrival,
			ind.tx_data_earliest_arrival,
			ind.ul_tti_earliest_arrival,
			ind.ul_dci_earliest_arrival
		);
		log_mmap_entry(2, p7_con->sfn , p7_con->slot , print_info);

		NFAPI_TRACE(NFAPI_TRACE_DEBUG,
			"NR_TIMING_INFO: PNF:%u.%u VNF:%u.%u delta_slots=%d time_since_last=%u jitter(dl:%u,tx:%u,ul:%u,dci:%u) latest_delay(dl:%d,tx:%d,ul:%d,dci:%d) earliest_arr(dl:%d,tx:%d,ul:%d,dci:%d)\n",
			ind.last_sfn,
			ind.last_slot,
			p7_con->sfn,
			p7_con->slot,
			pnf_ind_DEC - vnf_current_DEC,
			ind.time_since_last_timing_info,
			ind.dl_tti_jitter,
			ind.tx_data_request_jitter,
			ind.ul_tti_jitter,
			ind.ul_dci_jitter,
			ind.dl_tti_latest_delay,
			ind.tx_data_latest_delay,
			ind.ul_tti_latest_delay,
			ind.ul_dci_latest_delay,
			ind.dl_tti_earliest_arrival,
			ind.tx_data_earliest_arrival,
			ind.ul_tti_earliest_arrival,
			ind.ul_dci_earliest_arrival
		);
	}		
		p7_con->initial_timinginfo_received = 1; 
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

