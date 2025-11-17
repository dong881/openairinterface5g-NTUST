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

#include "timing_measurement.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Global timing context
timing_measurement_context_t *global_timing_ctx = NULL;

// Message type names for JSON output
static const char *fapi_message_names[] = {
  "DL_TTI_REQUEST",
  "UL_TTI_REQUEST",
  "UL_DCI_REQUEST",
  "TX_DATA_REQUEST",
  "SLOT_INDICATION",
  "UCI_INDICATION",
  "CRC_INDICATION",
  "RX_DATA_INDICATION",
  "RACH_INDICATION",
  "SRS_INDICATION"
};

// Timing point names for JSON output
static const char *timing_point_names[] = {
  "pnf_slot_indication_send",
  "message_pack",
  "socket_send",
  "socket_receive",
  "message_unpack_start",
  "message_unpack_end",
  "scheduler_start",
  "scheduler_end",
  "scheduled_data_pack_start",
  "scheduled_data_pack_end",
  "vnf_to_pnf_socket",
  "buffer_enqueue",
  "buffer_dequeue",
  "tx_func_start",
  "fronthaul_tx",
  "harq_feedback_received"
};

int timing_measurement_init(timing_measurement_context_t *ctx,
                           const char *mode,
                           const char *deployment,
                           bool ptp_sync,
                           const char *json_output_file,
                           uint32_t buffer_size) {
  if (!ctx) {
    return -1;
  }

  memset(ctx, 0, sizeof(timing_measurement_context_t));
  
  ctx->enabled = true;
  ctx->json_output_enabled = (json_output_file != NULL);
  
  if (json_output_file) {
    strncpy(ctx->json_output_file, json_output_file, sizeof(ctx->json_output_file) - 1);
  }
  
  if (mode) {
    strncpy(ctx->mode, mode, sizeof(ctx->mode) - 1);
  }
  
  if (deployment) {
    strncpy(ctx->deployment, deployment, sizeof(ctx->deployment) - 1);
  }
  
  ctx->ptp_sync = ptp_sync;
  ctx->start_time = time(NULL);
  
  // Initialize buffer
  ctx->buffer_size = buffer_size > 0 ? buffer_size : TIMING_BUFFER_SIZE;
  ctx->measurements = calloc(ctx->buffer_size, sizeof(timing_measurement_t));
  if (!ctx->measurements) {
    return -1;
  }
  
  ctx->write_index = 0;
  ctx->measurement_count = 0;
  
  // Initialize mutexes
  pthread_mutex_init(&ctx->lock, NULL);
  pthread_mutex_init(&ctx->harq_lock, NULL);
  
  // Initialize HARQ table
  memset(ctx->harq_table, 0, sizeof(ctx->harq_table));
  
  // Initialize packet statistics
  memset(ctx->packet_stats, 0, sizeof(ctx->packet_stats));
  
  // Auto-flush configuration
  ctx->auto_flush = true;
  ctx->flush_interval = 1000;  // Flush every 1000 measurements
  
  // Open JSON file and write header
  if (ctx->json_output_enabled) {
    ctx->json_fp = fopen(ctx->json_output_file, "w");
    if (!ctx->json_fp) {
      fprintf(stderr, "Failed to open JSON output file: %s (errno=%d)\n", 
              ctx->json_output_file, errno);
      free(ctx->measurements);
      return -1;
    }
    
    // Write JSON header
    fprintf(ctx->json_fp, "{\n");
    fprintf(ctx->json_fp, "  \"session_info\": {\n");
    fprintf(ctx->json_fp, "    \"mode\": \"%s\",\n", ctx->mode);
    fprintf(ctx->json_fp, "    \"start_time\": \"%ld\",\n", ctx->start_time);
    fprintf(ctx->json_fp, "    \"deployment\": \"%s\",\n", ctx->deployment);
    fprintf(ctx->json_fp, "    \"ptp_sync\": %s\n", ctx->ptp_sync ? "true" : "false");
    fprintf(ctx->json_fp, "  },\n");
    fprintf(ctx->json_fp, "  \"measurements\": [\n");
    fflush(ctx->json_fp);
  }
  
  return 0;
}

