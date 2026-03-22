# 5G NR Downlink Complete Flow Analysis: RLC → MAC → PHY

## **Executive Summary: 500µs Slot Budget Breakdown**

對於 30kHz SCS (Subcarrier Spacing)，每個 slot = **500 µs**

在這 500µs 內必須完成：
1. **MAC Scheduling (L2)**: ~50-100µs
2. **PHY Processing (L1)**: ~300-400µs
3. **RU/RF Transmission**: 剩餘時間

---

## **Part 1: Layer 2 Processing (RLC → MAC Scheduler)**

### **1.1 RLC Buffer Management**

**Location**: `openair2/LAYER2/nr_rlc/`

**數據來源鏈路**:
```
SDAP → PDCP → RLC → MAC
```

**RLC Entity Types**:
- **AM (Acknowledged Mode)**: 用於可靠傳輸 (DRB)
- **UM (Unacknowledged Mode)**: 用於實時流量
- **TM (Transparent Mode)**: 用於 SRB0

**Key Function**: `nr_rlc_entity_am_generate_pdu()`
- RLC 維護 **transmission buffer** (tx_list)
- 等待 MAC 請求時提供 SDU/PDU
- **不是實時處理** - 數據已經在 buffer 中等待

**Timeline**: RLC buffer 操作本身極快 (<1µs)，因為只是從已準備好的鏈表中取數據

---

### **1.2 MAC Scheduler Entry Point**

**Location**: `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler.c:163`

```c
void gNB_dlsch_ulsch_scheduler(module_id_t module_idP,
                                frame_t frame,
                                slot_t slot,
                                NR_Sched_Rsp_t *sched_info)
```

**每 slot 調用一次** (30kHz SCS = 500µs per slot)

**Scheduling Order** (Critical!):
```c
// Line 173: Lock scheduler
NR_SCHED_LOCK(&gNB->sched_lock);

// 1. Clear NFAPI structures (~5µs)
clear_nr_nfapi_information(gNB, CC_id, frame, slot, ...);

// 2. Broadcast channels
schedule_nr_mib(module_idP, frame, slot, &DL_req);           // ~2µs
schedule_nr_sib1(module_idP, frame, slot, &DL_req, &TX_req); // ~5µs

// 3. PRACH scheduling (prepare for UL)
schedule_nr_prach(module_idP, frame, slot);                   // ~3µs

// 4. CSI-RS scheduling
nr_csirs_scheduling(module_idP, frame, slot, &DL_req);        // ~5µs

// 5. Random Access
nr_schedule_RA(module_idP, frame, slot, ...);                 // ~10µs

// 6. **UL SCHEDULING FIRST** (Line 248-250)
nr_schedule_ulsch(module_idP, frame, slot, &UL_dci_req);      // ~20-30µs

// 7. **DL SCHEDULING SECOND** (Line 253-255)
nr_schedule_ue_spec(module_idP, frame, slot, &DL_req, &TX_req); // ~30-50µs

// 8. PUCCH scheduling (last)
nr_schedule_pucch(gNB, frame, slot);                          // ~5µs

// Unlock
NR_SCHED_UNLOCK(&gNB->sched_lock);
```

**Total MAC Scheduler Time**: **~50-100µs**

---

### **1.3 Downlink Scheduling Details**

**Location**: `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_dlsch.c:1006`

```c
void nr_schedule_ue_spec(module_id_t module_id,
                         frame_t frame,
                         slot_t slot,
                         nfapi_nr_dl_tti_request_t *DL_req,
                         nfapi_nr_tx_data_request_t *TX_req)
```

**Step 1: Preprocessing** (決定資源分配)
```c
gNB->pre_processor_dl(module_id, frame, slot);  // ~10µs
```

計算:
- 可用 RB (Resource Blocks) 數量
- HARQ retransmission 需求
- UE buffer status

**Step 2: Proportional Fair Scheduling** (`pf_dl()`)

**Location**: `gNB_scheduler_dlsch.c:601`

