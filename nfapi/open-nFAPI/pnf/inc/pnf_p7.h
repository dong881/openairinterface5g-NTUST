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



#ifndef _PNF_P7_H_
#define _PNF_P7_H_

#define TIMEHR_SEC(_time_hr) ((uint32_t)(_time_hr) >> 20)
#define TIMEHR_USEC(_time_hr) ((uint32_t)(_time_hr) & 0xFFFFF)
#define TIME2TIMEHR(_time) (((uint32_t)(_time.tv_sec) & 0xFFF) << 20 | ((uint32_t)(_time.tv_usec) & 0xFFFFF))

#include "nfapi_pnf_interface.h"


typedef struct {
	uint16_t dl_conf_ontime;
	uint16_t dl_conf_late;
	uint16_t ul_conf_ontime;
	uint16_t ul_conf_late;
	uint16_t hi_dci0_ontime;
	uint16_t hi_dci0_late;
	uint16_t tx_ontime;
	uint16_t tx_late;
} pnf_p7_stats_t;


typedef struct pnf_p7_arr_time {
  uint16_t ontime;
  uint16_t late;
} pnf_p7_arr_time_t;
typedef struct pnf_p7_dir {
  uint32_t bytes;
} pnf_p7_dir_t;
typedef struct {
  pnf_p7_arr_time_t dl_tti;
  pnf_p7_arr_time_t ul_tti;
  pnf_p7_arr_time_t ul_dci;
  pnf_p7_arr_time_t tx_data;
  pnf_p7_dir_t dl;
  pnf_p7_dir_t ul;
} pnf_p7_nr_stats_t;

typedef struct {
	uint8_t* buffer;
	uint32_t length;
} pnf_p7_rx_message_segment_t;

typedef struct pnf_p7_rx_message pnf_p7_rx_message_t;

typedef struct pnf_p7_rx_message {
	uint8_t sequence_number;
	uint8_t num_segments_received;
	uint8_t num_segments_expected;

	// the spec allows of upto 128 segments, this does seem excessive
	pnf_p7_rx_message_segment_t segments[128];

	uint32_t rx_hr_time;

	pnf_p7_rx_message_t* next;
} pnf_p7_rx_message_t;

typedef struct {

	pnf_p7_rx_message_t* msg_queue;

} pnf_p7_rx_reassembly_queue_t;


struct pnf_p7_t {

	nfapi_pnf_p7_config_t _public;

	//private data
	int p7_sock;

	uint8_t terminate;

	uint8_t tx_message_buffer[NFAPI_MAX_PACKED_MESSAGE_SIZE];
	uint8_t* rx_message_buffer;
	uint16_t rx_message_buffer_size;

	pthread_mutex_t mutex; // should we allow the client to specifiy
	pthread_mutex_t pack_mutex; // should we allow the client to specifiy

	nfapi_pnf_p7_subframe_buffer_t subframe_buffer[30/*NFAPI_MAX_TIMING_WINDOW_SIZE*/];
    nfapi_pnf_p7_slot_buffer_t slot_buffer[30/*NFAPI_MAX_TIMING_WINDOW_SIZE*/];
	uint32_t sequence_number;
	uint16_t max_num_segments;

	pnf_p7_rx_reassembly_queue_t reassembly_queue;

	uint8_t* reassemby_buffer;
	uint32_t reassemby_buffer_size;

	uint16_t sfn_sf;
	uint32_t sf_start_time_hr;
	int32_t sfn_sf_shift;
	
	uint16_t sfn;
	uint16_t slot;
  int mu;
	uint16_t sfn_slot;
	uint32_t slot_start_time_hr;
	int32_t slot_shift;

	uint8_t timing_info_period_counter;
	uint8_t timing_info_aperiodic_send; // 0:false 1:true

	uint32_t timing_info_ms_counter; // number of ms since last timing info

	uint32_t dl_config_jitter;
	uint32_t ul_config_jitter;
	uint32_t hi_dci0_jitter;
	uint32_t tx_jitter;
    
