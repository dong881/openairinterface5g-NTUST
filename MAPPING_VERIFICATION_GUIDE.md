# Mapping 验证快速指南

## 核心原则

**正确的 Mapping = VNF > Δt_arrive（在 Y 轴上）**

- VNF ahead time（蓝色线）应该永远高于 Δt_arrive（紫色方块）
- 特别是在 VNF 不变的平稳区间，这个关系应该始终成立

## 如何判断 Mapping 是否正确

### ✅ 正确的 Mapping 特征

```
VNF (blue line) is consistently ABOVE Δt_arrive (purple squares)
在 VNF 平稳的区域，Δt_arrive 值明显低于 VNF 值
Quality score >= 90%：mapping 很好，可以放心分析
```

**示例：** 800 数据对（99.4% 质量）
- VNF 平稳在 0.5-1.0ms
- Δt_arrive 保持在 1-2ms  
- 清晰的上下关系

### ⚠️ 边界异常情况

```
Quality score 90-50%：大多数样本满足 VNF > Δt_arrive
但有少数边界点出现 PNF >= VNF（约 1-10%）
这是可以接受的，是边界情况导致的异常
可以放心进行分析，只需注意这些异常区间
```

**示例：** 500 数据对（93.8% 质量）
- 有 3080/50000 个样本违规（6.2%）
- 大多数在 VNF 平稳区间仍然满足 VNF > Δt_arrive
- 少数边界点在 VNF 变化时出现 Δt_arrive >= VNF

### ✗ 错误的 Mapping 特征

```
Quality score < 50%：大部分样本违反 VNF > Δt_arrive
Δt_arrive 经常高于 VNF，即使在 VNF 平稳区间
这表示 mapping 到了错误的 (SFN, slot) 组合
结果完全无法用于分析，必须修复 mapping 逻辑！
```

## 快速验证步骤

1. **生成压力响应图表**
   ```bash
   python3 plot_advance.py --input-dir /path/to/data --output-dir /path/to/output
   ```

2. **观察 vnf_pnf_pressure_response_pattern@80pts.png**
   - 找 VNF 平稳的区间
   - 检查这些区间中 Δt_arrive 是否低于 VNF

3. **查看控制台输出的验证报告**
   ```
   === PNF-VNF Mapping 验证报告 ===
   Mapping 质量得分: XX.X%
   建议: ✓/⚠️/✗
   ```

4. **根据建议判断**
   - ✓ (>90%): Good to go
   - ⚠️ (50-90%): Acceptable with caveats  
   - ✗ (<50%): Must fix mapping logic

## 数据示例分析

### 300 组 (98.2% 质量)
```
VNF 值: ~500-1000 μs
Δt_arrive: ~900-1200 μs (在 VNF 平稳区低于 VNF)
违规样本: 903/50000 (1.8%) - 可忽略的边界异常
结论: ✓ Mapping 正确
```

### 500 组 (93.8% 质量)  
```
VNF 值: ~1000 μs (平稳区)
Δt_arrive: ~1000-1800 μs (在 VNF 平稳区多数< VNF)
违规样本: 3080/50000 (6.2%) - 可接受
结论: ⚠️ Mapping 质量一般但可用，有边界异常
```

### 800 组 (98.6% 质量)
```
VNF 值: ~500 μs
Δt_arrive: ~1000-1500 μs (始终> VNF，但这是常态)
违规样本: 682/50000 (1.4%) - 可忽略的异常
结论: ✓ Mapping 正确
```

### 900 组 (99.4% 质量)
```
VNF 值: ~1000 μs
Δt_arrive: ~1000-1800 μs
违规样本: 292/50000 (0.6%) - 极少异常
结论: ✓ Mapping 完美
```

## 关键要点

1. **VNF > Δt_arrive 的关系最重要**
   - 这是判断 mapping 是否到了正确 frame/slot 的基础

2. **VNF 平稳区间是最好的验证区域**
   - 当 VNF 不变时，Δt_arrive 应该稳定且低于 VNF
   - 如果在这个区间违反关系，说明 mapping 有问题

3. **少数边界异常是正常的**
   - 即使质量 90%+，也可能有 1-5% 的边界违规
   - 这些通常发生在 VNF 变化的边界处
   - 只要大多数样本满足关系就认为 mapping 正确

4. **Quality score 解读**
   - **>95%**: Perfect
   - **90-95%**: Very good
   - **85-90%**: Good (可用)
   - **50-85%**: Acceptable but with caveats
   - **<50%**: Mapping error (不可用)

## 故障排除

### 问题：Quality score < 50%

**可能原因：**
- Mapping 使用了错误的 (SFN, slot) 组合
- VNF 和 PNF 来自不同的时间基准
- 两个文件不是同一段 run

**解决方案：**
- 检查 `align_pnf_to_vnf_sequence()` 中的 mapping 逻辑
- 验证 SFN/slot 是否正确编码
- 确认数据文件是从同一段 log 提取

### 问题：Quality score 50-90% 但有大量违规

**可能原因：**
- Mapping 在某些时间段是对的，某些时间段是错的
- 可能存在 SFN wrap-around 或 slot offset 问题

**解决方案：**
- 检查违规点集中在哪些区域
- 分段分析不同时间区间的 mapping 质量
- 查看是否存在周期性的 mapping 错误

## 参考

- 检测函数: `detect_pnf_pressure_response_intervals()`
- 验证函数: `verify_pnf_vnf_mapping_quality()`
- 绘图函数: `plot_pnf_pressure_response_pattern()`

所有函数位于 `plot_advance.py`