```c
static void pf_dl(module_id_t module_id, frame_t frame, slot_t slot,
                  NR_UE_info_t **UE_list, int max_num_ue,
                  int num_beams, int n_rb_sched[])
{
  // For each UE, calculate PF coefficient
  for (UE in connected_ue_list) {
    // Proportional Fair metric
    coeff_ue = (TBS / time_window) / dl_thr_ue;

    // Check retransmissions first
    if (harq_pid >= 0) {
      allocate_retransmission(UE, harq_pid);
    }
  }

  // Sort UEs by coefficient (highest first)
  qsort(UE_sched, max_num_ue, sizeof(UEsched_t), comparator);

  // Allocate resources to top UEs
  for (UE in sorted_list) {
    if (n_rb_sched > 0) {
      allocate_dlsch_buffer(UE, TBS, MCS, RBs);
    }
  }
}
```

**Timeline**: ~20-30µs for 10 UEs

---

### **1.4 MAC PDU Assembly (從 RLC 獲取數據)**

**Location**: `gNB_scheduler_dlsch.c:1254-1349`

**Critical Section** (Line 1268-1348):

```c
// Start timing measurement
start_meas(&gNB_mac->rlc_data_req);

if (sched_ctrl->num_total_bytes > 0) {
  // Loop over all logical channels
  for (int i = 0; i < seq_arr_size(&sched_ctrl->lc_config); ++i) {
    const nr_lc_config_t *c = seq_arr_at(&sched_ctrl->lc_config, i);
    const int lcid = c->lcid;

    // Check if this LC has data
    if (sched_ctrl->rlc_status[lcid].bytes_in_buffer == 0)
      continue;

    while (bufEnd - buf > sizeof(NR_MAC_SUBHEADER_LONG) + 1) {
      // Calculate how much data to request from RLC
      const rlc_buffer_occupancy_t ndata =
        min(sched_ctrl->rlc_status[lcid].bytes_in_buffer,
            bufEnd - buf - sizeof(NR_MAC_SUBHEADER_LONG));

      // **KEY FUNCTION: GET DATA FROM RLC**
      tbs_size_t len = nr_mac_rlc_data_req(module_id,
                                           rnti,
                                           true,
                                           lcid,
                                           ndata,
                                           (char *)buf + sizeof(NR_MAC_SUBHEADER_LONG));

      if (len == 0) break;

      // Build MAC subheader
      header->R = 0;
      header->F = 1;
      header->LCID = lcid;
      header->L = htons(len);

      buf += len + sizeof(NR_MAC_SUBHEADER_LONG);
      dlsch_total_bytes += len;
      sdus += 1;
    }
  }
}

stop_meas(&gNB_mac->rlc_data_req);
```

**RLC Data Request Function** (`nr_rlc_oai_api.c:193`):

```c
tbs_size_t nr_mac_rlc_data_req(const module_id_t module_idP,
                               const uint16_t ue_id,
                               const bool gnb_flagP,
                               const logical_chan_id_t channel_idP,
                               const tb_size_t tb_sizeP,
                               char *buffer_pP)
{
  nr_rlc_manager_lock(nr_rlc_ue_manager);

  // Find RLC entity for this UE/LCID
  nr_rlc_entity_t *rb = nr_rlc_manager_get_rlc_entity(
    nr_rlc_ue_manager, ue_id, channel_idP);

  if (rb == NULL) {
    nr_rlc_manager_unlock(nr_rlc_ue_manager);
    return 0;
  }

  // Get max size from RLC entity
  maxsize = rb->get_buffer_occupancy(rb);

  // Generate PDU from RLC buffer
  ret = rb->generate_pdu(rb, buffer_pP,
                         (tb_sizeP < maxsize) ? tb_sizeP : maxsize);

  nr_rlc_manager_unlock(nr_rlc_ue_manager);
  return ret;
}
```

**Timeline for MAC PDU Assembly**:
- **Per UE**: ~5-10µs
- **10 UEs with data**: ~50-100µs
- **RLC buffer copy**: Memcpy bandwidth limited (~10 GB/s)

---

### **1.5 NFAPI Structure Population**

**Output Data Structures**:

```c
// DL_tti_request: PHY 需要知道的控制信息
nfapi_nr_dl_tti_request_t DL_req = {
  .SFN = frame,
  .Slot = slot,
  .dl_tti_request_body = {
    .nPDUs = num_pdsch + num_pdcch + num_ssb + num_csirs,
    .nGroup = 0
  }
};

// TX_data_request: 實際的 MAC PDU 數據
nfapi_nr_tx_data_request_t TX_req = {
  .Number_of_PDUs = num_pdsch,
  .pdu_list[i] = {
    .PDU_length = TBS,
    .PDU_index = pdu_index,
    .TLVs[0].value.direct = harq->transportBlock  // 指向 MAC PDU buffer
  }
};
```

