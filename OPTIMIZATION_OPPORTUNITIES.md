# 下行優化機會分析

基於對現有代碼的深入分析，以下是可進一步優化的機會。

## 目錄

1. [當前瓶頸分析](#1-當前瓶頸分析)
2. [信號生成並行化](#2-信號生成並行化-prs-ssb-csi-rs-pdcch)
3. [時序重排優化](#3-時序重排優化)
4. [相位旋轉並行化](#4-相位旋轉並行化)
5. [Gold序列預計算](#5-gold序列預計算)
6. [QPSK/16QAM/64QAM融合優化](#6-qpsk16qam64qam融合優化)
7. [記憶體存取優化](#7-記憶體存取優化)
8. [PDCCH生成優化](#8-pdcch生成優化)
9. [優先級排序](#9-優先級排序)

---

## 1. 當前瓶頸分析

根據 `l1_slot_timing.csv` 數據分析:

### 空閒 slot (無 PDSCH)
```
memclear_wait_ns: 20-70 μs (很高!)
fh_south_out_ns: 40-70 μs (主要瓶頸)
phy_proc_total_ns: 25-45 μs
```

### 問題發現

1. **memclear_wait 過高**: 即使沒有 PDSCH，memclear_wait 仍然有 20-70μs，這說明異步記憶體清除沒有被有效隱藏。

2. **信號生成串行執行**: PRS → SSB → PDCCH → PDSCH → CSI-RS 是串行的，但它們寫入不同的 RE，可以並行。

3. **FH South Out 佔主導**: RU TX 時間主要在 `fh_south_out`，已經並行化到 56 個任務。

---

## 2. 信號生成並行化 (PRS, SSB, CSI-RS, PDCCH)

### 現狀分析

**位置**: `openair1/SCHED_NR/phy_procedures_nr_gNB.c` lines 292-413

當前處理順序:
```c
// 3. PRS Generation
for (rsc_id...) nr_generate_prs(...);  // 串行

// 4. SSB Generation
for (i = 0; i < fp->Lmax; i++) nr_common_signal_procedures(...);  // 串行

// 5. PDCCH Generation
nr_generate_dci_top(...);  // 串行

// 6. PDSCH Codeword Phase
nr_pdsch_codeword_phase(...);  // 已優化

// 7. CSI-RS Generation
for (i...) nr_generate_csi_rs(...);  // 串行
```

### 優化方案

**3GPP 保證**: PRS、SSB、PDCCH、PDSCH、CSI-RS 寫入不同的 RE，可以完全並行。

```c
// 優化後: 並行啟動所有信號生成
typedef struct {
    int signal_type;  // PRS=0, SSB=1, PDCCH=2, PDSCH=3, CSIRS=4
    void *params;
    c16_t *txdataF;
    int txdataF_offset;
} signal_gen_task_t;

// 創建所有信號生成任務
int num_tasks = 0;
signal_gen_task_t tasks[MAX_SIGNALS];

// Add PRS tasks
for (rsc_id = 0; ...) tasks[num_tasks++] = {PRS, prs_cfg, ...};

// Add SSB tasks
for (i = 0; i < fp->Lmax; i++) {
    if (msgTx->ssb[i].active) tasks[num_tasks++] = {SSB, &ssb_pdu, ...};
}

// Add PDCCH task
if (num_pdcch_pdus > 0) tasks[num_tasks++] = {PDCCH, msgTx, ...};

// Add PDSCH task
if (pdsch_count > 0) tasks[num_tasks++] = {PDSCH, msgTx, ...};

// Add CSI-RS tasks
for (i...) if (csirs->active) tasks[num_tasks++] = {CSIRS, csirs, ...};

// 並行執行所有任務
enkiAddTaskSetMinRange(scheduler, g_signal_gen_task, tasks, num_tasks, 1);
enkiWaitForTaskSet(scheduler, g_signal_gen_task);
```

### 預期收益

| 信號 | 當前時間 | 並行後 |
|------|---------|--------|
| PRS | ~35-40 μs | 並行 |
| SSB | ~45-50 μs | 並行 |
| PDCCH | ~0 μs (無數據時) | 並行 |
| PDSCH | ~200+ μs | 並行 |
| CSI-RS | ~60-80 μs | 並行 |

**總體**: 串行 ~350 μs → 並行 ~200 μs (最長單一信號時間)

---

## 3. 時序重排優化

### 當前問題

異步 memclear 在無 PDSCH 時效果差:

```c
// 當前流程
enkits_pool_memclear_tx_async();  // 開始異步清除
if (pdsch_count > 0) {
    nr_pdsch_encoding_phase();    // 只有有 PDSCH 才能隱藏 memclear
}
enkits_pool_memclear_tx_wait();   // 無 PDSCH 時立即等待，浪費時間!
```

### 優化方案 A: 提前計算 Gold 序列

```c
// 優化後: 在 memclear 期間預計算 Gold 序列
enkits_pool_memclear_tx_async();

// 預計算所有需要的 Gold 序列 (與 memclear 並行)
if (prs_active) gold_prs = nr_gold_prs(...);
if (ssb_active) {
    gold_pss = ...;
    gold_sss = ...;
    gold_pbch_dmrs = nr_gold_pbch(...);
}
if (pdcch_active) gold_pdcch = nr_gold_pdcch(...);
if (pdsch_count > 0) nr_pdsch_encoding_phase();  // LDPC 編碼
if (csirs_active) gold_csirs = nr_gold_csi_rs(...);

enkits_pool_memclear_tx_wait();  // 現在 memclear 被計算時間隱藏
```

### 優化方案 B: 重新排列相位旋轉

相位旋轉只需要所有信號生成完成後才執行，可以考慮:

```c
// 當前: 相位旋轉在最後，是關鍵路徑

// 優化: 如果有些天線不參與傳輸，可以提前開始它們的相位旋轉
// (需要仔細分析可行性)
```

---

## 4. 相位旋轉並行化

### 現狀分析

**位置**: `phy_procedures_nr_gNB.c` lines 419-477

```c
for (int i = 0; i < num_beams_period; ++i) {
  for (int sym = 0; sym < nsymb; sym++) {
    for (int aa = 0; aa < active_antennas; aa++) {
      rotate_cpx_vector(...);  // 每個符號每個天線串行處理
      rotate_cpx_vector(...);  // 兩次調用 (正負頻率)
    }
  }
}
```

### 優化方案

```c
// 每個 (beam, symbol, antenna) 組合是獨立的
// 總任務數: num_beams × nsymb × num_antennas = 1 × 14 × 4 = 56 任務

typedef struct {
    c16_t *txdataF;
    const c16_t *rotation;
    int sym_offset;
    int first_carrier_offset;
    int nb_rb;
} phase_rot_task_t;

// 創建任務
phase_rot_task_t rot_tasks[56];
int task_idx = 0;
for (int i = 0; i < num_beams_period; ++i) {
    for (int sym = 0; sym < nsymb; sym++) {
        for (int aa = 0; aa < active_antennas; aa++) {
            rot_tasks[task_idx++] = {
                .txdataF = &gNB->common_vars.txdataF[i][aa][sym_offset],
                .rotation = rot_base + symb_offset_base + sym,
                ...
            };
        }
    }
}

// 並行執行
enkiAddTaskSetMinRange(scheduler, g_phase_rot_task, rot_tasks, task_idx, 1);
enkiWaitForTaskSet(scheduler, g_phase_rot_task);
```

### 預期收益

- 當前: ~40-90 μs (串行 14 符號 × 4 天線)
- 並行後: ~10-15 μs (56 任務分配到 8 workers)

---

## 5. Gold 序列預計算

### 現狀分析

多個函數重複計算 Gold 序列:
- `nr_gold_prs()` - PRS
- `nr_gold_pdcch()` - PDCCH DMRS
- `nr_gold_pdsch()` - PDSCH DMRS
- `nr_gold_csi_rs()` - CSI-RS
- `nr_gold_pbch()` - PBCH DMRS

### 優化方案

在 slot 開始時，根據需要生成的信號類型，預計算所有需要的 Gold 序列:

```c
// 在 phy_procedures_gNB_TX() 開頭，與 async memclear 並行
typedef struct {
    uint32_t *gold_prs[MAX_PRS_RESOURCES];
    uint32_t *gold_pdcch[MAX_CORESET_SYMBOLS];
    uint32_t *gold_pdsch[MAX_DMRS_SYMBOLS];
    uint32_t *gold_csirs[MAX_CSIRS_SYMBOLS];
    uint32_t *gold_pbch;
} slot_gold_cache_t;

// 預計算 (可以並行執行)
enkits_pool_memclear_tx_async();

// 並行計算所有 Gold 序列
precompute_gold_sequences(&gold_cache, frame, slot, ...);

nr_pdsch_encoding_phase();  // LDPC 編碼

enkits_pool_memclear_tx_wait();

// 後續信號生成使用預計算的 Gold 序列
nr_generate_prs(..., gold_cache.gold_prs[rsc_id]);
```

### 預期收益

- 消除信號生成中的 Gold 序列計算開銷
- 更好地利用異步 memclear 的等待時間

---

## 6. QPSK/16QAM/64QAM 融合優化

### 現狀分析

256QAM 已經實現融合調製+層映射，但 QPSK/16QAM/64QAM 仍然是分離的:

```c
if (use_fused_mod_layer) {  // 只有 256QAM
    nr_modulate_layer_map_256qam_parallel(...);
} else {
    // QPSK/16QAM/64QAM: 分離的調製和層映射
    nr_modulation(...);          // 調製
    nr_layer_mapping(...);       // 層映射
}
```

### 優化方案

為 QPSK/16QAM/64QAM 實現類似的融合函數:

```c
// 新增函數
void nr_modulate_layer_map_qpsk(const uint32_t *scrambled_data, ...);
void nr_modulate_layer_map_16qam(const uint32_t *scrambled_data, ...);
void nr_modulate_layer_map_64qam(const uint32_t *scrambled_data, ...);

// 使用 AVX2 gather 進行向量化查表
// QPSK: 2 bits/symbol, 16QAM: 4 bits/symbol, 64QAM: 6 bits/symbol
```

### 預期收益

- 消除 `mod_symbs` 中間緩衝區
- 減少記憶體頻寬
- 對於低 MCS 調度也能獲得優化

---

## 7. 記憶體存取優化

### 7.1 txdataF 預取

```c
// 在處理符號前預取下一個符號的 txdataF
for (int sym = 0; sym < nsymb; sym++) {
    // 預取下一個符號的數據
    if (sym + 1 < nsymb) {
        __builtin_prefetch(&txdataF[ant][sym_offset + symbol_sz], 1, 3);
    }
    // 處理當前符號
    process_symbol(sym);
}
```

### 7.2 對齊優化

確保所有緩衝區 64 字節對齊 (cache line):

```c
// 當前
c16_t tx_layers[rel15->nrOfLayers][layerSz2] __attribute__((aligned(64)));

// 確認所有臨時緩衝區都有對齊
c16_t mod_dmrs_precomputed[MAX_DMRS_SYMBOLS][dmrs_buf_size] __attribute__((aligned(64)));
```

---

## 8. PDCCH 生成優化

### 現狀分析

**位置**: `openair1/PHY/NR_TRANSPORT/nr_dci.c`

```c
// 對每個 DCI PDU 串行處理
for (int d = 0; d < pdcch_pdu_rel15->numDlDci; d++) {
    // DMRS 調製 (每符號)
    for (int symb = ...) {
        nr_modulation(gold, dmrs_length, ...);
    }
    // Polar 編碼
    polar_encoder_fast(...);
    // Scrambling + RE mapping
}
```

### 優化方案

1. **多 DCI 並行**: 如果有多個 DCI PDU，可以並行處理
2. **DMRS 預計算**: 與 PDSCH DMRS 類似，預計算 PDCCH DMRS

---

## 9. 優先級排序

根據預期收益和實現複雜度排序:

### 高優先級 (收益大，風險低)

| 優化 | 預期收益 | 複雜度 | 風險 |
|------|---------|--------|------|
| 相位旋轉並行化 | ~30-50 μs | 中 | 低 |
| Gold 序列預計算 | ~10-20 μs | 低 | 低 |
| 信號生成並行化 | ~100-150 μs | 高 | 中 |

### 中優先級 (收益中，需要驗證)

| 優化 | 預期收益 | 複雜度 | 風險 |
|------|---------|--------|------|
| QPSK/16QAM/64QAM 融合 | ~20-30 μs | 中 | 低 |
| 時序重排 | ~20-30 μs | 中 | 中 |

### 低優先級 (收益小或風險高)

| 優化 | 原因 |
|------|------|
| PDCCH 並行化 | 通常只有 1-2 個 DCI |
| 記憶體預取 | 編譯器可能已優化 |

---

## 實施建議

### 第一階段: 相位旋轉並行化

1. 在 `enkits_pool.c` 中添加相位旋轉任務函數
2. 修改 `phy_procedures_nr_gNB.c` 中的相位旋轉循環
3. 測量並驗證性能提升

### 第二階段: Gold 序列預計算

1. 創建 `slot_gold_cache_t` 結構
2. 在異步 memclear 期間預計算 Gold 序列
3. 修改各信號生成函數使用預計算的 Gold 序列

### 第三階段: 信號生成並行化

1. 創建統一的信號生成任務結構
2. 實現任務調度邏輯
3. 仔細測試確保 RE 不衝突

---

## 注意事項

1. **測試覆蓋**: 每個優化都需要完整的功能測試
2. **性能回歸**: 使用 `l1_slot_timing.csv` 監控性能
3. **代碼審查**: 並行化代碼容易引入競爭條件
4. **回滾計劃**: 保留舊代碼路徑，使用編譯選項切換
