# log_mmap_entry Log 檔案使用手冊

## 1. 輸出細節

### 1(a) 最後輸出資料夾
- `logs/`
- 這個資料夾在 softmodem 執行時建立於執行目錄下
- 不會建立更深的子資料夾，所有 log 檔案都直接放於 `logs/`

### 1(b) 檔案名稱
- 格式：`logs/<log_name_without_.bin>.<split_index>.bin`（當 `log_name` 自帶 `.bin` 副檔名時）
- `split_index` 以三位數格式填補：`000`, `001`, `002`
- 範例：`logs/pnf_timing_window-us.000.bin`、`logs/vnf_harq_rtt-us.000.bin`

### 1(c) 產生模式
- `PNF` mode 會產生：
  - `pnf_timing_window-us.bin`
- `VNF` mode 會產生：
  - `vnf_harq_rtt-us.bin`
  - `rlc_am_sn_in_tx_window-count.bin`
  - `rlc_am_arq_retx-count.bin`
  - `rlc_am_arq_rtt-us.bin`
  - `vnf_dl_harq_available-count.bin`
  - `vnf_dl_cqi-idx.bin`
  - `vnf_dl_mcs-idx.bin`
  - `vnf_dl_harq_round-count.bin`
  - `vnf_dl_harq_k1-count.bin`
  - `vnf_advance_time-us.bin`
  - `vnf_pnf_latency-us.bin`
- `MONOLITHIC` mode 會同時初始化上述 PNF 與 VNF log，並可擴充 monolithic-only log。

> 這裡的 `VNF mode` 即程式中對應的 VNF 執行模式。

## 2. 內容與數值拆分細節

### 2(a) 二進位記錄格式
- 每筆記錄固定佔用 8 bytes
- 檔案內是連續寫入的 signed 64-bit integer
- 無 header、無分隔符
- 若寫入大小超過當前 split，會自動 rollover 到下一個 `.###`

### 2(b) 兩種資料型態

#### 2(b).1. Packed log（含 SFN/Slot）
- 適用 log：
  - `pnf_timing_window-us.bin`
  - `vnf_advance_time-us.bin`
- 這兩個 log 的一筆記錄包含：
  1. `SFN`
  2. `Slot`
  3. `payload`
- 目前程式中這兩個 packed log 是由 `pack_sfn_slot_value()` 生成，接著用 `log_mmap_entry()` 直接寫入 8 bytes binary。

#### 2(b).2. Raw log（單一值）
- 適用 log：
  - `vnf_timing_pending_us-us.bin`
  - `vnf_harq_rtt-us.bin`
  - `vnf_dl_harq_available-count.bin`
  - `vnf_dl_cqi-idx.bin`
  - `vnf_dl_mcs-idx.bin`
  - `vnf_dl_harq_round-count.bin`
  - `vnf_dl_harq_k1-count.bin`
  - `rlc_am_sn_in_tx_window-count.bin`
  - `rlc_am_arq_retx-count.bin`
  - `rlc_am_arq_rtt-us.bin`
  - `vnf_pnf_latency-us.bin`
- 這些 log 只儲存一個 signed 64-bit 量測值

### 2(c) Packed log 的欄位分配
- 64-bit layout：
  - `bits 63..48`：SFN（16 bits allocated）
  - `bits 47..32`：Slot（16 bits allocated）
  - `bits 31..0`：payload（32 bits signed）

### 2(c).1 Python 讀取提醒
- `pnf_timing_window-us.bin`、`vnf_advance_time-us.bin` 為 packed log；其 raw record 應先讀成 signed 64-bit 再拆欄位。
- `vnf_timing_pending_us-us.bin`、`vnf_harq_rtt-us.bin`、`vnf_dl_harq_available-count.bin`、`vnf_dl_cqi-idx.bin`、`vnf_dl_mcs-idx.bin`、`vnf_dl_harq_round-count.bin`、`vnf_dl_harq_k1-count.bin`、`rlc_am_sn_in_tx_window-count.bin`、`rlc_am_arq_retx-count.bin`、`rlc_am_arq_rtt-us.bin`、`vnf_pnf_latency-us.bin` 等皆為 raw log，直接讀出 signed 64-bit integer 即可。
- Python 讀取範例：

```python
import struct
import ctypes

with open("logs/pnf_timing_window-us.bin.000", "rb") as f:
    while chunk := f.read(8):
        raw_value, = struct.unpack("<q", chunk)
        sfn = (raw_value >> 48) & 0xFFFF
        slot = (raw_value >> 32) & 0xFFFF
        payload = ctypes.c_int32(raw_value & 0xFFFFFFFF).value
        print(sfn, slot, payload)
```

- 注意：payload 是 signed 32-bit，必須做符號延伸還原。不要只用 `raw_value & 0xFFFFFFFF` 當成最終結果。

### 2(d) 實際需要的位寬
- SFN 範圍：`0..1023` → 10 bits
- Slot 範圍：`0..(10 << mu) - 1`
  - mu 最大通常是 5，最大 slot = 319 → 9 bits
