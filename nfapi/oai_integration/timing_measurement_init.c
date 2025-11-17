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

#include "timing_measurement_init.h"

#ifdef ENABLE_TIMING_MEASUREMENT
#include "timing_measurement.h"
#include <stdlib.h>
#include <string.h>

// Global timing context instance
static timing_measurement_context_t timing_ctx_instance;

// Initialize global timing context
void timing_measurement_global_init(const char *mode,
                                   const char *deployment,
                                   bool ptp_sync,
                                   const char *json_output_file,
                                   uint32_t buffer_size) {
  if (timing_measurement_init(&timing_ctx_instance, mode, deployment, ptp_sync, 
                             json_output_file, buffer_size) == 0) {
    global_timing_ctx = &timing_ctx_instance;
  } else {
    global_timing_ctx = NULL;
  }
}

// Cleanup global timing context
void timing_measurement_global_cleanup(void) {
  if (global_timing_ctx) {
    timing_measurement_cleanup(global_timing_ctx);
    global_timing_ctx = NULL;
  }
}

#else

// Stub implementations when timing measurement is disabled
void timing_measurement_global_init(const char *mode,
                                   const char *deployment,
                                   bool ptp_sync,
                                   const char *json_output_file,
                                   uint32_t buffer_size) {
  (void)mode;
  (void)deployment;
  (void)ptp_sync;
  (void)json_output_file;
  (void)buffer_size;
}

void timing_measurement_global_cleanup(void) {
}

#endif
