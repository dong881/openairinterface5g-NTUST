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


#ifndef _VNF_P7_H_
#define _VNF_P7_H_

#include "nfapi_vnf_interface.h"
#include <stdatomic.h>
#include <stdbool.h>
#define TIMEHR_SEC(_time_hr) ((uint32_t)(_time_hr) >> 20)
#define TIMEHR_USEC(_time_hr) ((uint32_t)(_time_hr) & 0xFFFFF)
#define TIME2TIMEHR(_time) (((uint32_t)(_time.tv_sec) & 0xFFF) << 20 | ((uint32_t)(_time.tv_usec) & 0xFFFFF))
/* ============================================================================
 * DYNAMIC SLOT SLEEP TIMING CONTROL CONSTANTS
 * ============================================================================ */
/* Dynamic Target Margin (adaptive to avoid late packets) */
#define MARGIN_TOLERANCE_US         20    // Initial deadband zone used for first synchronization
#define MARGIN_TOLERANCE_LOCKED_US 100    // Wider deadband zone used after first sync lock
#define SLOT_ARRAY_SIZE             20    // TDD cycle slot count (Reduced to 20 for faster convergence)

/*
 * get_vnf_timing_envs():
 *   Read runtime NFAPI timing configuration from environment variables.
 *
 *   SLOT_AHEAD
 *     - Controls the initial slot-ahead value used by the VNF.
 *     - Example: export SLOT_AHEAD=6
 *     - When dynamic timing is disabled, this value is treated as a fixed
 *       slot-ahead offset.
 *     - When dynamic timing is enabled, this value is used only as the initial
 *       slot-ahead starting point.
 *
 *   DYNAMIC_TIMING
 *     - If set to a nonzero value, enable dynamic timing adjustment.
 *     - Example: export DYNAMIC_TIMING=1
 *     - This allows dynamic timing to run from any initial SLOT_AHEAD value.
 *
 *   General behavior:
 *     - Fixed mode: SLOT_AHEAD > 0 and DYNAMIC_TIMING is unset.
 *       The VNF uses the fixed slot-ahead value and skips dynamic timing.
 *     - Dynamic mode: DYNAMIC_TIMING=1 or SLOT_AHEAD == 0.
 *       The VNF applies dynamic timing adjustment, with SLOT_AHEAD providing
 *       the initial slot-ahead start point.
 *
 *   IMPORTANT USAGE NOTE (sudo):
 *     - When running the softmodem with `sudo`, regular exported environment
 *       variables are NOT passed to the executed process by default.
 *     - To fix this, you must either use `sudo -E` to preserve environment,
 *       or pass the variable inline with the command:
 *       `sudo DYNAMIC_TIMING=1 SLOT_AHEAD=2 TIMING_WINDOW=3000 ./nr-softmodem ...`
 *
 *   The function fills the caller-provided pointers and keeps all
 *   timing behavior local to the caller scope, without global state.
 */
static inline void get_vnf_timing_envs(int *slot_ahead, bool *dynamic_timing_enabled) {
    const char *slot_ahead_env = getenv("SLOT_AHEAD");
    int env_slot = slot_ahead_env ? atoi(slot_ahead_env) : 0;
    const char *dynamic_env = getenv("DYNAMIC_TIMING");
    bool env_dynamic = false;

    if (dynamic_env && atoi(dynamic_env) != 0) {
        env_dynamic = true;
    }
    if (env_slot == 0) {
        env_dynamic = true;
    }

    if (slot_ahead) *slot_ahead = env_slot;
    if (dynamic_timing_enabled) *dynamic_timing_enabled = env_dynamic;
}

typedef struct {
	uint8_t* buffer;
	uint32_t length;
} vnf_p7_rx_message_segment_t;

typedef struct vnf_p7_rx_message vnf_p7_rx_message_t;

typedef struct vnf_p7_rx_message {
	uint8_t sequence_number;
	uint8_t num_segments_received;
	uint8_t num_segments_expected;

	// the spec allows of upto 128 segments, this does seem excessive
	vnf_p7_rx_message_segment_t segments[128];

	uint32_t rx_hr_time;

	vnf_p7_rx_message_t* next;
} vnf_p7_rx_message_t;

typedef struct {

	vnf_p7_rx_message_t* msg_queue;

} vnf_p7_rx_reassembly_queue_t;


#define P7_Q10_ONE 1024

/*
 * Runtime-learned / baseline-provided reference.
 *
 * These values must be filled from:
 *   - fixed-slot baseline experiment,
 *   - runtime config,
 *   - previous known-good learned state.
 *
 * Do NOT hardcode "4 slots" or a static margin threshold in controller.
 */
