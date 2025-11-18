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

#ifndef NFAPI_VNF_H_
#define NFAPI_VNF_H_

void configure_nfapi_vnf(char *vnf_addr, int vnf_p5_port, char *pnf_ip_addr, int pnf_p7_port, int vnf_p7_port);
void configure_nr_nfapi_vnf(char *vnf_addr, int vnf_p5_port, char *pnf_ip_addr, int pnf_p7_port, int vnf_p7_port);

#include "nfapi_nr_interface_scf.h"

/*
 * Node-sync helpers used by the open-nFAPI layer to share precise timing
 * information with the OAI-specific integration code. The time offset is
 * expressed in microseconds relative to the monotonic reference used for the
 * nfapi transmit_timestamp field.
 */
uint32_t oai_nfapi_get_time_offset(void);
void oai_nfapi_set_phy_time_offset(uint16_t phy_id, uint32_t link_delay_us, int32_t phy_time_offset_us);
void oai_vnf_update_timing_info(const nfapi_nr_timing_info_t *timing_info);

#endif /* NFAPI_VNF_H_ */
