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
  - `vnf_total_dl_harq_available-count.bin`
  - `vnf_dl_cqi-idx.bin`
  - `vnf_dl_mcs-idx.bin`
  - `vnf_dl_harq_round-count.bin`
  - `vnf_dl_harq_k1-count.bin`
  - `vnf_advance_time-us.bin`
  - `vnf_pnf_latency-us.bin`
- `MONOLITHIC` mode 會同時初始化上述 PNF 與 VNF log，並可擴充 monolithic-only log。

> 這裡的 `VNF mode` 即程式中對應的 VNF 執行模式。

## 1(d) 動態開關說明
- `nr-softmodem` 中的 `log_mmap_entry` 紀錄預設為關閉。
- 執行中可用 `SIGUSR1` 來切換開/關，無需重新啟動程式。
- 每次傳送 `SIGUSR1` 都會反轉當前狀態：
  - 關閉 -> 開啟
  - 開啟 -> 關閉
- 如果尚未開啟過，第一次開啟時會初始化所有應用於目前執行模式的 mmap loggers。
- 關閉後，`log_mmap_entry()` 會直接返回，不會產生新的 log 檔案或寫入資料。

### 1(d).1. 命令範例
- 查 PID：`ps aux | grep nr-softmodem`
- 開啟／關閉 log：
  ```bash
  kill -SIGUSR1 <PID>
  ```
- 如果想確認目前狀態，可留意程式輸出，它會印出：
  - `[LOG] mmap logging enabled`
  - `[LOG] mmap logging disabled`

### 1(d).2. 行為說明
- `SIGUSR1` 只是切換開關，不會改變 log 檔名或資料格式。
- 該機制支援所有現有 `log_mmap_entry()` 呼叫，無須額外修改來源程式碼。
- 若程式在關閉狀態時收到訊號，會在下次收到 `SIGUSR1` 時啟動 logger。

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
  - `vnf_total_dl_harq_available-count.bin`
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

## 5(a) 在 `screen` 中執行 gNB VNF / PNF

### 開啟新的 `screen` 視窗
建議為 VNF 和 PNF 各創建一個獨立 `screen` session，方便管理與日後切換：

```bash
screen -S oai_vnf
# 在這個視窗中啟動 VNF
./build/oai/install/bin/nr-softmodem --nfapi VNF ...
```

```bash
screen -S oai_pnf
# 在這個視窗中啟動 PNF
./build/oai/install/bin/nr-softmodem --nfapi PNF ...
```

### 從另一個終端或 screen 視窗切換回來
```bash
screen -ls
screen -r oai_vnf
screen -r oai_pnf
```

### 從另一個終端發送 `SIGUSR1`
1. 先取得正在執行中 VNF / PNF 的 PID：
   ```bash
   ps aux | grep nr-softmodem
   ```
2. 傳送訊號切換 log：
   ```bash
   kill -SIGUSR1 <PID>
   ```
3. 程式會在 `screen` 視窗中印出：
   - `[LOG] mmap logging enabled`
   - `[LOG] mmap logging disabled`

### `screen` 常用操作
- 解除連線（detach）：`Ctrl-a d`
- 重新連線：`screen -r <session_name>`
- 列出所有 session：`screen -ls`

### 注意事項
- VNF / PNF 兩個執行個體各有自己的 `screen` session，彼此獨立，互不影響。
- `SIGUSR1` 只切換當前 process 的 mmap logging，不會影響另一個 process。
- 若要同時開啟或關閉兩個 session 的 log，必須對各自 PID 分別發送 `SIGUSR1`。

## 6. 結論
- 這組 log 使用固定 8 bytes binary，是最省空間的方案
- 純十進制存儲無法比 binary 更省
- 若要讓 `Value` 佔更多 bits，應先調整 SFN/Slot 的位寬分配
- 現行 SFN/Slot 可壓縮到 19 bits，payload 最多可擴到 45 bits
