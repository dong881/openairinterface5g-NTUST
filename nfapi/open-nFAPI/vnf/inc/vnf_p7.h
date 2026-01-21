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
#define TIMEHR_SEC(_time_hr) ((uint32_t)(_time_hr) >> 20)
#define TIMEHR_USEC(_time_hr) ((uint32_t)(_time_hr) & 0xFFFFF)
#define TIME2TIMEHR(_time) (((uint32_t)(_time.tv_sec) & 0xFFF) << 20 | ((uint32_t)(_time.tv_usec) & 0xFFFFF))
/* ============================================================================
 * DYNAMIC SLOT SLEEP TIMING CONTROL CONSTANTS
 * ============================================================================ */
/* Dynamic Target Margin (adaptive to avoid late packets) */
#define MARGIN_TOLERANCE_US     100    // Deadband zone: +/- MARGIN_TOLERANCE_US us
#define TARGET_MARGIN_INITIAL   250   // Maximum safety buffer (user request: catch all late)
#define TARGET_TIMING_WINDOW    2200  // Minimum target (to avoid constant adjustment)
#define JITTER_THRESHOLD_US     200   // High/Low jitter boundary
#define PROFILE_LIMIT     		450    // Maximum Pass 3 adjustment per cycle
#define MIN_SLEEP_US            50    // Minimum allowable sleep time
#define MAX_SLEEP_US            950  // Maximum allowable sleep time
#define MAX_BORROW_DEPTH        4     // Maximum backward/forward borrow depth
#define SLOT_ARRAY_SIZE         20    // TDD cycle slot count (Reduced to 20 for faster convergence)
#define DEFAULT_SLOT_SLEEP_US   500   // Initial sleep value for all slots


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
	pthread_t thread;
	pthread_mutex_t mutex;
	int socket;
	struct sockaddr_in local_addr;
	struct sockaddr_in remote_addr;
	
	vnf_p7_rx_reassembly_queue_t reassembly_queue;
	uint8_t* reassembly_buffer;
	uint32_t reassembly_buffer_size;

	uint32_t sequence_number;

	struct nfapi_vnf_p7_connection_info* next;

    /* Timing Control Parameters */
    int32_t sleep_baseline_us;
} nfapi_vnf_p7_connection_info_t;

struct vnf_p7_t{

	nfapi_vnf_p7_config_t _public;
	
	// private data
	uint8_t terminate;
	nfapi_vnf_p7_connection_info_t* p7_connections;
	int socket;
	uint32_t sf_start_time_hr;
	uint32_t slot_start_time_hr;
	uint8_t* rx_message_buffer; // would this be better put in the p7 conenction info?
	uint16_t rx_message_buffer_size;
	
};

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

extern int32_t slot_profile_us[SLOT_ARRAY_SIZE];
/* Timing Statistics Structure */
typedef struct {
    int32_t max_late;       // Maximum late arrival (us)
    int32_t min_late;       // Minimum late arrival (us)
    int32_t min_early;      // Minimum early arrival (us) - most negative
    int32_t max_early;      // Maximum early arrival (us)
    uint32_t jitter;        // Max jitter (us)
    uint32_t sample_count;  // Number of samples aggregated
} vnf_timing_stats_t;

/* Global statistics storage */
extern vnf_timing_stats_t vnf_dl_stats;
extern vnf_timing_stats_t vnf_ul_stats;
extern vnf_timing_stats_t vnf_all_stats;

/* Function Declaration */
void vnf_p7_extract_timing_info(const void* void_ind);

/* Pass 2: Critical Correction */
void vnf_p7_critical_correction(nfapi_vnf_p7_connection_info_t* p7_info, uint32_t current_slot);

/* Pass 3: Convergence Optimization */
void vnf_p7_convergence_optimization(nfapi_vnf_p7_connection_info_t* p7_info, uint32_t current_slot, vnf_timing_stats_t* vnf_stats);

/* Main Dynamic Timing Handler */
void handle_dynamic_timing_info(nfapi_vnf_p7_connection_info_t* p7_info, void *void_ind);

void dump_slot_sleep_states(nfapi_vnf_p7_connection_info_t* p7_info, const void* void_ind);
void dump_slot_profile_us(const void* void_ind);

#endif // _VNF_P7_H_