void timing_measurement_cleanup(timing_measurement_context_t *ctx) {
  if (!ctx) {
    return;
  }
  
  // Flush any remaining measurements
  if (ctx->enabled && ctx->json_output_enabled) {
    timing_measurement_flush(ctx);
  }
  
  // Close JSON file
  if (ctx->json_fp) {
    // Write packet statistics
    fprintf(ctx->json_fp, "  ],\n");
    fprintf(ctx->json_fp, "  \"packet_statistics\": {\n");
    
    for (int i = 0; i < FAPI_MSG_MAX; i++) {
      if (i > 0) fprintf(ctx->json_fp, ",\n");
      fprintf(ctx->json_fp, "    \"%s\": {\n", fapi_message_names[i]);
      fprintf(ctx->json_fp, "      \"total\": %lu,\n", ctx->packet_stats[i].total);
      fprintf(ctx->json_fp, "      \"dropped\": %lu,\n", ctx->packet_stats[i].dropped);
      fprintf(ctx->json_fp, "      \"late\": %lu\n", ctx->packet_stats[i].late);
      fprintf(ctx->json_fp, "    }");
    }
    
    fprintf(ctx->json_fp, "\n  }\n");
    fprintf(ctx->json_fp, "}\n");
    
    fclose(ctx->json_fp);
    ctx->json_fp = NULL;
  }
  
  // Free buffer
  if (ctx->measurements) {
    free(ctx->measurements);
    ctx->measurements = NULL;
  }
  
  // Destroy mutexes
  pthread_mutex_destroy(&ctx->lock);
  pthread_mutex_destroy(&ctx->harq_lock);
  
  ctx->enabled = false;
}

timing_measurement_t* timing_measurement_start(timing_measurement_context_t *ctx,
                                              uint16_t sfn,
                                              uint16_t slot,
                                              uint8_t message_type,
                                              uint32_t rnti,
                                              uint8_t harq_process_id) {
  if (!ctx || !ctx->enabled || !ctx->measurements) {
    return NULL;
  }
  
  pthread_mutex_lock(&ctx->lock);
  
  timing_measurement_t *measurement = &ctx->measurements[ctx->write_index];
  
  // Clear the measurement entry
  memset(measurement, 0, sizeof(timing_measurement_t));
  
  // Set basic information
  measurement->sfn = sfn;
  measurement->slot = slot;
  measurement->message_type = message_type;
  measurement->rnti = rnti;
  measurement->harq_process_id = harq_process_id;
  
  // Advance write index (circular buffer)
  ctx->write_index = (ctx->write_index + 1) % ctx->buffer_size;
  ctx->measurement_count++;
  
  pthread_mutex_unlock(&ctx->lock);
  
  return measurement;
}

void timing_measurement_record(timing_measurement_t *measurement,
                              timing_point_t point,
                              uint64_t timestamp) {
  if (!measurement || point >= TIMING_POINT_MAX) {
    return;
  }
  
  measurement->timestamps[point] = timestamp;
  set_timestamp_valid(measurement, point);
}

harq_tracking_entry_t* harq_tracking_start(timing_measurement_context_t *ctx,
                                          uint32_t rnti,
                                          uint8_t harq_process_id,
                                          uint16_t sfn,
                                          uint16_t slot,
                                          uint64_t timestamp) {
  if (!ctx) {
    return NULL;
  }
  
  pthread_mutex_lock(&ctx->harq_lock);
  
  // Find an empty entry
  harq_tracking_entry_t *entry = NULL;
  for (int i = 0; i < MAX_HARQ_ENTRIES; i++) {
    if (!ctx->harq_table[i].in_use) {
      entry = &ctx->harq_table[i];
      break;
    }
  }
  
  if (entry) {
    memset(entry, 0, sizeof(harq_tracking_entry_t));
    entry->rnti = rnti;
    entry->harq_process_id = harq_process_id;
    entry->initial_tx_timestamp = timestamp;
    entry->initial_sfn = sfn;
    entry->initial_slot = slot;
    entry->retransmission_count = 0;
    entry->completed = false;
    entry->in_use = true;
  }
  
  pthread_mutex_unlock(&ctx->harq_lock);
  
  return entry;
}

void harq_tracking_add_retransmission(harq_tracking_entry_t *entry,
                                     uint16_t sfn,
                                     uint16_t slot,
                                     uint8_t rv_index,
                                     uint64_t timestamp) {
  if (!entry || entry->retransmission_count >= MAX_HARQ_RETRANSMISSIONS) {
    return;
  }
  
  harq_retransmission_t *retx = &entry->retransmissions[entry->retransmission_count];
  retx->timestamp = timestamp;
  retx->sfn = sfn;
  retx->slot = slot;
  retx->rv_index = rv_index;
  
  entry->retransmission_count++;
}

