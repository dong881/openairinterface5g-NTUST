# PNF 壓力回應模式檢測 (PNF Pressure Response Detection)

## 功能說明

新增功能用於偵測和可視化一個重要的控制器行為模式：

**VNF 平穩 → PNF 下降 → VNF 上升**

此模式驗證了壓力-債務模型（pressure-debt model）的正確性：
- 當 PNF 報告 timing margin 下降（壓力信號）時
- VNF 控制器立即做出響應，增加 s_ahead（VNF ahead time）

## 參數定義

在 `plot_advance.py` 中定義的相關參數：

```python
# VNF 平穩性判斷
VNF_FLAT_THRESHOLD = 50  # 變化率門檻 (μs/sample)，低於此值視為平穩

# PNF 下降偵測
PNF_DROP_THRESHOLD = -100  # 下降幅度門檻 (μs)，負於此值視為下降

# VNF 上升響應
RISE_START_THRESHOLD = 50  # 開始上升的差分門檻 (μs)

# 時間窗口
PNF_FLAT_WINDOW = 10  # 確認 VNF 平穩的連續點數
PNF_RESPONSE_DELAY = 5  # PNF 下降與 VNF 上升之間允許的最大延遲
```

## 使用方法

### 1. 自動檢測（默認）

運行批處理時自動執行檢測：

```bash
python3 plot_advance.py --input-dir /path/to/data --output-dir /path/to/output
```

輸出文件：
```
output_dir/
├── 300/
│   └── vnf_pnf/
│       └── vnf_pnf_pressure_response_pattern@80pts.png
├── 500/
│   └── vnf_pnf/
│       └── vnf_pnf_pressure_response_pattern@80pts.png
└── ...
```

### 2. 調整檢測參數

編輯 `plot_advance.py` 中的參數來微調檢測靈敏度：

**更敏感的檢測（找到更多模式）：**
```python
VNF_FLAT_THRESHOLD = 100  # 增加允許的平穩變化量
PNF_DROP_THRESHOLD = -50   # 降低下降幅度要求
PNF_RESPONSE_DELAY = 10    # 增加允許的延遲窗口
```

**更嚴格的檢測（找到高質量模式）：**
```python
VNF_FLAT_THRESHOLD = 20   # 降低允許的平穩變化量
PNF_DROP_THRESHOLD = -200  # 提高下降幅度要求
PNF_RESPONSE_DELAY = 3     # 減少允許的延遲窗口
```

## 圖表解讀

生成的 `vnf_pnf_pressure_response_pattern@80pts.png` 圖表包含：

- **藍色曲線**：VNF ahead time（VNF 的前導時間）
- **紫色方塊**：PNF timing margin（Δt_arrive，PNF 的時間裕度）
- **綠色背景區域**：VNF 平穩區間
- **紅色虛線**：PNF 下降點
- **橙色背景區域**：VNF 上升響應區間

典型的壓力回應模式特徵：
1. 綠色區域內 VNF（藍線）保持相對平穩
2. 紅色虛線處 PNF（紫色方塊）出現顯著下降
3. 橙色區域內 VNF 迅速上升，對應額外的 s_ahead

## 控制器驗證

此模式檢測驗證了 `p7_run_ewma_lab_control()` 中的以下邏輯：

### 壓力累積（Pressure Accumulation）
```c
// 行 455-480：壓力源
pressure_debt_us = tail_risk_us + failure_debt_us + queue_debt_us;
```

### 增量 UP 邏輯（Incremental UP）
```c
// 行 513-524：基於壓力的 s_ahead 增量調整
if (pressure_debt_us > 0) {
    extra_slots = ceil(pressure_debt_us / slot_duration_us);
    target_s_ahead = s_ahead_env + extra_slots;
}
```

### 安全的 DOWN 邏輯（Safe DOWN）
```c
// 行 548-564：只在安全時才減少 s_ahead
if (post_down_tail_risk_us <= 0) {
    target_s_ahead = s_ahead_env - 1;
}
```

## 檢測算法原理

```python
detect_pnf_pressure_response_intervals(vnf_data, aligned_pnf):
    for each candidate position:
        # 階段 1：檢查 VNF 平穩
        if all(abs(diff) <= VNF_FLAT_THRESHOLD for diff in window):
            # 階段 2：尋找 PNF 下降
            if pnf_change <= PNF_DROP_THRESHOLD:
                # 階段 3：尋找 VNF 上升響應
                if rise_detected:
                    yield (flat_start, pnf_drop_idx, rise_start, rise_end)
```

## 典型檢測結果

基於 100,000 點測試數據：

| 負載 | 檢測到模式數 | 第一個模式 |
|------|------------|----------|
| 300μs | 多個 | (51990, 52155, ...) |
| 500μs | 5+ | (275, 289, 291, 292) |
| 800μs | 多個 | (1184, ...) |
| 900μs | 多個 | (290, ...) |

> 注：檢測到的模式數量取決於數據特性和參數設置

## 性能指標

- 檢測時間：~0.1秒 (100,000點數據)
- 繪圖生成時間：~0.5秒
- 總批處理時間：~10秒 (4個 pair，並行 4個 worker)

## 故障排除

### 未檢測到任何模式

**可能原因：**
1. PNF 數據中沒有顯著的下降事件
2. VNF 控制策略不同導致沒有相應的上升
3. 參數設置過於嚴格

**解決方案：**
```python
# 放寬檢測條件
VNF_FLAT_THRESHOLD = 150
PNF_DROP_THRESHOLD = -50
PNF_RESPONSE_DELAY = 15
```

### 檢測到過多模式（偽陽性）

**可能原因：**
1. 檢測條件過於寬鬆
2. 數據中有大量小的波動被錯認為模式

**解決方案：**
```python
# 嚴格化檢測條件
VNF_FLAT_THRESHOLD = 20
PNF_DROP_THRESHOLD = -200
PNF_RESPONSE_DELAY = 2
```

## 未來改進

- [ ] 為每個 pair 繪製多個檢測到的模式（不只是最佳的一個）
- [ ] 添加模式質量評分（基於上升幅度、延遲時間等）
- [ ] 實現時間戳統計，分析 PNF 下降與 VNF 上升的延遲特性
- [ ] 交互式參數調整工具

## 參考文獻

相關代碼位置：
- 檢測函數：[plot_advance.py](plot_advance.py#L750-L830)
- 繪圖函數：[plot_advance.py](plot_advance.py#L1410-L1500)
- 集成點：[plot_advance.py](plot_advance.py#L1625-L1650)

控制器實現：
- 壓力計算：[vnf_p7.c](nfapi/open-nFAPI/vnf/src/vnf_p7.c#L410-L500)
- UP/DOWN 邏輯：[vnf_p7.c](nfapi/open-nFAPI/vnf/src/vnf_p7.c#L513-L564)
