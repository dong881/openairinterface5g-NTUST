# L1 Downlink Timing Accuracy Analysis

## 問題發現

檢查 `openair1/PHY/NR_TRANSPORT/nr_dlsch.c` 中的 precoding 和 RE mapping 計時實現，發現一個計時混雜問題。

## 原始問題

### Fast Path (2-layer PMI=0 direct mapping) - Lines 944-987

**原始代碼 (有問題):**
```c
if (timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
const size_t txdataF_offset_per_symbol = l_symbol * symbol_sz + txdataF_offset;

// Layer 0 → Antenna 0, Layer 1 → Antenna 1
for (int layer = 0; layer < 2; layer++) {
    layer_sz = do_onelayer(...);  // 真正的 RE mapping
}

// Antennas 2-3: Zero-fill
const int total_res = rel15->rbSize * NR_NB_SC_PER_RB;
for (int ant = 2; ant < frame_parms->nb_antennas_tx; ant++) {
    memset(&txdataF[ant][...], 0, ...);  // ⚠️ 這不是 RE mapping！
}

if (timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    total_mapping_ns += timespec_diff_ns_local(&t_start, &t_end);  // ❌ 包含了 memset 時間
}
```

**問題分析:**
- ❌ **RE mapping 時間混雜**: 包含了 `memset()` 清零 antenna 2-3 的時間
- ❌ **不準確的測量**: memset 操作不是 RE mapping 的一部分，是後處理清理
- ⚠️ **影響**: RE mapping 時間會虛高，尤其是在大 RB 數量時

## 修正後的代碼

### Fast Path - 正確分離 RE mapping 和 memset

**修正後 (正確):**
```c
if (timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
const size_t txdataF_offset_per_symbol = l_symbol * symbol_sz + txdataF_offset;

// Layer 0 → Antenna 0, Layer 1 → Antenna 1
for (int layer = 0; layer < 2; layer++) {
    layer_sz = do_onelayer(...);  // ✅ 純粹的 RE mapping
}

if (timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    total_mapping_ns += timespec_diff_ns_local(&t_start, &t_end);  // ✅ 只測量 RE mapping
}

// Antennas 2-3: Zero-fill (not part of RE mapping timing)
const int total_res = rel15->rbSize * NR_NB_SC_PER_RB;
for (int ant = 2; ant < frame_parms->nb_antennas_tx; ant++) {
    memset(&txdataF[ant][...], 0, ...);  // ✅ 不計入 RE mapping 時間
}
```

**改進:**
- ✅ **純粹的 RE mapping**: 只測量 `do_onelayer()` 循環
- ✅ **分離後處理**: memset 清零不計入 RE mapping 時間
- ✅ **準確測量**: 反映真實的 RE mapping 性能

## Standard Path 驗證 (無問題)

### Standard Path (non-PMI=0) - Lines 990-1033

**RE mapping timing (正確):**
```c
// Measure RE mapping time for this symbol
if (timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
for (int layer = 0; layer < rel15->nrOfLayers; layer++) {
    layer_sz = do_onelayer(...);  // ✅ 純粹的 RE mapping
}
if (timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    total_mapping_ns += timespec_diff_ns_local(&t_start, &t_end);  // ✅ 正確
}
```

**Precoding timing (正確):**
```c
// Measure precoding time for this symbol
if (timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
for (int ant = 0; ant < frame_parms->nb_antennas_tx; ant++) {
    do_txdataF(...);  // ✅ 純粹的 precoding
}
if (timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    total_precoding_ns += timespec_diff_ns_local(&t_start, &t_end);  // ✅ 正確
}
```

**結論:**
- ✅ Standard path 的計時實現完全正確
- ✅ RE mapping 和 precoding 完全分離
- ✅ 沒有混雜其他操作

## 完整的計時流程驗證

### 1. 變數初始化 (Line 876-877)
```c
long total_mapping_ns = 0;
long total_precoding_ns = 0;
```
✅ **正確**: 在 symbol 循環外初始化，累加所有 symbol 的時間

### 2. Symbol 循環 (Line 880)
```c
for (int l_symbol = rel15->StartSymbolIndex; l_symbol < rel15->StartSymbolIndex + rel15->NrOfSymbols; l_symbol++)
```
✅ **正確**: 每個 symbol 分別測量並累加

