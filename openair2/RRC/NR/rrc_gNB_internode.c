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

/*! \file rrc_gNB_internode.c
 * \brief rrc internode procedures for gNB
 * \author Raymond Knopp
 * \date 2019
 * \version 1.0
 * \company Eurecom
 * \email: raymond.knopp@eurecom.fr
 */
#ifndef RRC_GNB_INTERNODE_C
#define RRC_GNB_INTERNODE_C

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "OCTET_STRING.h"
#include "RRC/NR/nr_rrc_proto.h"
#include "T.h"
#include "asn_codecs.h"
#include "assertions.h"
#include "common/utils/T/T.h"
#include "constr_TYPE.h"
#include "executables/softmodem-common.h"
#include "nr_rrc_defs.h"
#include "uper_decoder.h"
#include "uper_encoder.h"
#include "x2ap_messages_types.h"

NR_CG_Config_t *generate_CG_Config(const NR_RRCReconfiguration_t *reconfig, const NR_RadioBearerConfig_t *rbconfig)
{
  NR_CG_Config_t *cg_Config = calloc(1, sizeof(*cg_Config));
  cg_Config->criticalExtensions.present = NR_CG_Config__criticalExtensions_PR_c1;
  cg_Config->criticalExtensions.choice.c1 = calloc(1,sizeof(*cg_Config->criticalExtensions.choice.c1));
  cg_Config->criticalExtensions.choice.c1->present = NR_CG_Config__criticalExtensions__c1_PR_cg_Config;
  cg_Config->criticalExtensions.choice.c1->choice.cg_Config = calloc(1,sizeof(NR_CG_Config_IEs_t));
  char buffer[1024];
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_RRCReconfiguration, NULL, (void *)reconfig, buffer, 1024);
  AssertFatal (enc_rval.encoded > 0, "ASN1 message encoding failed (%s, %jd)!\n",
               enc_rval.failed_type->name, enc_rval.encoded);
  cg_Config->criticalExtensions.choice.c1->choice.cg_Config->scg_CellGroupConfig = calloc(1,sizeof(OCTET_STRING_t));
  OCTET_STRING_fromBuf(cg_Config->criticalExtensions.choice.c1->choice.cg_Config->scg_CellGroupConfig,
                       (const char *)buffer,
                       (enc_rval.encoded+7)>>3);

  FILE *fd; // file to be generated for nr-ue
  if (get_softmodem_params()->phy_test==1 || get_softmodem_params()->do_ra > 0) {
    // This is for phytest only, emulate first X2 message if uecap.raw file is present
    LOG_I(RRC,"Dumping NR_RRCReconfiguration message (%jd bytes)\n",(enc_rval.encoded+7)>>3);
    for (int i=0; i<(enc_rval.encoded+7)>>3; i++) {
      printf("%02x",((uint8_t *)buffer)[i]);
    }
    printf("\n");
    fd = fopen("reconfig.raw","w");
    if (fd != NULL) {
      fwrite((void *)buffer,1,(size_t)((enc_rval.encoded+7)>>3),fd);
      fclose(fd);
    }
  }
  
  enc_rval = uper_encode_to_buffer(&asn_DEF_NR_RadioBearerConfig, NULL, (void *)rbconfig, buffer, 1024);
  AssertFatal (enc_rval.encoded > 0, "ASN1 message encoding failed (%s, %jd)!\n",
	       enc_rval.failed_type->name, enc_rval.encoded);
  cg_Config->criticalExtensions.choice.c1->choice.cg_Config->scg_RB_Config = calloc(1,sizeof(OCTET_STRING_t));
  
  OCTET_STRING_fromBuf(cg_Config->criticalExtensions.choice.c1->choice.cg_Config->scg_RB_Config,
		       (const char *)buffer,
		       (enc_rval.encoded+7)>>3);


  
  if (get_softmodem_params()->phy_test==1 || get_softmodem_params()->do_ra > 0) {

    LOG_I(RRC,"Dumping scg_RB_Config message (%jd bytes)\n",(enc_rval.encoded+7)>>3);
    for (int i=0; i<(enc_rval.encoded+7)>>3; i++) {
      printf("%02x",((uint8_t *)buffer)[i]);
    }
    
    printf("\n");
    fd = fopen("rbconfig.raw","w");
    if (fd != NULL) {
      fwrite((void *)buffer,1,(size_t)((enc_rval.encoded+7)>>3),fd);
      fclose(fd);
    }
  }
  
  return cg_Config;
}

#endif
