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

/*! \file nr_mac_extern.h
* \brief NR mac externs
* \author  Navid Nikaein, Raymond Knopp, Guido Casati
* \date 2019
* \version 1.0
* \email navid.nikaein@eurecom.fr, guido.casati@iis.fraunhofer.de
* @ingroup _mac

*/

#ifndef __NR_MAC_EXTERN_H__
#define __NR_MAC_EXTERN_H__

#include "common/ran_context.h"
#include "nr_mac.h"

/*#include "PHY/defs_common.h"*/

/* Scheduler */
extern RAN_CONTEXT_t RC;
extern uint8_t nfapi_mode;

extern const uint32_t NR_SHORT_BSR_TABLE[NR_SHORT_BSR_TABLE_SIZE];
extern const uint32_t NR_LONG_BSR_TABLE[NR_LONG_BSR_TABLE_SIZE];

extern const char table_38211_6_3_1_5_1[6][2][1];
extern const char table_38211_6_3_1_5_2[28][4][1];
extern const char table_38211_6_3_1_5_3[28][4][1];
extern const char table_38211_6_3_1_5_4[3][2][2];
extern const char table_38211_6_3_1_5_5[22][4][2];
#endif //DEF_H