typedef struct p7_sync_reference_profile_s {
    /*
     * Baseline slot-ahead that produced best known result.
     * Example: fixed 4 slots ahead may set this to 4 externally.
     */
    int32_t reference_s_ahead;

    /*
     * Baseline effective latency in us.
     * This should come from the same comparison metric used in plots.
     */
    int32_t reference_latency_us;

    /*
     * Baseline PNF margin floor in us.
     * Recommended source:
     *   margin_floor = margin_ewma - margin_dev
     * from fixed baseline run.
     */
    int32_t reference_margin_floor_us;
} p7_sync_reference_profile_t;

/*
 * Runtime PNF/MAC feedback collected during previous control period.
 *
 * These must be updated by collectors before p7_run_ewma_lab_control().
 */
typedef struct p7_sync_runtime_feedback_s {
    /*
     * PNF timing margin statistics from check_nr_p7_timing().
     * margin = delay_to_msg_slot - time_since_slot_start.
     */
    int32_t recent_pnf_margin_min_us;
    int32_t recent_pnf_margin_ewma_us;
    int32_t recent_pnf_margin_dev_us;

    /*
     * PNF timing failures.
     */
    int32_t recent_pnf_too_late_max_us;
    int32_t recent_pnf_too_late_count;
    int32_t recent_pnf_no_tx_data_count;

    /*
     * MAC reliability symptoms.
     */
    int32_t recent_mac_harq_feedback_timeout_count;
    int32_t recent_mac_retx_abort_count;

    /*
     * Effective latency estimator used to compare against baseline.
     * Unit: us.
     */
    int32_t recent_effective_latency_ewma_us;
} p7_sync_runtime_feedback_t;

#define P7_EWMA_LAB_MAX_STATES        16
#define P7_EWMA_LAB_MAX_LOAD_PROFILES 8
#define P7_EWMA_LAB_Q10               1024

typedef struct {
    int32_t valid;

    int32_t sample_count;

    /*
     * EWMA probability of late sample.
     * Q10 scale:
     *   0    = no recent late
     *   1024 = always late
     */
    int32_t late_ewma_q10;

    int32_t risk_ewma_us;
    int32_t latency_cost_ewma_us;
    int32_t jitter_ewma_us;

    /*
     * Consecutive samples that were safe under this s_ahead.
     */
    int32_t consecutive_safe_count;
} p7_ewma_lab_state_score_t;

