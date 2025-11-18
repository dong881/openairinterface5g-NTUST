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

#ifndef TIMING_MEASUREMENT_H
#define TIMING_MEASUREMENT_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of measurements to buffer before flushing to file
#define TIMING_BUFFER_SIZE 10000

// Maximum number of HARQ tracking entries
#define MAX_HARQ_ENTRIES 1000

// Maximum number of retransmissions per HARQ process
#define MAX_HARQ_RETRANSMISSIONS 8

// Timing measurement point identifiers
typedef enum {
  TIMING_POINT_PNF_SLOT_INDICATION_SEND = 0,
  TIMING_POINT_MESSAGE_PACK,
  TIMING_POINT_SOCKET_SEND,
  TIMING_POINT_SOCKET_RECEIVE,
  TIMING_POINT_MESSAGE_UNPACK_START,
  TIMING_POINT_MESSAGE_UNPACK_END,
  TIMING_POINT_SCHEDULER_START,
  TIMING_POINT_SCHEDULER_END,
  TIMING_POINT_SCHEDULED_DATA_PACK_START,
  TIMING_POINT_SCHEDULED_DATA_PACK_END,
  TIMING_POINT_VNF_TO_PNF_SOCKET,
  TIMING_POINT_BUFFER_ENQUEUE,
  TIMING_POINT_BUFFER_DEQUEUE,
  TIMING_POINT_TX_FUNC_START,
  TIMING_POINT_FRONTHAUL_TX,
  TIMING_POINT_HARQ_FEEDBACK_RECEIVED,
  TIMING_POINT_MAX
} timing_point_t;

// FAPI message types
typedef enum {
  FAPI_MSG_DL_TTI_REQUEST = 0,
  FAPI_MSG_UL_TTI_REQUEST,
  FAPI_MSG_UL_DCI_REQUEST,
  FAPI_MSG_TX_DATA_REQUEST,
  FAPI_MSG_SLOT_INDICATION,
  FAPI_MSG_UCI_INDICATION,
  FAPI_MSG_CRC_INDICATION,
  FAPI_MSG_RX_DATA_INDICATION,
  FAPI_MSG_RACH_INDICATION,
  FAPI_MSG_SRS_INDICATION,
  FAPI_MSG_MAX
} fapi_message_type_t;

// Single timing measurement entry
typedef struct {
  uint64_t timestamps[TIMING_POINT_MAX];  // nanosecond precision timestamps
  uint16_t sfn;                           // System Frame Number
  uint16_t slot;                          // Slot Number
  uint8_t message_type;                   // FAPI message type
  uint8_t harq_process_id;                // HARQ process ID (if applicable)
  uint32_t rnti;                          // Radio Network Temporary Identifier
  bool dropped;                           // Message was dropped
  bool retransmission;                    // This is a retransmission
  bool nack_received;                     // NACK was received for this message
  uint8_t valid_mask[TIMING_POINT_MAX/8 + 1]; // Bitmask for valid timestamps
} timing_measurement_t;

// HARQ retransmission tracking
typedef struct {
  uint64_t timestamp;
  uint16_t sfn;
  uint16_t slot;
  uint8_t rv_index;
} harq_retransmission_t;

// HARQ tracking entry
typedef struct {
  uint32_t rnti;
  uint8_t harq_process_id;
  uint64_t initial_tx_timestamp;
  uint16_t initial_sfn;
  uint16_t initial_slot;
  uint8_t retransmission_count;
  harq_retransmission_t retransmissions[MAX_HARQ_RETRANSMISSIONS];
  uint64_t ack_timestamp;
  uint16_t ack_sfn;
  uint16_t ack_slot;
  bool completed;
  bool in_use;
} harq_tracking_entry_t;

// Packet statistics per message type
typedef struct {
  uint64_t total;
  uint64_t dropped;
  uint64_t late;
} packet_stats_t;