- 真正必要位寬：`10 + 9 = 19 bits`
- 現行實作配置：`16 + 16 = 32 bits`
- 因此現有設計預留了 `13 bits` 額外空間

### 2(e) 十進制存儲比較
- 如果改成 ASCII text，記錄例如 `1023 319 -50000\n` 會需要約 16~20 bytes
- 這比目前固定 8 bytes 二進位格式更浪費空間
- 如果改成十進制位置打包，仍需約 17~19 digits，等價於 57~63 bits
- 結論：
  - 目前 64-bit binary 範例最省空間
  - 十進制編碼不會使空間更省
  - 要擴充 payload bit，應重構欄位配置，而非改成十進制

### 2(f) 可擴充的 payload bit
- 當前 64-bit 分配：`16 + 16 + 32 = 64 bits`
- 若將 SFN/Slot 壓縮到實際位寬：
  - SFN 用 10 bits
  - Slot 用 9 bits
- 剩餘可給 payload：`64 - 19 = 45 bits`
- 意味著：payload 從 32-bit 可以擴到 45-bit
- 這表示 Signed payload 的範圍可從 `±2.1e9` 擴到 `±8.8e13`

## 3. 各 log 的數值意義與範圍

| log 名稱 | 代表意義 | 估計範圍 | 單位 | 解析方式 |
|---|---|---|---|---|
| `pnf_timing_window-us.bin` | PNF 時序窗口檢查 margin | 典型 `-50k..+400k` | μs | packed log |
| `vnf_advance_time-us.bin` | VNF slot send advance time | 典型 `0..20k` | μs | packed log |
| `vnf_timing_pending_us-us.bin` | VNF pending timing debt | 典型 `-20k..20k` | μs | raw log |
| `vnf_harq_rtt-us.bin` | HARQ round-trip delay | 典型 `0..20k` | μs | raw log |
| `vnf_dl_harq_available-count.bin` | Available DL HARQ process count at scheduling, including `0` when no DL HARQ process is free | 典型 `0..N` | count | raw log |
| `vnf_dl_cqi-idx.bin` | reported wideband CQI index | 典型 `0..31` | CQI index | raw log |
| `vnf_dl_mcs-idx.bin` | selected MCS index for DL scheduling | 典型 `0..28` | MCS index | raw log |
| `vnf_dl_harq_round-count.bin` | DL HARQ transmission count until success; `5` indicates DTX or final HARQ failure | 典型 `1..5` | count | raw log |
| `vnf_dl_harq_k1-count.bin` | DL HARQ timing indicator (k1) for PUCCH ACK/NACK scheduling | 典型 `-N..+N` | count | raw log |
| `vnf_pnf_latency-us.bin` | VNF‑PNF latency placeholder | 目前已初始化但未在現有 code 中寫入值 | raw log |
| `rlc_am_sn_in_tx_window-count.bin` | RLC AM control PDU ACK_SN window validity indicator | `0` or `1` | count | raw log |
| `rlc_am_arq_retx-count.bin` | RLC AM retransmission count when SDU completes | 典型 `0..N` | count | raw log |
| `rlc_am_arq_rtt-us.bin` | RLC AM ARQ RTT from first transmission to ACK/clear | 典型 `0..20k` | μs | raw log |

> Note: `vnf_dl_harq_available-count.bin` 是 DL HARQ 可用性分布的主要 log，當沒有空閒 DL HARQ process 時會寫入 `0`。

> 以上範圍為程式邏輯推估的典型值；特殊狀況下仍可能超出。

## 4. 解析條件

### 4(a) 是否拆成 SFN/Slot
- 若 `log_name` 為：
  - `pnf_timing_window-us.bin`
  - `vnf_advance_time-us.bin`
  → 必須拆成 SFN/Slot/payload
- 否則 → 當作單一 signed 64-bit 量測值

### 4(b) Packed log 拆分公式
- `raw_value` = signed 64-bit
- `sfn = (raw_value >> 48) & 0xFFFF`
- `slot = (raw_value >> 32) & 0xFFFF`
- `payload = (int32_t)(raw_value & 0xFFFFFFFF)`
- 注意：payload 是 signed 32-bit，必須做符號延伸還原

### 4(c) 建議輸出欄位
- `log_name`
- `split_file`
- `entry_index`
- `raw_value`
- `is_packed`
- `sfn`
- `slot`
- `signed_payload`
- `payload_us`

## 5. 使用方式

1. 掃描 `logs/<name>.*`
2. 將所有 split 檔案讀為 signed 64-bit values
3. 對 packed log 進行 SFN/Slot 解碼
4. 對 raw log 直接做 microsecond 統計
5. 如果需要時間軸排序，優先用 `sfn`/`slot`

## 6. 結論
- 這組 log 使用固定 8 bytes binary，是最省空間的方案
- 純十進制存儲無法比 binary 更省
- 若要讓 `Value` 佔更多 bits，應先調整 SFN/Slot 的位寬分配
- 現行 SFN/Slot 可壓縮到 19 bits，payload 最多可擴到 45 bits