### 3. Fast Path vs Standard Path 分支
- **Fast Path**: 只測量 RE mapping，precoding = 0 (因為 direct mapping)
- **Standard Path**: 分別測量 RE mapping 和 precoding

✅ **正確**: 邏輯符合兩種路徑的不同處理方式

### 4. 最終累加 (Line 1041-1044)
```c
if (timing_enabled) {
    pdsch_detail.re_mapping_ns += total_mapping_ns;
    pdsch_detail.precoding_ns += total_precoding_ns;
}
```
✅ **正確**: 累加到全局統計

## 性能影響估算

### memset 操作的時間開銷

**條件:**
- 4 antennas (需要清零 antenna 2-3)
- 273 PRBs × 12 subcarriers = 3276 resource elements per symbol
- 每個 RE = 4 bytes (complex 16-bit)
- 總共: 3276 × 4 = 13KB per symbol per antenna

**memset 開銷估算:**
- 2 antennas × 13KB = 26KB per symbol
- Modern CPU memset: ~20 GB/s
- 估計時間: 26KB / 20GB/s ≈ **1.3 µs per symbol**

**對於 14 symbols:**
- 總 memset 時間: 14 × 1.3 µs ≈ **18 µs**

### 修正前後對比

**修正前 (Fast Path):**
- RE mapping 時間: 實際 RE mapping + 18µs memset = **虛高 18µs**
- 影響: 在總時間 ~250µs 中佔 7.2%

**修正後 (Fast Path):**
- RE mapping 時間: 只有實際 RE mapping = **準確**
- memset 時間: 不計入任何計時統計

## 其他計時點驗證

### Encoding (Line 1113-1134)
```c
if (timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_enc_start);
if (nr_dlsch_encoding(...) == -1) return;
if (timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_enc_end);
    pdsch_detail.encoding_ns += timespec_diff_ns_local(&t_enc_start, &t_enc_end);
}
```
✅ **正確**: 純粹測量 LDPC encoding

### Scrambling (Line 780-798)
```c
if (timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
nr_pdsch_codeword_scrambling(...);
if (timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    pdsch_detail.scrambling_ns += timespec_diff_ns_local(&t_start, &t_end);
}
```
✅ **正確**: 純粹測量 scrambling

### Modulation (Line 802-807)
```c
if (timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
nr_modulation(...);
if (timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    pdsch_detail.modulation_ns += timespec_diff_ns_local(&t_start, &t_end);
}
```
✅ **正確**: 純粹測量 modulation

### Layer Mapping (Line 843-851)
```c
if (timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_start);
nr_layer_mapping(...);
if (timing_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    pdsch_detail.layer_mapping_ns += timespec_diff_ns_local(&t_start, &t_end);
}
```
✅ **正確**: 純粹測量 layer mapping

## 總結

### 修正內容
- **檔案**: `openair1/PHY/NR_TRANSPORT/nr_dlsch.c`
- **位置**: Lines 944-988 (Fast Path)
- **修正**: 將 `clock_gettime(&t_end)` 移到 memset 之前
- **影響**: RE mapping 時間更準確，減少約 18µs 的虛高

### 驗證結果
- ✅ **Fast Path RE mapping**: 現在正確，不包含 memset
- ✅ **Fast Path precoding**: 正確為 0 (direct mapping 不需要 precoding)
- ✅ **Standard Path RE mapping**: 一直都正確
- ✅ **Standard Path precoding**: 一直都正確
- ✅ **所有其他計時點**: 全部正確，沒有混雜

### 計時準確性
所有計時點現在都正確反映實際處理時間，沒有混雜其他操作：
1. **Encoding**: 純 LDPC + rate matching
2. **Scrambling**: 純 gold sequence scrambling
3. **Modulation**: 純 QAM mapping
4. **Layer Mapping**: 純 codeword to layer distribution
5. **RE Mapping**: 純 layer to RE distribution (不含 memset)
6. **Precoding**: 純 precoding matrix multiplication (Standard Path only)

### 建議
- ✅ 修正已完成並編譯成功
- ✅ 可以信任 CSV 輸出的計時數據進行性能分析
- ⚠️ 如需分析 memset overhead，可單獨添加計時點