// Main timing measurement context
typedef struct {
  bool enabled;
  bool json_output_enabled;
  char json_output_file[256];
  FILE *json_fp;
  pthread_mutex_t lock;
  
  // Circular buffer for measurements
  timing_measurement_t *measurements;
  uint32_t buffer_size;
  uint32_t write_index;
  uint32_t measurement_count;
  
  // HARQ tracking table
  harq_tracking_entry_t harq_table[MAX_HARQ_ENTRIES];
  pthread_mutex_t harq_lock;
  
  // Packet statistics
  packet_stats_t packet_stats[FAPI_MSG_MAX];
  
  // Session information
  char mode[16];  // "fapi" or "nfapi"
  char deployment[32];  // "same_machine" or "different_machine"
  bool ptp_sync;
  time_t start_time;
  
  // Auto-flush configuration
  bool auto_flush;
  uint32_t flush_interval;  // Number of measurements before flush
} timing_measurement_context_t;

// Get high-precision timestamp
static inline uint64_t get_timestamp_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Set a bit in the valid mask
static inline void set_timestamp_valid(timing_measurement_t *m, timing_point_t point) {
  if (point < TIMING_POINT_MAX) {
    m->valid_mask[point / 8] |= (1 << (point % 8));
  }
}

// Check if a timestamp is valid
static inline bool is_timestamp_valid(const timing_measurement_t *m, timing_point_t point) {
  if (point < TIMING_POINT_MAX) {
    return (m->valid_mask[point / 8] & (1 << (point % 8))) != 0;
  }
  return false;
}

// Initialize timing measurement system
int timing_measurement_init(timing_measurement_context_t *ctx,
                           const char *mode,
                           const char *deployment,
                           bool ptp_sync,
                           const char *json_output_file,
                           uint32_t buffer_size);

// Cleanup timing measurement system
void timing_measurement_cleanup(timing_measurement_context_t *ctx);

// Start a new measurement for a specific frame/slot
timing_measurement_t* timing_measurement_start(timing_measurement_context_t *ctx,
                                              uint16_t sfn,
                                              uint16_t slot,
                                              uint8_t message_type,
                                              uint32_t rnti,
                                              uint8_t harq_process_id);

// Record a timestamp for a measurement point
void timing_measurement_record(timing_measurement_t *measurement,
                              timing_point_t point,
                              uint64_t timestamp);

// HARQ tracking functions
harq_tracking_entry_t* harq_tracking_start(timing_measurement_context_t *ctx,
                                          uint32_t rnti,
                                          uint8_t harq_process_id,
                                          uint16_t sfn,
                                          uint16_t slot,
                                          uint64_t timestamp);

void harq_tracking_add_retransmission(harq_tracking_entry_t *entry,
                                     uint16_t sfn,
                                     uint16_t slot,
                                     uint8_t rv_index,
                                     uint64_t timestamp);

void harq_tracking_complete(harq_tracking_entry_t *entry,
                           uint16_t sfn,
                           uint16_t slot,
                           uint64_t timestamp);

// Flush measurements to JSON file
void timing_measurement_flush(timing_measurement_context_t *ctx);

// Update packet statistics
void timing_update_packet_stats(timing_measurement_context_t *ctx,
                               uint8_t message_type,
                               bool dropped,
                               bool late);

// Timing measurement macros - always enabled
#define TIMING_RECORD(ctx, measurement, point) \
  do { \
    if ((ctx) && (ctx)->enabled && (measurement)) { \
      timing_measurement_record((measurement), (point), get_timestamp_ns()); \
    } \
  } while(0)

#define TIMING_START(ctx, sfn, slot, msg_type, rnti, harq_id) \
  ((ctx) && (ctx)->enabled ? timing_measurement_start((ctx), (sfn), (slot), (msg_type), (rnti), (harq_id)) : NULL)

#define TIMING_UPDATE_STATS(ctx, msg_type, dropped, late) \
  do { \
    if ((ctx) && (ctx)->enabled) { \
      timing_update_packet_stats((ctx), (msg_type), (dropped), (late)); \
    } \
  } while(0)

// Global timing context (to be defined in implementation)
extern timing_measurement_context_t *global_timing_ctx;

#ifdef __cplusplus
}
#endif

#endif /* TIMING_MEASUREMENT_H */