**Timeline**: ~5µs (只是設置指針和元數據)

---

## **Part 2: Layer 1 Processing (PHY TX)**

### **2.1 PHY Entry Point**

**Location**: `openair1/SCHED_NR/phy_procedures_nr_gNB.c:249`

```c
void phy_procedures_gNB_TX(processingData_L1tx_t *msgTx,
                           int frame,
                           int slot,
                           int do_meas)
```

**Input Data Structure**:

```c
typedef struct processingData_L1tx {
  int frame, slot;                        // Timing
  PHY_VARS_gNB *gNB;                     // PHY config

  // From MAC scheduler:
  nfapi_nr_dl_tti_pdcch_pdu pdcch_pdu[]; // Control (DCI)
  NR_gNB_DLSCH_t **dlsch;                // Data (PDSCH)
  NR_gNB_SSB_t ssb[64];                  // Sync signals
  NR_gNB_CSIRS_t csirs_pdu[];            // Reference signals

  uint16_t num_pdsch_slot;               // PDSCH count this slot
  uint16_t num_ul_pdcch;                 // UL DCI count
  uint16_t num_dl_pdcch;                 // DL DCI count
} processingData_L1tx_t;
```

---

### **2.2 PHY Processing Pipeline** (Sequential)

**Current Implementation** (NOT parallelized):

```c
void phy_procedures_gNB_TX(processingData_L1tx_t *msgTx, ...) {

  // **STEP 1: Memory Initialization** (Lines 284-291)
  // ⏱️ ~50µs (4 antennas × 273 PRBs × 14 symbols × 12 subcarriers)
  enkits_pool_memclear_tx((void***)gNB->common_vars.txdataF,
                          num_beams,
                          num_tx_ant,
                          txdataF_offset,
                          fp->samples_per_slot_wCP);

  // **STEP 2: PRS Generation** (Lines 295-309)
  // ⏱️ ~5µs (if enabled, rare in deployment)
  for (int rsc_id = 0; rsc_id < NumPRSResources; rsc_id++) {
    nr_generate_prs(slot_prs, &txdataF[0][0][offset], AMP, prs_config, ...);
  }

  // **STEP 3: SSB Generation** (Lines 314-322)
  // ⏱️ ~10-20µs per SSB block (PSS/SSS/PBCH/DMRS)
  for (int i = 0; i < fp->Lmax; i++) {
    if (msgTx->ssb[i].active) {
      nr_common_signal_procedures(gNB, frame, slot, msgTx->ssb[i].ssb_pdu);
    }
  }

  // **STEP 4: PDCCH Generation** (Lines 327-339)
  // ⏱️ ~20-30µs (Polar encoding + CCE mapping + DMRS)
  if (num_pdcch_pdus > 0) {
    nr_generate_dci_top(msgTx, slot, txdataF_offset);
  }

  // **STEP 5: PDSCH Generation** (Lines 344-354)
  // ⏱️ **200-300µs** (BOTTLENECK!)
  if (msgTx->num_pdsch_slot > 0) {
    nr_generate_pdsch(msgTx, frame, slot);
  }

  // **STEP 6: CSI-RS Generation** (Lines 358-399)
  // ⏱️ ~15-25µs per CSI-RS configuration
  for (int i = 0; i < NR_SYMBOLS_PER_SLOT; i++) {
    if (csirs_pdu[i].active == 1) {
      nr_generate_csi_rs(&gNB->frame_parms, ...);
    }
  }

  // **STEP 7: Phase Rotation** (Lines 404-425)
  // ⏱️ ~10-15µs (per-symbol frequency offset compensation)
  for (int aa = 0; aa < num_tx_ant; aa++) {
    apply_nr_rotation_TX(&txdataF[beam][aa][offset], ...);
  }
}
```

**Total PHY Time**: **~300-400µs** (dominated by PDSCH)

---

### **2.3 PDSCH Processing Deep Dive**

