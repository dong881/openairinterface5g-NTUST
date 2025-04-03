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

/*! \file rrc_gNB_nsa.c
 * \brief rrc NSA procedures for gNB
 * \author Raymond Knopp
 * \date 2019
 * \version 1.0
 * \company Eurecom
 * \email: raymond.knopp@eurecom.fr
 */

#include <assert.h>
#include <assertions.h>
#include <openair2/RRC/NR/nr_rrc_proto.h>
#include <openair2/RRC/NR/rrc_gNB_UE_context.h>
#include <openair3/ocp-gtpu/gtp_itf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MESSAGES/asn1_msg.h"
#include "OCTET_STRING.h"
#include "RRC/NR/nr_rrc_config.h"
#include "T.h"
#include "asn_codecs.h"
#include "asn_internal.h"
#include "assertions.h"
#include "common/ngran_types.h"
#include "common/ran_context.h"
#include "common/utils/T/T.h"
#include "constr_TYPE.h"
#include "executables/nr-softmodem.h"
#include "executables/softmodem-common.h"
#include "gtpv1_u_messages_types.h"
#include "intertask_interface.h"
#include "ngap_messages_types.h"
#include "nr_pdcp/nr_pdcp_entity.h"
#include "nr_pdcp/nr_pdcp_oai_api.h"
#include "nr_rrc_defs.h"
#include "openair2/F1AP/f1ap_ids.h"
#include "openair2/LAYER2/NR_MAC_gNB/mac_proto.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include "openair3/SECU/key_nas_deriver.h"
#include "rrc_gNB_du.h"
#include "rlc.h"
#include "s1ap_messages_types.h"
#include "tree.h"
#include "uper_decoder.h"
#include "uper_encoder.h"
#include "x2ap_messages_types.h"
#include "xer_decoder.h"
#include "xer_encoder.h"

// In case of phy-test and do-ra mode, read UE capabilities directly from file
// and put it into a CG-ConfigInfo field
static int cg_config_info_from_ue_cap_file(uint32_t maxlen, uint8_t buf[maxlen])
{
  NR_CG_ConfigInfo_t *cgci = calloc_or_fail(1, sizeof(*cgci));
  cgci->criticalExtensions.present = NR_CG_ConfigInfo__criticalExtensions_PR_c1;
  cgci->criticalExtensions.choice.c1 = calloc_or_fail(1, sizeof(*cgci->criticalExtensions.choice.c1));
  cgci->criticalExtensions.choice.c1->present = NR_CG_ConfigInfo__criticalExtensions__c1_PR_cg_ConfigInfo;
  NR_CG_ConfigInfo_IEs_t *cgci_ies = calloc_or_fail(1, sizeof(*cgci_ies));
  cgci->criticalExtensions.choice.c1->choice.cg_ConfigInfo = cgci_ies;

  if (uecap_file != NULL) {
    LOG_I(NR_RRC, "creating CG-ConfigInfo from UE capability file %s\n", uecap_file);

    FILE *f = fopen(uecap_file, "r");
    if (!f) {
      LOG_E(NR_RRC, "cannot open file %s, cannot read UE capabilities\n", uecap_file);
      return 0;
    }
    char UE_NR_Capability_xer[65536];
    size_t size = fread(UE_NR_Capability_xer, 1, sizeof UE_NR_Capability_xer, f);
    fclose(f);
    if (size == 0 || size == sizeof UE_NR_Capability_xer) {
      LOG_E(NR_RRC, "UE Capabilities XER file %s could not be read (read %ld bytes)\n", uecap_file, size);
      return 0;
    }
    NR_UE_NR_Capability_t *cap = calloc_or_fail(1, sizeof(*cap));
    asn_dec_rval_t dec_rval = xer_decode(0, &asn_DEF_NR_UE_NR_Capability, (void *)&cap, UE_NR_Capability_xer, size);
    DevAssert(dec_rval.code == RC_OK);
    //xer_fprint(stdout, &asn_DEF_NR_UE_NR_Capability, cap);

    OCTET_STRING_t *os_cap = calloc_or_fail(1, sizeof(*os_cap));
    os_cap->size = uper_encode_to_new_buffer(&asn_DEF_NR_UE_NR_Capability, NULL, cap, (void **)&os_cap->buf);
    cgci_ies->ue_CapabilityInfo = os_cap;
  }

  //xer_fprint(stdout, &asn_DEF_NR_CG_ConfigInfo, cgci);
  asn_enc_rval_t rval = uper_encode_to_buffer(&asn_DEF_NR_CG_ConfigInfo, NULL, cgci, buf, maxlen);
  DevAssert(rval.encoded > 0);
  return (rval.encoded + 7) >> 3;
}