typedef struct {
    int32_t valid;

    /*
     * Learned load region.
     * Unit should match recent_p7_msg_count / recent_msg_per_slot.
     */
    int32_t load_center;
    int32_t load_ewma;
    int32_t load_dev;

    /*
     * Learned best s_ahead for this load region.
     * 0 means not learned yet.
     */
    int32_t learned_best_s_ahead;

    p7_ewma_lab_state_score_t state[P7_EWMA_LAB_MAX_STATES + 1];
} p7_ewma_lab_load_profile_t;
typedef struct nfapi_vnf_p7_connection_info {

	/*! The PHY id */
	int phy_id;


	// this does not belong here...
	uint8_t stream_id;

	/*! Flag indicating the sync state of the P7 conenction */
	uint8_t in_sync;

	int dl_out_sync_offset; 
	int dl_out_sync_period; // ms (as a pow2)

	int dl_in_sync_offset; 
	int dl_in_sync_period; // ms (as a pow2)

	uint8_t filtered_adjust;
	uint16_t min_sync_cycle_count;
	uint32_t latency[8];
	uint32_t average_latency;
	int32_t sf_offset_filtered;
	int32_t sf_offset_trend;
	int32_t sf_offset;
	int32_t slot_offset;
	int32_t slot_offset_trend;
	int32_t slot_offset_filtered;
	uint16_t zero_count;
	int32_t adjustment;
	int32_t slot_adjustment;
	int32_t us_adjustment;
	int32_t insync_minor_adjustment;
	int32_t insync_minor_adjustment_duration;
	uint8_t sync_locked;  // Flag: once offset converges within ±10, permanently stop adjusting
	/* Periodic sync control */
	uint32_t sync_slot_counter;                // Counter for periodic sync
	uint32_t sync_period_slots;                // Period between syncs (configurable)

	/* Dynamic Timing Adjustment State */
	int32_t convergence_count;
	int32_t estimated_mean_late;      // Jacobson/Karels estimated mean delay
	int32_t estimated_jitter_var;     // Jacobson/Karels estimated jitter variance
	int32_t late_jitter;            // Separate EWMA for late jitter
	int32_t early_jitter;           // Separate EWMA for early jitter
	uint32_t last_adjustment_time_hr; // Time of last adjustment (for Dead Time / RTT masking)
	int32_t long_ewma_process_us;
	int32_t short_ewma_process_us;
	int32_t ewma_process_us;
	int32_t ewma_owd_us;

	/* Percentile and PID State */
	int32_t delay_history[128];
	uint32_t delay_history_idx;
	uint32_t delay_history_count;
	int32_t pid_integral_us;
	int32_t pid_prev_error_us;
	int32_t min_owd_us;
	uint32_t min_owd_timestamp_hr;
	uint32_t peak_latency_timestamp_hr;
	int32_t consecutive_late_spikes;
	int32_t panic_extension_slots;
	int32_t consecutive_panic_spikes;
	int32_t stable_top_pending_drop;
	int32_t reduction_penalty_counter;
	uint32_t last_increase_timestamp_hr;

	int32_t total_advanced_us; // Absolute cumulative phase shift relative to initial sync
	int32_t last_total_advanced_us; // Reference total advance measured at last adjustment
    int32_t absolute_max_advance_us;
    int32_t delta_sfn_slot;
    uint32_t smoothed_pnf_jitter_us;

	uint32_t previous_t1;
	uint32_t previous_t2;
	int32_t previous_sf_offset_filtered;
	int32_t previous_slot_offset_filtered;
	uint8_t initial_timinginfo_received;
	int sfn_sf;
	int sfn;
	int slot;
	
  	int mu; // some 5G slot calculations need the numerology to know the number
          // of slots

	struct timespec next_slot_time;
	uint32_t slot_duration_us;
	uint8_t running;
	
	/* Timing Jump tracking to avoid stale compensation */
	int32_t last_sfnslot_jump;

	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t  initial_timinginfo_cond;
	int socket;
	struct sockaddr_in local_addr;
	struct sockaddr_in remote_addr;
	
	vnf_p7_rx_reassembly_queue_t reassembly_queue;
	uint8_t* reassembly_buffer;
	uint32_t reassembly_buffer_size;

	uint32_t sequence_number;

	struct nfapi_vnf_p7_connection_info* next;

    /* Timing Stats History (to aggregate split packets) */
    struct {
      uint32_t abs_slot;      // Absolute slot number (sfn * slots_per_frame + slot)
      int32_t max_late;       // Max observed late value
      int32_t max_early;      // Max observed early value (negative)
    } slot_history[SLOT_ARRAY_SIZE];

    /* Time Borrowing (forward prevention of late slots) */
    int32_t time_debt_us;           // Accumulated time debt from past events

    /* Time Bank: borrowed time to be repaid by future slots */
    int32_t pending_us;             // Accumulated borrowed time (us) to be repaid incrementally

	/* Adaptive Decay Control for EWMA Lab Mode */
	int32_t last_adjustment_steps;  // How many slots we increased in last adjustment
	int32_t last_adjustment_sfn;    // SFN when we made the last upward adjustment
	int32_t last_adjustment_slot;   // Slot when we made the last upward adjustment

	/* Pressure Debt Model for UP/DOWN Control */
	int32_t pressure_debt_us;

	/*
	 * Collected during the last control period.
	 * Reset after p7_run_ewma_lab_control() consumes them.
	 */
	int32_t recent_p7_too_late_max_us;
	int32_t recent_rlc_reject_count;
	int32_t recent_harq_timeout_count;

	/*
	 * Recent average number of P7 messages per slot.
	 * If unavailable, keep it as 1.
	 */
	int32_t recent_msg_per_slot;

	/*
	* Fields for offered-load-aware peak detection.
	*/
	int32_t recent_p7_msg_count;
	int32_t estimated_offered_load;
	int32_t offered_load_dev;
	int32_t peak_offered_load;

	p7_sync_reference_profile_t sync_ref;
	p7_sync_runtime_feedback_t  sync_fb;
	int32_t learned_good_s_ahead;
	int32_t learned_good_latency_us;
	int32_t learned_good_margin_floor_us;
	/*
	* EWMA LAB timing controller internal state.
	*
	* These are NOT external nFAPI inputs.
	* They are internal controller memory used to avoid oscillation.
	*/
	int32_t ewma_lab_safe_period_count;
	int32_t ewma_lab_late_period_count;
	int32_t ewma_lab_risk_period_count;
	int32_t ewma_lab_hold_down_count;
	int32_t ewma_lab_last_direction;
	int32_t ewma_lab_last_target_s_ahead;

	int32_t ewma_lab_uncertainty_ewma_us;
	int32_t ewma_lab_uncertainty_dev_us;

	int32_t ewma_lab_current_load_profile;

	p7_ewma_lab_load_profile_t ewma_lab_load_profile[P7_EWMA_LAB_MAX_LOAD_PROFILES];
	
} nfapi_vnf_p7_connection_info_t;