void harq_tracking_complete(harq_tracking_entry_t *entry,
                           uint16_t sfn,
                           uint16_t slot,
                           uint64_t timestamp) {
  if (!entry) {
    return;
  }
  
  entry->ack_timestamp = timestamp;
  entry->ack_sfn = sfn;
  entry->ack_slot = slot;
  entry->completed = true;
}

static void write_measurement_json(FILE *fp, const timing_measurement_t *m, bool first) {
  if (!fp || !m) {
    return;
  }
  
  if (!first) {
    fprintf(fp, ",\n");
  }
  
  fprintf(fp, "    {\n");
  fprintf(fp, "      \"sfn\": %u,\n", m->sfn);
  fprintf(fp, "      \"slot\": %u,\n", m->slot);
  fprintf(fp, "      \"message_type\": \"%s\",\n", 
          m->message_type < FAPI_MSG_MAX ? fapi_message_names[m->message_type] : "UNKNOWN");
  fprintf(fp, "      \"rnti\": %u,\n", m->rnti);
  fprintf(fp, "      \"harq_process_id\": %u,\n", m->harq_process_id);
  
  // Write timestamps
  fprintf(fp, "      \"timestamps\": {\n");
  bool first_ts = true;
  for (int i = 0; i < TIMING_POINT_MAX; i++) {
    if (is_timestamp_valid(m, i)) {
      if (!first_ts) fprintf(fp, ",\n");
      fprintf(fp, "        \"%s\": %lu", timing_point_names[i], m->timestamps[i]);
      first_ts = false;
    }
  }
  fprintf(fp, "\n      },\n");
  
  // Calculate and write latencies in microseconds
  fprintf(fp, "      \"latencies_us\": {\n");
  
  double pack_latency = 0.0;
  if (is_timestamp_valid(m, TIMING_POINT_MESSAGE_PACK) && 
      is_timestamp_valid(m, TIMING_POINT_PNF_SLOT_INDICATION_SEND)) {
    pack_latency = (m->timestamps[TIMING_POINT_MESSAGE_PACK] - 
                   m->timestamps[TIMING_POINT_PNF_SLOT_INDICATION_SEND]) / 1000.0;
  }
  
  double network_transport = 0.0;
  if (is_timestamp_valid(m, TIMING_POINT_SOCKET_RECEIVE) && 
      is_timestamp_valid(m, TIMING_POINT_SOCKET_SEND)) {
    network_transport = (m->timestamps[TIMING_POINT_SOCKET_RECEIVE] - 
                        m->timestamps[TIMING_POINT_SOCKET_SEND]) / 1000.0;
  }
  
  double unpack_latency = 0.0;
  if (is_timestamp_valid(m, TIMING_POINT_MESSAGE_UNPACK_END) && 
      is_timestamp_valid(m, TIMING_POINT_MESSAGE_UNPACK_START)) {
    unpack_latency = (m->timestamps[TIMING_POINT_MESSAGE_UNPACK_END] - 
                     m->timestamps[TIMING_POINT_MESSAGE_UNPACK_START]) / 1000.0;
  }
  
  double scheduler_processing = 0.0;
  if (is_timestamp_valid(m, TIMING_POINT_SCHEDULER_END) && 
      is_timestamp_valid(m, TIMING_POINT_SCHEDULER_START)) {
    scheduler_processing = (m->timestamps[TIMING_POINT_SCHEDULER_END] - 
                           m->timestamps[TIMING_POINT_SCHEDULER_START]) / 1000.0;
  }
  
  double data_pack_latency = 0.0;
  if (is_timestamp_valid(m, TIMING_POINT_SCHEDULED_DATA_PACK_END) && 
      is_timestamp_valid(m, TIMING_POINT_SCHEDULED_DATA_PACK_START)) {
    data_pack_latency = (m->timestamps[TIMING_POINT_SCHEDULED_DATA_PACK_END] - 
                        m->timestamps[TIMING_POINT_SCHEDULED_DATA_PACK_START]) / 1000.0;
  }
  
  double vnf_to_pnf_transport = 0.0;
  if (is_timestamp_valid(m, TIMING_POINT_BUFFER_ENQUEUE) && 
      is_timestamp_valid(m, TIMING_POINT_VNF_TO_PNF_SOCKET)) {
    vnf_to_pnf_transport = (m->timestamps[TIMING_POINT_BUFFER_ENQUEUE] - 
                           m->timestamps[TIMING_POINT_VNF_TO_PNF_SOCKET]) / 1000.0;
  }
  
  double buffer_wait_time = 0.0;
  if (is_timestamp_valid(m, TIMING_POINT_BUFFER_DEQUEUE) && 
      is_timestamp_valid(m, TIMING_POINT_BUFFER_ENQUEUE)) {
    buffer_wait_time = (m->timestamps[TIMING_POINT_BUFFER_DEQUEUE] - 
                       m->timestamps[TIMING_POINT_BUFFER_ENQUEUE]) / 1000.0;
  }
  
  double phy_processing = 0.0;
  if (is_timestamp_valid(m, TIMING_POINT_FRONTHAUL_TX) && 
      is_timestamp_valid(m, TIMING_POINT_TX_FUNC_START)) {
    phy_processing = (m->timestamps[TIMING_POINT_FRONTHAUL_TX] - 
                     m->timestamps[TIMING_POINT_TX_FUNC_START]) / 1000.0;
  }
  
  double total_latency = pack_latency + network_transport + unpack_latency + 
                        scheduler_processing + data_pack_latency + vnf_to_pnf_transport + 
                        buffer_wait_time + phy_processing;
  
  fprintf(fp, "        \"pack_latency\": %.3f,\n", pack_latency);
  fprintf(fp, "        \"network_transport\": %.3f,\n", network_transport);
  fprintf(fp, "        \"unpack_latency\": %.3f,\n", unpack_latency);
  fprintf(fp, "        \"scheduler_processing\": %.3f,\n", scheduler_processing);
  fprintf(fp, "        \"data_pack_latency\": %.3f,\n", data_pack_latency);
  fprintf(fp, "        \"vnf_to_pnf_transport\": %.3f,\n", vnf_to_pnf_transport);
  fprintf(fp, "        \"buffer_wait_time\": %.3f,\n", buffer_wait_time);
  fprintf(fp, "        \"phy_processing\": %.3f,\n", phy_processing);
  fprintf(fp, "        \"total_latency\": %.3f\n", total_latency);
  fprintf(fp, "      },\n");
  
  // Write flags
  fprintf(fp, "      \"flags\": {\n");
  fprintf(fp, "        \"dropped\": %s,\n", m->dropped ? "true" : "false");
  fprintf(fp, "        \"retransmission\": %s,\n", m->retransmission ? "true" : "false");
  fprintf(fp, "        \"nack_received\": %s\n", m->nack_received ? "true" : "false");
  fprintf(fp, "      }\n");
  fprintf(fp, "    }");
}