/* generate prototypes for the tree management functions (RB_INSERT used in rrc_add_nsa_user) */
RB_PROTOTYPE(rrc_nr_ue_tree_s, rrc_gNB_ue_context_s, entries,
             rrc_gNB_compare_ue_rnti_id);

void rrc_add_nsa_user(gNB_RRC_INST *rrc, x2ap_ENDC_sgnb_addition_req_t *m)
{
  AssertFatal(!IS_SA_MODE(get_softmodem_params()), "%s() cannot be called in SA mode, it is intrinsically for NSA\n", __func__);
  AssertFatal(NODE_IS_MONOLITHIC(rrc->node_type), "NSA, phy_test and do_ra only work in monolithic\n");

  sctp_assoc_t assoc_id = -1; /* integrated DU, hardcoded: if multiple we would not know what to do? */
  /* all the 0 are DU-related info that we will fill later, see rrc_add_nsa_user_resp() below */
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_create_ue_context(assoc_id, 0, rrc, 0, 0);
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;

  nr_pdcp_entity_security_keys_and_algos_t security_parameters = {0};

  uint8_t tmp[1024];
  uint8_t *cgci_buf = NULL;
  uint32_t cgci_len = 0;
  if (get_softmodem_params()->phy_test == 1 || get_softmodem_params()->do_ra == 1) {
    DevAssert(m == NULL);
    UE->rb_config = get_default_rbconfig(10 /* EPS bearer ID */, 1 /* drb ID */, NR_CipheringAlgorithm_nea0, NR_SecurityConfig__keyToUse_master);
    cgci_len = cg_config_info_from_ue_cap_file(sizeof tmp, tmp);
    DevAssert(cgci_len > 0);
    cgci_buf = tmp;
  } else {
    DevAssert(m != NULL);
    cgci_buf = m->rrc_buffer;
    cgci_len = m->rrc_buffer_size;

    /* TODO: handle more than one bearer */
    if (m->nb_e_rabs_tobeadded != 1) {
      LOG_E(RRC, "fatal: m->nb_e_rabs_tobeadded = %d, should be 1\n", m->nb_e_rabs_tobeadded);
      exit(1);
    }

    /* store security key and security capabilities */
    memcpy(UE->kgnb, m->kgnb, 32);
    UE->security_capabilities.nRencryption_algorithms = m->security_capabilities.encryption_algorithms;
    UE->security_capabilities.nRintegrity_algorithms = m->security_capabilities.integrity_algorithms;

    /* Select ciphering algorithm based on gNB configuration file and
     * UE's supported algorithms.
     * We take the first from the list that is supported by the UE.
     * The ordering of the list comes from the configuration file.
     */
    /* preset nea0 as fallback */
    UE->ciphering_algorithm = 0;
    for (int i = 0; i < rrc->security.ciphering_algorithms_count; i++) {
      int nea_mask[4] = {
        0,
        0x8000,  /* nea1 */
        0x4000,  /* nea2 */
        0x2000   /* nea3 */
      };
      if (rrc->security.ciphering_algorithms[i] == 0) {
        /* nea0 already preselected */
        break;
      }
      if (UE->security_capabilities.nRencryption_algorithms & nea_mask[rrc->security.ciphering_algorithms[i]]) {
        UE->ciphering_algorithm = rrc->security.ciphering_algorithms[i];
        break;
      }
    }

    LOG_I(RRC, "selecting ciphering algorithm %d\n", (int)UE->ciphering_algorithm);

    /* integrity: no integrity protection for DRB in ENDC mode
     * as written in 38.331: "If UE is connected to E-UTRA/EPC, this field
     * indicates the integrity protection algorithm to be used for SRBs
     * configured with NR PDCP, as specified in TS 33.501"
     * So nothing for DRB. Plus a test with a COTS UE revealed that it
     * did not apply integrity on the DRB.
     */
    UE->integrity_algorithm = 0;

    LOG_I(RRC, "selecting integrity algorithm %d\n", UE->integrity_algorithm);

    /* derive UP security key */
    security_parameters.ciphering_algorithm = UE->ciphering_algorithm;
    security_parameters.integrity_algorithm = UE->integrity_algorithm;
    nr_derive_key(UP_ENC_ALG, UE->ciphering_algorithm, UE->kgnb, security_parameters.ciphering_key);
    nr_derive_key(UP_INT_ALG, UE->integrity_algorithm, UE->kgnb, security_parameters.integrity_key);

    e_NR_CipheringAlgorithm cipher_algo;
    switch (UE->ciphering_algorithm) {
      case 0:
        cipher_algo = NR_CipheringAlgorithm_nea0;
        break;
      case 1:
        cipher_algo = NR_CipheringAlgorithm_nea1;
        break;
      case 2:
        cipher_algo = NR_CipheringAlgorithm_nea2;
        break;
      case 3:
        cipher_algo = NR_CipheringAlgorithm_nea3;
        break;
      default:
        LOG_E(RRC, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
        exit(1);
    }

    UE->rb_config = get_default_rbconfig(m->e_rabs_tobeadded[0].e_rab_id, m->e_rabs_tobeadded[0].drb_ID, cipher_algo, NR_SecurityConfig__keyToUse_secondary);
  }

  if(m!=NULL) {

    UE->x2_target_assoc = m->target_assoc_id;
    UE->MeNB_ue_x2_id = m->ue_x2_id;
    gtpv1u_enb_create_tunnel_req_t  create_tunnel_req = {0};
    gtpv1u_enb_create_tunnel_resp_t create_tunnel_resp = {0};
    if (m->nb_e_rabs_tobeadded>0) {
      for (int i=0; i<m->nb_e_rabs_tobeadded; i++) {
        // Add the new E-RABs at the corresponding rrc ue context of the gNB
        UE->e_rab[i].param.e_rab_id = m->e_rabs_tobeadded[i].e_rab_id;
        UE->e_rab[i].param.gtp_teid = m->e_rabs_tobeadded[i].gtp_teid;
        memcpy(&UE->e_rab[i].param.sgw_addr, &m->e_rabs_tobeadded[i].sgw_addr, sizeof(transport_layer_addr_t));
        UE->nb_of_e_rabs++;
        //Fill the required E-RAB specific information for the creation of the S1-U tunnel between the gNB and the SGW
        create_tunnel_req.eps_bearer_id[i] = UE->e_rab[i].param.e_rab_id;
        create_tunnel_req.sgw_S1u_teid[i] = UE->e_rab[i].param.gtp_teid;
        memcpy(&create_tunnel_req.sgw_addr[i], &UE->e_rab[i].param.sgw_addr, sizeof(transport_layer_addr_t));
        LOG_I(RRC,"S1-U tunnel: index %d target sgw ip %d.%d.%d.%d length %d gtp teid %u\n",
              i,
              create_tunnel_req.sgw_addr[i].buffer[0],
              create_tunnel_req.sgw_addr[i].buffer[1],
              create_tunnel_req.sgw_addr[i].buffer[2],
              create_tunnel_req.sgw_addr[i].buffer[3],
              create_tunnel_req.sgw_addr[i].length,
              create_tunnel_req.sgw_S1u_teid[i]);
      }

      create_tunnel_req.rnti = ue_context_p->ue_context.rrc_ue_id;
      create_tunnel_req.num_tunnels    = m->nb_e_rabs_tobeadded;
      RB_INSERT(rrc_nr_ue_tree_s, &RC.nrrrc[rrc->module_id]->rrc_ue_head, ue_context_p);
      if (!IS_SOFTMODEM_NOS1) {
        gtpv1u_create_s1u_tunnel(rrc->module_id, &create_tunnel_req, &create_tunnel_resp, nr_pdcp_data_req_drb);
        DevAssert(create_tunnel_resp.num_tunnels == 1);
        UE->nsa_gtp_teid[0] = create_tunnel_resp.enb_S1u_teid[0];
        UE->nsa_gtp_addrs[0] = create_tunnel_resp.enb_addr;
        UE->nsa_gtp_ebi[0] = create_tunnel_resp.eps_bearer_id[0];
      }
    }
  }

  DevAssert(UE->rb_config != NULL);
  nr_pdcp_add_drbs(GNB_FLAG_YES,
                   UE->rrc_ue_id,
                   UE->rb_config->drb_ToAddModList,
                   &security_parameters);

  cu_to_du_rrc_information_t cu2du = {
      .cG_ConfigInfo = cgci_buf,
      .cG_ConfigInfo_length = cgci_len,
  };
  /* assumption: only a single bearer, see above */
  NR_DRB_ToAddModList_t *rb_list = UE->rb_config->drb_ToAddModList;
  AssertFatal(rb_list->list.count == 1, "can only handle one bearer for NSA/phy-test/do-ra, but has %d\n", rb_list->list.count);
  f1ap_drb_to_be_setup_t drbs = {
      .drb_id = rb_list->list.array[0]->drb_Identity,
      .rlc_mode = F1AP_RLC_MODE_UM_BIDIR, // hardcoded keep it backwards compatible for now
      // rrc->configuration.um_on_default_drb ? F1AP_RLC_MODE_UM_BIDIR : F1AP_RLC_MODE_AM,
      .up_ul_tnl_length = 1,
  };
  f1ap_ue_context_setup_t req = {
      .gNB_CU_ue_id = UE->rrc_ue_id,
      .gNB_DU_ue_id = 0xffffffff, /* not known yet */
      .plmn.mcc = rrc->configuration.plmn[0].mcc,
      .plmn.mnc = rrc->configuration.plmn[0].mnc,
      .plmn.mnc_digit_length = rrc->configuration.plmn[0].mnc_digit_length,
      .nr_cellid = rrc->nr_cellid,
      .servCellId = 0,
      .drbs_to_be_setup_length = 1,
      .drbs_to_be_setup = &drbs,
      .cu_to_du_rrc_information = &cu2du,
  };
  f1_ue_data_t ue_data = cu_get_f1_ue_data(UE->rrc_ue_id);
  RETURN_IF_INVALID_ASSOC_ID(ue_data.du_assoc_id);
  rrc->mac_rrc.ue_context_setup_request(ue_data.du_assoc_id, &req);
}

void rrc_add_nsa_user_resp(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE, const f1ap_ue_context_setup_t *resp)
{
  DevAssert(resp->crnti != NULL);
  /* we did not fill any DU-related ID info in rrc_add_nsa_user() */
  UE->rnti = *resp->crnti;
  DevAssert(cu_exists_f1_ue_data(UE->rrc_ue_id));
  f1_ue_data_t ue_data = cu_get_f1_ue_data(UE->rrc_ue_id);
  ue_data.secondary_ue = resp->gNB_DU_ue_id;
  bool success = cu_update_f1_ue_data(UE->rrc_ue_id, &ue_data);
  DevAssert(success);

  NR_RRCReconfiguration_t *reconfig = calloc(1, sizeof(NR_RRCReconfiguration_t));
  reconfig->rrc_TransactionIdentifier = 0;
  reconfig->criticalExtensions.present = NR_RRCReconfiguration__criticalExtensions_PR_rrcReconfiguration;
  reconfig->criticalExtensions.choice.rrcReconfiguration = get_default_reconfig(UE->masterCellGroup);

  NR_CG_Config_t *CG_Config = generate_CG_Config(reconfig, UE->rb_config);

  if (get_softmodem_params()->phy_test > 0 || get_softmodem_params()->do_ra > 0) {
    /* we are done, no X2 answer necessary */
    ASN_STRUCT_FREE(asn_DEF_NR_CG_Config, CG_Config);
    return;
  }

  MessageDef *msg = itti_alloc_new_message(TASK_RRC_ENB, 0, X2AP_ENDC_SGNB_ADDITION_REQ_ACK);
  x2ap_ENDC_sgnb_addition_req_ACK_t *ack = &X2AP_ENDC_SGNB_ADDITION_REQ_ACK(msg);

  ack->nb_e_rabs_admitted_tobeadded = UE->nb_of_e_rabs;
  DevAssert(UE->x2_target_assoc > 0);
  ack->target_assoc_id = UE->x2_target_assoc;

  for (int i = 0; i < UE->nb_of_e_rabs; i++) {
    ack->e_rabs_admitted_tobeadded[i].e_rab_id = UE->e_rab[i].param.e_rab_id;
    ack->e_rabs_admitted_tobeadded[i].gtp_teid = UE->nsa_gtp_teid[0];
    memcpy(&ack->e_rabs_admitted_tobeadded[i].gnb_addr, &UE->nsa_gtp_addrs[0], sizeof(transport_layer_addr_t));
    ack->e_rabs_admitted_tobeadded[i].gnb_addr.length = 32; // bits, IPv4 only
  }

  ack->MeNB_ue_x2_id = UE->MeNB_ue_x2_id;
  ack->SgNB_ue_x2_id = UE->rrc_ue_id;

  // Send to X2 entity to transport to MeNB
  asn_enc_rval_t enc_rval =
      uper_encode_to_buffer(&asn_DEF_NR_CG_Config, NULL, (void *)CG_Config, ack->rrc_buffer, sizeof(ack->rrc_buffer));
  ack->rrc_buffer_size = (enc_rval.encoded + 7) >> 3;
  itti_send_msg_to_task(TASK_X2AP, ENB_MODULE_ID_TO_INSTANCE(0), msg);
  ASN_STRUCT_FREE(asn_DEF_NR_CG_Config, CG_Config);
}

void rrc_remove_nsa_user(gNB_RRC_INST *rrc, int rnti) {
  protocol_ctxt_t      ctxt;
  rrc_gNB_ue_context_t *ue_context;
  int                  e_rab;

  LOG_D(RRC, "calling rrc_remove_nsa_user rnti %x\n", rnti);
  PROTOCOL_CTXT_SET_BY_MODULE_ID(&ctxt, rrc->module_id, GNB_FLAG_YES, rnti, 0, 0, rrc->module_id);

  ue_context = rrc_gNB_get_ue_context_by_rnti_any_du(rrc, rnti);
  if (ue_context == NULL) {
    LOG_W(RRC, "rrc_remove_nsa_user: rnti %x not found\n", rnti);
    return;
  }

  nr_pdcp_remove_UE(ue_context->ue_context.rrc_ue_id);
  rrc_rlc_remove_ue(&ctxt);

  // lock the scheduler before removing the UE. Note: mac_remove_nr_ue() checks
  // that the scheduler is actually locked!
  NR_SCHED_LOCK(&RC.nrmac[rrc->module_id]->sched_lock);
  mac_remove_nr_ue(RC.nrmac[rrc->module_id], rnti);
  NR_SCHED_UNLOCK(&RC.nrmac[rrc->module_id]->sched_lock);
  gtpv1u_enb_delete_tunnel_req_t tmp={0};
  tmp.rnti=rnti;
  tmp.from_gnb=1;
  LOG_D(RRC, "ue_context->ue_context.nb_of_e_rabs %d will be deleted for rnti %x\n", ue_context->ue_context.nb_of_e_rabs, rnti);
  for (e_rab = 0; e_rab < ue_context->ue_context.nb_of_e_rabs; e_rab++) {
    tmp.eps_bearer_id[tmp.num_erab++]= ue_context->ue_context.nsa_gtp_ebi[e_rab];
    // erase data
    ue_context->ue_context.nsa_gtp_teid[e_rab] = 0;
    memset(&ue_context->ue_context.nsa_gtp_addrs[e_rab], 0, sizeof(ue_context->ue_context.nsa_gtp_addrs[e_rab]));
    ue_context->ue_context.nsa_gtp_ebi[e_rab] = 0;
  }
  gtpv1u_delete_all_s1u_tunnel(rrc->module_id, rnti);
  /* remove context */
  rrc_gNB_remove_ue_context(rrc, ue_context);
}