typedef struct vnf_p7_s {
  nfapi_vnf_p7_config_t _public;

  // private data
	uint8_t terminate;
	nfapi_vnf_p7_connection_info_t* p7_connections;
	int socket;
	uint32_t sf_start_time_hr;
	uint32_t slot_start_time_hr;
	uint8_t* rx_message_buffer; // would this be better put in the p7 conenction info?
	uint16_t rx_message_buffer_size;

} vnf_p7_t;

uint32_t vnf_get_current_time_hr(void);

uint16_t increment_sfn_sf(uint16_t sfn_sf);
int vnf_sync(vnf_p7_t* vnf_p7, nfapi_vnf_p7_connection_info_t* p7_info);
int vnf_nr_sync(vnf_p7_t* vnf_p7, nfapi_vnf_p7_connection_info_t* p7_info);

int send_mac_subframe_indications(vnf_p7_t* config);
int send_mac_slot_indications(vnf_p7_t* config);
int vnf_p7_read_dispatch_message(vnf_p7_t* vnf_p7 );
void vnf_nr_handle_p7_message(void *pRecvMsg, int recvMsgLen, vnf_p7_t* vnf_p7);
vnf_p7_rx_message_t* vnf_p7_rx_reassembly_queue_add_segment(vnf_p7_t* vnf_p7, vnf_p7_rx_reassembly_queue_t* queue, uint16_t sequence_number, uint16_t segment_number, uint8_t m, uint8_t* data, uint16_t data_len);
void* vnf_p7_malloc(vnf_p7_t* vnf_p7, size_t size);
void vnf_p7_free(vnf_p7_t* vnf_p7, void* ptr);
void vnf_p7_rx_reassembly_queue_remove_msg(vnf_p7_t* vnf_p7, vnf_p7_rx_reassembly_queue_t* queue, vnf_p7_rx_message_t* msg);
void vnf_p7_rx_reassembly_queue_remove_old_msgs(vnf_p7_t* vnf_p7, vnf_p7_rx_reassembly_queue_t* queue, uint32_t delta);
uint32_t calculate_transmit_timestamp(int mu, uint16_t sfn, uint16_t slot, uint32_t slot_start_time_hr);


void vnf_p7_connection_info_list_add(vnf_p7_t* vnf_p7, nfapi_vnf_p7_connection_info_t* node);
nfapi_vnf_p7_connection_info_t* vnf_p7_connection_info_list_find(vnf_p7_t* vnf_p7, uint16_t phy_id);
nfapi_vnf_p7_connection_info_t* vnf_p7_connection_info_list_delete(vnf_p7_t* vnf_p7, uint16_t phy_id);

int vnf_p7_pack_and_send_p7_msg(vnf_p7_t* vnf_p7, nfapi_p7_message_header_t* header);
void vnf_p7_release_msg(vnf_p7_t* vnf_p7, nfapi_p7_message_header_t* header);
void vnf_p7_release_pdu(vnf_p7_t* vnf_p7, void* pdu);

/* Timing Statistics Structure - Simplified */
typedef struct {
  int32_t worst_late;
  int32_t worst_early;
  uint32_t packet_slot;   // Computed packet slot index in SLOT_ARRAY_SIZE
  uint32_t pnf_reported_jitter; // Maximum jitter reported by PNF across message types
} vnf_timing_stats_t;

/* Function Declaration */
// Extract timing info points from a timing_info message
// Returns the number of valid stats extracted (0-8)
int vnf_p7_extract_timing_info(const nfapi_nr_timing_info_t *ind,
                               nfapi_vnf_p7_connection_info_t *p7_info,
                               vnf_timing_stats_t *out_stats,
                               int max_stats);

/* Convergence Optimization */
void vnf_p7_convergence_optimization(nfapi_vnf_p7_connection_info_t *p7_info, const vnf_timing_stats_t *stats);

extern int s_ahead_env;

/* Main Dynamic Timing Handler */
void handle_dynamic_timing_info(nfapi_vnf_p7_connection_info_t* p7_info, void *void_ind);



#endif // _VNF_P7_H_
void unit_test_vnf_p7_convergence_optimization(void);