**Location**: `openair1/PHY/NR_TRANSPORT/nr_dlsch.c`

**Function**: `nr_generate_pdsch(processingData_L1tx_t *msgTx, int frame, int slot)`

**PDSCH Pipeline** (per UE):

```c
void nr_generate_pdsch(processingData_L1tx_t *msgTx, ...) {

  for (int i = 0; i < msgTx->num_pdsch_slot; i++) {
    NR_gNB_DLSCH_t *dlsch = msgTx->dlsch[i];

    // **SUBSTEP 5.1: LDPC Encoding**
    // ⏱️ ~100-150µs (WITH ACC100 hardware acceleration!)
    // ⏱️ ~500-800µs (WITHOUT ACC100 - software LDPC)
    nr_dlsch_encoding(dlsch->harq_processes[harq_pid],
                      rel15,
                      gNB,
                      frame,
                      slot);

    // **SUBSTEP 5.2: Scrambling**
    // ⏱️ ~20-30µs
    nr_pdsch_codeword_scrambling(encoded_bits,
                                 total_bits,
                                 0, // q
                                 Nid_cell,
                                 rnti,
                                 scrambled_output);

    // **SUBSTEP 5.3: Modulation**
    // ⏱️ ~30-40µs (QPSK/16QAM/64QAM/256QAM)
    nr_modulation(scrambled_output,
                  total_bits,
                  rel15->qamModOrder[0],
                  mod_symbols);

    // **SUBSTEP 5.4: Layer Mapping**
    // ⏱️ ~5-10µs (distribute across MIMO layers)
    nr_layer_mapping(mod_symbols,
                     rel15->nrOfLayers,
                     total_symbols,
                     tx_layers);

    // **SUBSTEP 5.5: Precoding**
    // ⏱️ ~40-60µs (beamforming matrix multiplication)
    nr_ue_spec_beamforming(tx_layers,
                           precoding_matrix,
                           rel15->nrOfLayers,
                           num_tx_ant,
                           precoded_symbols);

    // **SUBSTEP 5.6: RE Mapping**
    // ⏱️ ~30-50µs (map to OFDM grid, avoid DMRS/PTRS/CSI-RS)
    nr_pdsch_resource_mapping(precoded_symbols,
                              pdsch_dmrs,
                              rel15,
                              txdataF);
  }
}
```

**PDSCH Timing Breakdown** (single UE, 273 PRBs, MCS 28):
- **Encoding (ACC100)**: 100µs
- **Scrambling**: 25µs
- **Modulation**: 35µs
- **Layer Mapping**: 8µs
- **Precoding**: 50µs
- **RE Mapping**: 40µs
- **Total**: **~258µs per UE**

**Multiple UEs**:
- 2 UEs: ~300µs (some overhead amortization)
- 4 UEs: ~400µs

---

### **2.4 LDPC Encoding with ACC100**

**Location**: `openair1/PHY/CODING/nrLDPC_encoder/ldpc_generate_coefficient.c`

**Software Path** (NO ACC100):
```c
nr_dlsch_encoding()
  └─> nrLDPC_encoding_segment()  // Software LDPC
        └─> ldpc_encode()        // ⏱️ ~500-800µs (CPU intensive)
```

**Hardware Path** (WITH ACC100):
```c
nr_dlsch_encoding()
  └─> gNB->nrLDPC_coding_interface.nrLDPC_coding_encoder()
        └─> dpdk_bbdev_enqueue()  // ⏱️ ~100-150µs
              └─> ACC100 hardware processing
```

**ACC100 Configuration** (from deployment):
```bash
--loader.ldpc.shlibversion _aal
--nrLDPC_coding_aal.dpdk_dev 0000:87:00.0
--nrLDPC_coding_aal.dpdk_core_list 16-17
--nrLDPC_coding_aal.vfio_vf_token c2d9f0a2-bc24-4a83-8126-9fbb22f3ce12
--nrLDPC_coding_aal.eal_init_bbdev 1
```

**Impact**:
- ACC100 saves **400-650µs per slot**
- Critical for meeting 500µs budget with multiple UEs

---

### **2.5 Output: Frequency Domain Samples**