void timing_measurement_flush(timing_measurement_context_t *ctx) {
  if (!ctx || !ctx->enabled || !ctx->json_output_enabled || !ctx->json_fp) {
    return;
  }
  
  pthread_mutex_lock(&ctx->lock);
  
  // Determine how many measurements to flush
  uint32_t count = ctx->measurement_count < ctx->buffer_size ? 
                   ctx->measurement_count : ctx->buffer_size;
  
  // Calculate starting index
  uint32_t start_index;
  if (ctx->measurement_count < ctx->buffer_size) {
    start_index = 0;
  } else {
    start_index = ctx->write_index;
  }
  
  // Write measurements
  bool first = (ctx->measurement_count <= ctx->buffer_size);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t index = (start_index + i) % ctx->buffer_size;
    write_measurement_json(ctx->json_fp, &ctx->measurements[index], first && i == 0);
  }
  
  fflush(ctx->json_fp);
  
  pthread_mutex_unlock(&ctx->lock);
}

void timing_update_packet_stats(timing_measurement_context_t *ctx,
                               uint8_t message_type,
                               bool dropped,
                               bool late) {
  if (!ctx || message_type >= FAPI_MSG_MAX) {
    return;
  }
  
  pthread_mutex_lock(&ctx->lock);
  
  ctx->packet_stats[message_type].total++;
  if (dropped) {
    ctx->packet_stats[message_type].dropped++;
  }
  if (late) {
    ctx->packet_stats[message_type].late++;
  }
  
  pthread_mutex_unlock(&ctx->lock);
  
  // Auto-flush check
  if (ctx->auto_flush && (ctx->measurement_count % ctx->flush_interval) == 0) {
    timing_measurement_flush(ctx);
  }
}
