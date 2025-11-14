/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file nfapi_delay_mgmt.h
 * \brief NFAPI P7 Delay Management Implementation (SCF 222 Section 2.6)
 * \author OAI
 * \date 2024
 * \version 1.0
 * \company OpenAirInterface Software Alliance
 * \email: contact@openairinterface.org
 */

#ifndef _NFAPI_DELAY_MGMT_H_
#define _NFAPI_DELAY_MGMT_H_

#include <stdint.h>
#include <sys/time.h>
#include "nfapi_nr_interface_scf.h"

#ifdef __cplusplus
extern "C" {
#endif

// Timing window configuration per message type
typedef struct {
  uint32_t timing_offset_us;   // Time offset before slot start (μs)
  uint16_t timing_window_us;   // Reception window size (μs) - 0 to 30,000
  uint8_t enabled;             // 1 if timing management enabled for this message type
} nfapi_timing_window_config_t;

// Jitter calculation state (RFC 3550 Section 6.4.1)
typedef struct {
  uint32_t previous_transit_time; // Previous transit time
  uint32_t jitter;                // Current jitter estimate in μs
  uint8_t initialized;            // Flag for first packet
} nfapi_jitter_state_t;

// Message arrival statistics
typedef struct {
  int32_t latest_delay;       // Latest delay from acceptable time (μs)
  int32_t earliest_arrival;   // Earliest arrival from acceptable time (μs)
  uint32_t num_on_time;       // Count of on-time messages
  uint32_t num_too_late;      // Count of late messages
  uint32_t num_too_early;     // Count of early messages
} nfapi_message_stats_t;

// Delay management state for PHY instance
typedef struct {
  // Timing window configurations per message type
  nfapi_timing_window_config_t dl_tti_config;
  nfapi_timing_window_config_t ul_tti_config;
  nfapi_timing_window_config_t ul_dci_config;
  nfapi_timing_window_config_t tx_data_config;

  // Jitter tracking per message type
  nfapi_jitter_state_t dl_tti_jitter;
  nfapi_jitter_state_t ul_tti_jitter;
  nfapi_jitter_state_t ul_dci_jitter;
  nfapi_jitter_state_t tx_data_jitter;

  // Message arrival statistics per message type
  nfapi_message_stats_t dl_tti_stats;
  nfapi_message_stats_t ul_tti_stats;
  nfapi_message_stats_t ul_dci_stats;
  nfapi_message_stats_t tx_data_stats;

  // Timing info reporting configuration
  uint8_t timing_info_mode;      // Bit0=Periodic, Bit1=Aperiodic
  uint8_t timing_info_period;    // Period in slots (1-255)
  uint8_t subcarrier_spacing;    // 0=15KHz, 1=30KHz, 2=60KHz, 3=120KHz, 4=240KHz

  // Timing info state
  uint16_t last_sfn;             // Last SFN for timing info
  uint16_t last_slot;            // Last slot for timing info
  struct timeval last_timing_info_time; // Time of last timing info report
  uint32_t slot_counter;         // Counter for periodic reports

  // SFN/Slot time reference (for timestamp calculations)
  struct timeval sfn_slot_zero_time; // Reference time for SFN/slot 0/0
  uint8_t time_reference_valid;      // Flag indicating if reference is valid
  
  // Node sync state
  uint32_t t1;                   // DL Node Sync t1 value
  uint32_t t2;                   // UL Node Sync t2 value
  uint32_t t3;                   // UL Node Sync t3 value
} nfapi_delay_mgmt_state_t;

// Message type enumeration for delay management
typedef enum {
  NFAPI_MSG_TYPE_DL_TTI = 0,
  NFAPI_MSG_TYPE_UL_TTI,
  NFAPI_MSG_TYPE_UL_DCI,
  NFAPI_MSG_TYPE_TX_DATA,
  NFAPI_MSG_TYPE_MAX
} nfapi_msg_type_e;

// Message arrival result
typedef enum {
  NFAPI_MSG_ARRIVAL_ON_TIME = 0,
  NFAPI_MSG_ARRIVAL_TOO_EARLY,
  NFAPI_MSG_ARRIVAL_TOO_LATE
} nfapi_msg_arrival_result_e;

// ========== Function Prototypes ==========

/**
 * @brief Initialize delay management state
 * @param state Pointer to delay management state
 */
void nfapi_delay_mgmt_init(nfapi_delay_mgmt_state_t *state);

/**
 * @brief Configure timing window for a message type
 * @param state Pointer to delay management state
 * @param msg_type Message type
 * @param timing_offset_us Timing offset in microseconds
 * @param timing_window_us Timing window in microseconds
 */
void nfapi_delay_mgmt_configure_window(nfapi_delay_mgmt_state_t *state,
                                        nfapi_msg_type_e msg_type,
                                        uint32_t timing_offset_us,
                                        uint16_t timing_window_us);

/**
 * @brief Configure timing info reporting
 * @param state Pointer to delay management state
 * @param mode Timing info mode (Bit0=Periodic, Bit1=Aperiodic)
 * @param period Period in slots for periodic reporting
 */
void nfapi_delay_mgmt_configure_timing_info(nfapi_delay_mgmt_state_t *state,
                                             uint8_t mode,
                                             uint8_t period);

/**
 * @brief Set SFN/slot 0/0 time reference
 * @param state Pointer to delay management state
 * @param ref_time Reference time for SFN/slot 0/0
 */
void nfapi_delay_mgmt_set_time_reference(nfapi_delay_mgmt_state_t *state,
                                          struct timeval *ref_time);

/**
 * @brief Calculate transmit timestamp from current time
 * @param state Pointer to delay management state
 * @return Transmit timestamp in microseconds from SFN/slot 0/0
 */
uint32_t nfapi_delay_mgmt_get_transmit_timestamp(nfapi_delay_mgmt_state_t *state);

/**
 * @brief Check if message arrived within timing window
 * @param state Pointer to delay management state
 * @param msg_type Message type
 * @param sfn Target SFN
 * @param slot Target slot
 * @param transmit_timestamp Timestamp from P7 header (μs from SFN/slot 0/0)
 * @param receive_time Message receive time
 * @return Message arrival result (on-time, too early, too late)
 */
nfapi_msg_arrival_result_e nfapi_delay_mgmt_check_message_arrival(
    nfapi_delay_mgmt_state_t *state,
    nfapi_msg_type_e msg_type,
    uint16_t sfn,
    uint16_t slot,
    uint32_t transmit_timestamp,
    struct timeval *receive_time);

/**
 * @brief Update jitter calculation for message type
 * @param state Pointer to delay management state
 * @param msg_type Message type
 * @param transmit_timestamp Timestamp from P7 header
 * @param receive_time Message receive time
 */
void nfapi_delay_mgmt_update_jitter(nfapi_delay_mgmt_state_t *state,
                                     nfapi_msg_type_e msg_type,
                                     uint32_t transmit_timestamp,
                                     struct timeval *receive_time);

/**
 * @brief Check if timing info should be sent (periodic or aperiodic)
 * @param state Pointer to delay management state
 * @param sfn Current SFN
 * @param slot Current slot
 * @param force_aperiodic Force aperiodic report (e.g., message too late/early)
 * @return 1 if timing info should be sent, 0 otherwise
 */
int nfapi_delay_mgmt_should_send_timing_info(nfapi_delay_mgmt_state_t *state,
                                              uint16_t sfn,
                                              uint16_t slot,
                                              int force_aperiodic);

/**
 * @brief Build timing info message
 * @param state Pointer to delay management state
 * @param timing_info Pointer to timing info structure to populate
 */
void nfapi_delay_mgmt_build_timing_info(nfapi_delay_mgmt_state_t *state,
                                         nfapi_nr_timing_info_t *timing_info);

/**
 * @brief Process DL Node Sync message (PNF side)
 * @param state Pointer to delay management state
 * @param dl_sync DL Node Sync message
 * @param receive_time Message receive time
 */
void nfapi_delay_mgmt_process_dl_node_sync(nfapi_delay_mgmt_state_t *state,
                                            nfapi_nr_dl_node_sync_t *dl_sync,
                                            struct timeval *receive_time);

/**
 * @brief Build UL Node Sync response (PNF side)
 * @param state Pointer to delay management state
 * @param ul_sync Pointer to UL Node Sync structure to populate
 */
void nfapi_delay_mgmt_build_ul_node_sync(nfapi_delay_mgmt_state_t *state,
                                          nfapi_nr_ul_node_sync_t *ul_sync);

/**
 * @brief Process UL Node Sync message (VNF side)
 * @param state Pointer to delay management state
 * @param ul_sync UL Node Sync message
 */
void nfapi_delay_mgmt_process_ul_node_sync(nfapi_delay_mgmt_state_t *state,
                                            nfapi_nr_ul_node_sync_t *ul_sync);

/**
 * @brief Calculate slot time from SFN/slot and subcarrier spacing
 * @param sfn System Frame Number
 * @param slot Slot number
 * @param subcarrier_spacing Subcarrier spacing (0=15KHz, 1=30KHz, etc.)
 * @return Time in microseconds from SFN/slot 0/0
 */
uint64_t nfapi_delay_mgmt_calc_slot_time_us(uint16_t sfn, uint16_t slot, uint8_t subcarrier_spacing);

/**
 * @brief Get slots per frame for subcarrier spacing
 * @param subcarrier_spacing Subcarrier spacing (0=15KHz, 1=30KHz, etc.)
 * @return Number of slots per 10ms frame
 */
uint16_t nfapi_delay_mgmt_get_slots_per_frame(uint8_t subcarrier_spacing);

#ifdef __cplusplus
}
#endif

#endif /* _NFAPI_DELAY_MGMT_H_ */