**Data Structure**:
```c
// Output array: gNB->common_vars.txdataF
int32_t **txdataF[num_beams][num_tx_ant][samples_per_slot]

// For 30kHz SCS, 273 PRBs:
// samples_per_slot = 14 symbols × 273 RBs × 12 subcarriers
//                  = 45,864 complex samples per antenna
// Data type: int16_t (I) + int16_t (Q) = 32 bits per sample
// Total size: 45,864 × 4 bytes × 4 antennas × 1 beam = ~732 KB
```

**Memory Layout**:
```
txdataF[beam][antenna][symbol_offset + RB_offset + subcarrier]
```

---

## **Part 3: Radio Unit Processing**

### **3.1 O-RAN FHI 7.2 Fronthaul** (Current Deployment)

**Location**: `openair1/SCHED_NR/nr_ru_procedures.c`

**Function**: `nr_feptx_prec()` + O-RAN library

```c
// Copy frequency domain data to RU buffers
nr_feptx_prec(ru, frame_tx, slot_tx);

// O-RAN FHI 7.2 transmission (handled by xRAN library)
oran_fh_if4p5_south_out(ru, frame_tx, slot_tx);
  └─> xran_fh_tx_send_slot()
        └─> DPDK Ethernet transmission to Liteon RU
```

**Liteon RU** (External Hardware):
- Receives O-RAN packets
- Performs OFDM modulation (IFFT + Cyclic Prefix)
- RF up-conversion
- Antenna transmission

**Timeline**:
- **DU processing**: ~10-20µs (copy + packetization)
- **Fronthaul latency**: ~50-100µs (1G/10G Ethernet)
- **RU IFFT**: ~20-30µs (hardware)

---

## **Part 4: Complete Timeline Summary**

### **500µs Slot Budget Breakdown** (30kHz SCS)

```
┌─────────────────────────────────────────────────────────────┐
│ Slot N: 500µs Total Budget                                  │
├─────────────────────────────────────────────────────────────┤
│ T+0µs    : Slot boundary                                    │
│ T+5µs    : MAC scheduler starts                             │
│   ├─ Clear structures (5µs)                                 │
│   ├─ MIB/SIB1 (7µs)                                        │
│   ├─ PRACH/CSI-RS/RA (18µs)                               │
│   ├─ UL Scheduling (25µs)                                  │
│   ├─ DL Scheduling (pf_dl) (30µs)                         │
│   └─ RLC data request (50µs)                              │
│ T+140µs  : MAC scheduler completes                          │
│          : processingData_L1tx_t ready for PHY              │
├─────────────────────────────────────────────────────────────┤
│ T+140µs  : phy_procedures_gNB_TX starts                     │
│   ├─ Memory clear (50µs)                                   │
│   ├─ PRS generation (5µs, if active)                      │
│   ├─ SSB generation (15µs, periodic)                      │
│   ├─ PDCCH generation (25µs)                              │
│   ├─ **PDSCH generation (250µs)** ⚠️ BOTTLENECK            │
│   │    ├─ LDPC encoding (100µs with ACC100)               │
│   │    ├─ Scrambling (25µs)                               │
│   │    ├─ Modulation (35µs)                               │
│   │    ├─ Layer mapping (8µs)                             │
│   │    ├─ Precoding (50µs)                                │
│   │    └─ RE mapping (40µs)                               │
│   ├─ CSI-RS generation (20µs)                             │
│   └─ Phase rotation (12µs)                                │
│ T+517µs  : phy_procedures_gNB_TX completes ⚠️ OVER BUDGET!  │
├─────────────────────────────────────────────────────────────┤
│ T+520µs  : O-RAN fronthaul + RU processing (50µs)          │
│ T+570µs  : RF transmission begins                           │
│          : ⚠️ **70µs LATE** - slot N+1 already started!     │
└─────────────────────────────────────────────────────────────┘
```

### **Critical Path Analysis**

**Current Total Latency**: **~570µs**
- MAC: 140µs
- PHY: 377µs (measured average from CSV)
- RU: 50µs

**Budget Overrun**: **+70µs** (14% over budget)

**Bottleneck**: PDSCH generation (250µs = 66% of PHY time)

---

## **Part 5: Parallelization Opportunities**

### **5.1 Data Dependencies Analysis**

**HARD DEPENDENCIES** (Must be sequential):
1. **Memory Clear FIRST** - All other steps write to txdataF
2. **Phase Rotation LAST** - Operates on final txdataF content