	//P7 NR - RFC 3550 jitter calculation state
	// Each message type has: jitter value (uint32_t), prev_transit (int64_t), and init flag
	uint32_t dl_tti_jitter;
	uint32_t ul_tti_jitter;
	uint32_t ul_dci_jitter;
	uint32_t tx_data_jitter;
	
	// RFC 3550 jitter state: previous transit time (arrival - transmit) in microseconds
	int64_t dl_tti_prev_transit_us;
	int64_t ul_tti_prev_transit_us;
	int64_t ul_dci_prev_transit_us;
	int64_t tx_data_prev_transit_us;
	
	// RFC 3550 jitter state (wrap-safe): previous receive time (TIME_HR) and transmit timestamp (µs)
	// NOTE: In OAI nFAPI, P7 header transmit_timestamp is derived from SFN/slot and wraps every 10.24s.
	//       Therefore we compute jitter from deltas (R(i)-R(i-1)) and (S(i)-S(i-1)) with wrap handling.
	uint32_t dl_tti_prev_rx_time_hr;
	uint32_t ul_tti_prev_rx_time_hr;
	uint32_t ul_dci_prev_rx_time_hr;
	uint32_t tx_data_prev_rx_time_hr;
	
	uint32_t dl_tti_prev_tx_ts_us;
	uint32_t ul_tti_prev_tx_ts_us;
	uint32_t ul_dci_prev_tx_ts_us;
	uint32_t tx_data_prev_tx_ts_us;
	
	// Smoothed jitter estimate (as double for 1/16 smoothing factor)
	double dl_tti_jitter_us;
	double ul_tti_jitter_us;
	double ul_dci_jitter_us;
	double tx_data_jitter_us;
	
	// Init flags for RFC 3550 jitter calculation
	uint8_t dl_tti_jitter_init;
	uint8_t ul_tti_jitter_init;
	uint8_t ul_dci_jitter_init;
	uint8_t tx_data_jitter_init;
	
	// Timestamp unwrap state (32-bit to 64-bit conversion)
	uint64_t ts_epoch_base;
	uint32_t last_ts_32;

	// Legacy fields (kept for compatibility)
	int32_t dl_tti_prev_transit_time_diff;
	int32_t ul_tti_prev_transit_time_diff;
	int32_t ul_dci_prev_transit_time_diff;
	int32_t tx_data_prev_transit_time_diff;

	uint32_t dl_tti_latest_delay;
	uint32_t dl_tti_earliest_arrival;
	uint32_t ul_tti_latest_delay;
	uint32_t ul_tti_earliest_arrival;
	uint32_t ul_dci_latest_delay;
	uint32_t ul_dci_earliest_arrival;
	uint32_t tx_data_latest_delay;
	uint32_t tx_data_earliest_arrival;

	// Configuration
	uint32_t dl_tti_timing_offset;
	uint32_t ul_tti_timing_offset;
	uint32_t ul_dci_timing_offset;
	uint32_t tx_data_timing_offset;
	uint32_t timing_window;
	uint32_t timing_info_mode;
	uint32_t timing_info_period;

	uint32_t tick;
	pnf_p7_stats_t stats;
	pnf_p7_nr_stats_t nr_stats;

};

int pnf_p7_message_pump(pnf_p7_t* pnf_p7);
int pnf_nr_p7_message_pump(pnf_p7_t* pnf_p7);
int pnf_p7_pack_and_send_p7_message(pnf_p7_t* pnf_p7, nfapi_p7_message_header_t* msg, uint32_t msg_len);
int pnf_p7_send_message(pnf_p7_t* pnf_p7, uint8_t* msg, uint32_t msg_len);


