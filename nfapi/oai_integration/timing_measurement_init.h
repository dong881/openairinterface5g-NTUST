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

#ifndef TIMING_MEASUREMENT_INIT_H
#define TIMING_MEASUREMENT_INIT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the global timing measurement system
 * 
 * @param mode Operating mode ("fapi" or "nfapi")
 * @param deployment Deployment type ("same_machine" or "different_machine")
 * @param ptp_sync Whether PTP synchronization is enabled
 * @param json_output_file Path to JSON output file (NULL to disable file output)
 * @param buffer_size Size of measurement buffer (0 for default)
 */
void timing_measurement_global_init(const char *mode,
                                   const char *deployment,
                                   bool ptp_sync,
                                   const char *json_output_file,
                                   uint32_t buffer_size);

/**
 * Cleanup the global timing measurement system
 */
void timing_measurement_global_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* TIMING_MEASUREMENT_INIT_H */
