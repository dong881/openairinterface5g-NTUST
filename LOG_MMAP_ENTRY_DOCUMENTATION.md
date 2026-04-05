# log_mmap_entry Log 檔案使用手冊

## 1. 輸出細節

### 1(a) 最後輸出資料夾
- `logs/`
- 這個資料夾位於執行 softmodem 時的當前工作目錄
- 並不會建立更深的子資料夾，所有 log 檔案都直接放在 `logs/` 下

### 1(b) 檔案名稱
- `logs/<log_name>.<split_index>`
- `split_index` 以三位數格式填補，例如 `000`, `001`, `002`
- 例如：`logs/pnf_timing_window.000`, `logs/vnf_harq_rtt.000`

### 1(c) 產生模式
- `PNF` mode 產生：`pnf_timing_window`
- `VNF` mode 產生：
  - `vnf_harq_buffer`
  - `vnf_harq_rtt`
  - `vnf_rlc_runtime`
  - `vnf_rlc_hol_delay`
  - `vnf_rlc_avg_to_tx`
  - `vnf_advance_time`
  - `vnf_pnf_latency`
- `MONOLITHIC` mode 同時可能產生上述 PNF 與 VNF 的 log

> 本手冊中之「VNF mode」可對應到實際程式中的 VNF mode。

## 2. Log 檔案內容說明

### 2(a) 基本檔案格式
- 每個 log split 檔案都是二進位檔
- 檔案內容由一連串 `8 bytes` 的 signed 64-bit 值組成
- 無額外檔頭、無分隔符，純粹連續寫入
- 若檔案超過大小限制，會自動分割到下一個 `.001`、`.002`

### 2(b) 各 log 檔案簡述

| log 名稱 | 模式 | 內容重點 |
|---|---|---|
| `pnf_timing_window` | PNF | 儲存 PNF 時序窗口檢查結果，且該筆值包含 SFN/Slot 與對應 payload |
| `vnf_harq_buffer` | VNF | 儲存 VNF HARQ buffer 相關延遲量測值 |
| `vnf_harq_rtt` | VNF | 儲存 VNF HARQ round-trip delay 量測值 |
| `vnf_rlc_runtime` | VNF | 儲存 VNF RLC 執行時間量測值 |
| `vnf_rlc_hol_delay` | VNF | 儲存 VNF RLC head-of-line delay 量測值 |
| `vnf_rlc_avg_to_tx` | VNF | 儲存 VNF RLC 平均送出準備時間量測值 |
| `vnf_advance_time` | VNF | 儲存 VNF slot 送出時的 advance time 量測值，包含 SFN/Slot 資訊 |
| `vnf_pnf_latency` | VNF | 儲存 VNF 與 PNF 之間 latency 量測值，包含 SFN/Slot 資訊 |

### 2(c) 何時使用哪個 log
- 若要分析 PNF 端時序窗口，可檢查 `pnf_timing_window`
- 若要分析 VNF 端的延遲或緩衝行為，可檢查 `vnf_*` 系列
- 若執行 monolithic 型態，可同時保留 PNF 與 VNF 資料

## 3. 解析重點與注意事項

### 3(a) 何時拆解成 SFN/Slot
- 只有下列 log 需要進一步拆解成 SFN / Slot 與 payload：
  - `pnf_timing_window`
  - `vnf_advance_time`
  - `vnf_pnf_latency`
- 其餘 log 皆可直接視為單一的 signed 64-bit 量測值

### 3(b) 讀檔時的基本判斷邏輯
- 開啟 `logs/<name>.<idx>`，以 `'<{count}q'` 解析成 signed 64-bit 值
- 若 log 屬於上述三個編碼型 log，再進行 SFN/Slot 拆解
- 其餘 log 直接視為單一量測值，不拆解 SFN/Slot

### 3(c) 檔案位置與讀取方式
- 讀取時需要檢查所有 `logs/<name>.*` 檔案