int pnf_p7_slot_ind(pnf_p7_t* config, uint16_t phy_id, uint16_t sfn, uint16_t slot);
int pnf_p7_subframe_ind(pnf_p7_t* config, uint16_t phy_id, uint16_t sfn_sf);
int nfapi_pnf_p7_nr_slot_ind(nfapi_pnf_p7_config_t* config, nfapi_nr_slot_indication_scf_t* ind);
int nfapi_pnf_p7_nr_rx_data_ind(nfapi_pnf_p7_config_t* config, nfapi_nr_rx_data_indication_t* ind);
int nfapi_pnf_p7_nr_crc_ind(nfapi_pnf_p7_config_t* config, nfapi_nr_crc_indication_t* ind);
int nfapi_pnf_p7_nr_srs_ind(nfapi_pnf_p7_config_t* config, nfapi_nr_srs_indication_t* ind);
int nfapi_pnf_p7_nr_uci_ind(nfapi_pnf_p7_config_t* config, nfapi_nr_uci_indication_t* ind);
int nfapi_pnf_p7_nr_rach_ind(nfapi_pnf_p7_config_t* config, nfapi_nr_rach_indication_t* ind);
pnf_p7_rx_message_t* pnf_p7_rx_reassembly_queue_add_segment(pnf_p7_t* pnf_p7, pnf_p7_rx_reassembly_queue_t* queue, uint32_t rx_hr_time, uint16_t sequence_number, uint16_t segment_number, uint8_t m, uint8_t* data, uint16_t data_len);
void pnf_p7_rx_reassembly_queue_remove_msg(pnf_p7_t* pnf_p7, pnf_p7_rx_reassembly_queue_t* queue, pnf_p7_rx_message_t* msg);
void pnf_p7_rx_reassembly_queue_remove_old_msgs(pnf_p7_t* pnf_p7, pnf_p7_rx_reassembly_queue_t* queue, uint32_t rx_hr_time, uint32_t delta);
void pnf_nr_handle_p7_message(void *pRecvMsg, int recvMsgLen, pnf_p7_t *pnf_p7, uint32_t rx_hr_time);
struct timespec pnf_timespec_sub(struct timespec lhs, struct timespec rhs);
uint32_t pnf_get_current_time_hr(void);
struct timespec pnf_timespec_add(struct timespec lhs, struct timespec rhs);
void pnf_p7_free(pnf_p7_t* pnf_p7, void* ptr);
void* pnf_p7_malloc(pnf_p7_t* pnf_p7, size_t size);

/*===========================================================================
 * RFC 3550 Section 6.4.1 Interarrival Jitter Calculation
 * 
 * The jitter is calculated using the method defined in RFC 3550:
 *   transit = arrival_time - transmit_timestamp
 *   d = transit - prev_transit
 *   jitter = jitter + (|d| - jitter) / 16
 *
 * For P7 Timing Info, we use:
 *   - transmit_timestamp: P7 header's Transmit Timestamp (32-bit µs)
 *   - arrival_time: PHY receive time (µs, from monotonic clock)
 *===========================================================================*/

typedef enum {
    NFAPI_JITTER_DL_TTI = 0,
    NFAPI_JITTER_UL_TTI,
    NFAPI_JITTER_UL_DCI,
    NFAPI_JITTER_TX_DATA,
    NFAPI_JITTER_MAX
} nfapi_jitter_msg_type_t;

// Convert TIME_HR format to microseconds (within the 12-bit second cycle)
uint64_t pnf_timehr_to_us(pnf_p7_t* pnf_p7, uint32_t time_hr);

// Update jitter state using RFC 3550 algorithm
void pnf_update_jitter(pnf_p7_t* pnf_p7, 
                       nfapi_jitter_msg_type_t msg_type,
                       uint32_t p7_tx_timestamp,
                       uint32_t recv_time_hr);

// Get jitter value for timing info (uint32_t in µs)
uint32_t pnf_get_jitter(pnf_p7_t* pnf_p7, nfapi_jitter_msg_type_t msg_type);

// Reset jitter state (e.g., on sync reset)
void pnf_reset_jitter(pnf_p7_t* pnf_p7, nfapi_jitter_msg_type_t msg_type);

#endif /* _PNF_P7_H_ */