**NO DEPENDENCIES** (Can run in parallel):
- PRS, SSB, PDCCH, PDSCH, CSI-RS generation
- Each writes to **orthogonal Resource Elements** (guaranteed by 3GPP spec)
- Multi-antenna processing (independent per antenna)
- Multi-beam processing (independent per beam)

### **5.2 enkiTS Thread Pool Integration**

**Available Resources**:
- **8 worker threads** (cores 6,7,8,9,10,11,13,14)
- **Work-stealing scheduler** (zero-lock, automatic load balancing)

**Parallelization Strategy**:

```c
void phy_procedures_gNB_TX_parallel(processingData_L1tx_t *msgTx, ...) {

  // Phase 1: Memory initialization (parallel across antennas/beams)
  enkits_pool_memclear_tx(...);  // ⏱️ 50µs → 10µs (5× speedup)

  // Phase 2: ALL signals in parallel
  enkiTaskSet *tasks[5];

  tasks[0] = enkiCreateTaskSet(scheduler, prs_generation_task);
  tasks[1] = enkiCreateTaskSet(scheduler, ssb_generation_task);
  tasks[2] = enkiCreateTaskSet(scheduler, pdcch_generation_task);
  tasks[3] = enkiCreateTaskSet(scheduler, pdsch_generation_task);
  tasks[4] = enkiCreateTaskSet(scheduler, csirs_generation_task);

  // Submit all tasks
  for (int i = 0; i < 5; i++) {
    enkiAddTaskSet(scheduler, tasks[i]);
  }

  // Wait for all to complete
  for (int i = 0; i < 5; i++) {
    enkiWaitForTaskSet(scheduler, tasks[i]);
  }

  // Phase 3: Phase rotation (parallel across antennas)
  enkiTaskSet *rotation_task = enkiCreateTaskSet(scheduler, phase_rotation_task);
  enkiAddTaskSetMinRange(scheduler, rotation_task, data, num_antennas, 1);
  enkiWaitForTaskSet(scheduler, rotation_task);
}
```

**Expected Performance**:

```
Sequential Timeline:
Memory (50µs) → PRS (5µs) → SSB (15µs) → PDCCH (25µs) →
PDSCH (250µs) → CSI-RS (20µs) → Rotation (12µs) = 377µs

Parallel Timeline:
Memory (10µs) → [All signals in parallel: max(250µs)] → Rotation (3µs) = 263µs

Speedup: 377µs → 263µs = **1.43× improvement**
Remaining budget: 500µs - 140µs(MAC) - 263µs(PHY) - 50µs(RU) = **+47µs margin**
```

---

## **Part 6: Recommendations**

### **Immediate Actions** (enkiTS already deployed):

1. **Parallelize Signal Generation**:
   - Implement task-based PRS/SSB/PDCCH/PDSCH/CSI-RS
   - Use work-stealing for load balancing
   - Target: 377µs → 263µs

2. **Optimize PDSCH Pipeline**:
   - Parallelize per-layer precoding
   - Vectorize RE mapping with AVX512
   - Target: 250µs → 180µs

3. **Profile ACC100 Usage**:
   - Verify LDPC encoding is actually using hardware
   - Check for unnecessary software fallbacks
   - Target: Maintain 100µs encoding time

### **System Configuration**:

```bash
# Verify ACC100 is active
lspci | grep acc100

# Check DPDK binding
dpdk-devbind.py --status

# Monitor thread pool utilization
perf top -p $(pgrep nr-softmodem)

# Real-time priority for L1_tx_thread
chrt -f -p 98 <L1_tx_thread_pid>
```

---

## **Conclusion**

**在 500µs slot budget 內必須完成**:
1. ✅ **RLC → MAC (140µs)**: 從 buffer 取數據 + 調度決策
2. ⚠️ **MAC → PHY (377µs)**: 頻域信號生成 (當前超預算)
3. ✅ **PHY → RU (50µs)**: O-RAN 傳輸 + IFFT

**關鍵瓶頸**: PDSCH generation (250µs)
**解決方案**: enkiTS parallelization (已部署，待實施)
**預期改善**: 377µs → 263µs (114µs saved, 22.8% faster)
