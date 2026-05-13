#!/usr/bin/env python3
import struct
import ctypes
import argparse
import os
from collections import defaultdict, Counter

import matplotlib.pyplot as plt


# ============================================================
# 論文圖表全局設定
# ============================================================
plt.rcParams['font.family'] = 'serif'
plt.rcParams['font.size'] = 14
plt.rcParams['axes.labelsize'] = 16
plt.rcParams['xtick.labelsize'] = 14
plt.rcParams['ytick.labelsize'] = 14
plt.rcParams['legend.fontsize'] = 12

SQUARE_FIGSIZE = (4.8, 4.8)
VNF_MARKER = 'o'
PNF_MARKER = 's'
MARKER_SIZE = 3
DEADLINE_COLOR = '#e74c3c'


# ============================================================
# 讀取 binary log
# ============================================================
def read_packed_log(filepath, max_points=100000):
    """
    讀取二進位 log。

    原始 64-bit layout:
      bits 63~48: sfn
      bits 47~32: slot
      bits 31~0 : signed payload_us

    payload 轉成 ms。
    """

    data = []

    if not os.path.exists(filepath):
        print(f"[ERROR] 找不到檔案: {filepath}")
        return data

    with open(filepath, "rb") as f:
        idx = 0

        while len(data) < max_points:
            chunk = f.read(8)

            if len(chunk) < 8:
                break

            raw_value, = struct.unpack("<q", chunk)

            sfn = (raw_value >> 48) & 0xFFFF
            slot = (raw_value >> 32) & 0xFFFF
            payload_us = ctypes.c_int32(raw_value & 0xFFFFFFFF).value

            data.append({
                "idx": idx,
                "sfn": sfn,
                "slot": slot,
                "payload": payload_us / 1000.0,
                "payload_us": payload_us,
            })

            idx += 1

    print(f"[INFO] 已讀取 {filepath}: {len(data)} points")
    return data


# ============================================================
# 重建 absolute slot
# ============================================================
def reconstruct_absolute_slot(data, slots_per_frame=20, sfn_mod=1024, name="DATA"):
    """
    根據 log 順序重建 monotonic absolute slot。

    因為 raw sfn 只有 0~1023，所以不能直接當 full SFN。
    這裡使用:
        modulo_slot = sfn * slots_per_frame + slot

    當 modulo_slot 從高值跳回低值時，判定發生 SFN wrap。
    absolute_slot = wrap_count * cycle_slots + modulo_slot

    對 VNF:
      正常應該幾乎每筆 absolute_slot 遞增 1。

    對 PNF:
      因為 low offered load 不一定每 slot 有排程，
      所以 absolute_slot 可以跳躍，也可以同一 slot 有多筆。
    """

    if not data:
        return []

    cycle_slots = sfn_mod * slots_per_frame

    extended = []
    wrap_count = 0
    prev_modulo_slot = None

    # threshold 用半個 cycle，避免正常 missing slots 被誤判成 wrap
    wrap_threshold = cycle_slots // 2

    wrap_events = 0
    backward_small_jumps = 0

    for d in data:
        modulo_slot = d["sfn"] * slots_per_frame + d["slot"]

        if prev_modulo_slot is not None:
            diff = modulo_slot - prev_modulo_slot

            # 例如 1023:19 -> 0:0
            # diff 會是非常大的負值
            if diff < -wrap_threshold:
                wrap_count += 1
                wrap_events += 1

            # 小幅 backward 通常代表 raw log 非單調或同時間多來源亂序
            elif diff < 0:
                backward_small_jumps += 1

        abs_slot = wrap_count * cycle_slots + modulo_slot

        new_d = d.copy()
        new_d["modulo_slot"] = modulo_slot
        new_d["wrap_count"] = wrap_count
        new_d["abs_slot_raw"] = abs_slot
        new_d["abs_slot"] = abs_slot

        extended.append(new_d)
        prev_modulo_slot = modulo_slot

    print(f"\n[ABS SLOT RECONSTRUCT] {name}")
    print(f"  slots_per_frame       = {slots_per_frame}")
    print(f"  cycle_slots           = {cycle_slots}")
    print(f"  wrap events           = {wrap_events}")
    print(f"  small backward jumps  = {backward_small_jumps}")
    print(f"  abs_slot min/max      = {extended[0]['abs_slot']} / {extended[-1]['abs_slot']}")

    if backward_small_jumps > 0:
        print(f"  [WARNING] {name} 有小幅 backward jumps，代表 log 順序可能不是完全單調。")

    return extended


# ============================================================
# 自動估計 PNF 對 VNF 的 cycle offset
# ============================================================
def estimate_pnf_cycle_offset(vnf_data, pnf_data, slots_per_frame=20, sfn_mod=1024, search_cycles=8):
    """
    PNF 和 VNF 幾乎同時啟動，但 raw SFN 只有 0~1023。
    因此兩邊重建出來的 abs_slot 可能差一個或多個 1024-cycle。

    這裡只搜尋 k * cycle_slots 的 offset，
    選擇讓 PNF 起始 slot 最接近 VNF 起始 slot的 k。

    因為 VNF 每 slot 都有，所以只需要讓 PNF timeline 落在 VNF timeline 附近。
    """

    if not vnf_data or not pnf_data:
        return 0

    cycle_slots = sfn_mod * slots_per_frame

    vnf_first = vnf_data[0]["abs_slot_raw"]
    pnf_first = pnf_data[0]["abs_slot_raw"]

    best_k = 0
    best_score = None

    for k in range(-search_cycles, search_cycles + 1):
        shifted_pnf_first = pnf_first + k * cycle_slots
        score = abs(shifted_pnf_first - vnf_first)

        if best_score is None or score < best_score:
            best_score = score
            best_k = k

    offset = best_k * cycle_slots

    print(f"\n[OFFSET ESTIMATION]")
    print(f"  VNF first abs_slot_raw = {vnf_first}")
    print(f"  PNF first abs_slot_raw = {pnf_first}")
    print(f"  selected cycle shift   = {best_k}")
    print(f"  selected slot offset   = {offset}")
    print(f"  first-slot distance    = {best_score} slots")

    return offset


def apply_abs_slot_offset(data, offset, name="DATA"):
    """
    對 abs_slot 套用估計出的 offset。
    """

    shifted = []

    for d in data:
        new_d = d.copy()
        new_d["abs_slot"] = d["abs_slot_raw"] + offset
        shifted.append(new_d)

    if shifted:
        print(f"\n[APPLY OFFSET] {name}")
        print(f"  offset            = {offset}")
        print(f"  abs_slot min/max  = {shifted[0]['abs_slot']} / {shifted[-1]['abs_slot']}")

    return shifted


# ============================================================
# diagnostics
# ============================================================
def print_data_range(data, name):
    if not data:
        print(f"[INFO] {name} is empty")
        return

    sfn_values = [d["sfn"] for d in data]
    slot_values = [d["slot"] for d in data]
    payload_values = [d["payload"] for d in data]

    print(f"\n[RANGE] {name}")
    print(f"  points           = {len(data)}")
    print(f"  sfn min/max      = {min(sfn_values)} / {max(sfn_values)}")
    print(f"  slot min/max     = {min(slot_values)} / {max(slot_values)}")
    print(f"  payload min/max  = {min(payload_values):.6f} / {max(payload_values):.6f} ms")


def check_abs_slot_pattern(data, name):
    """
    檢查 absolute slot 的分佈。
    """

    if not data:
        return

    abs_slots = [d["abs_slot"] for d in data]
    counter = Counter(abs_slots)

    duplicated = {k: v for k, v in counter.items() if v > 1}

    print(f"\n[ABS SLOT CHECK] {name}")
    print(f"  total points           = {len(data)}")
    print(f"  unique abs slots       = {len(counter)}")
    print(f"  duplicated abs slots   = {len(duplicated)}")

    if len(data) >= 2:
        diffs = []
        for i in range(1, len(data)):
            diffs.append(abs_slots[i] - abs_slots[i - 1])

        print(f"  abs_slot diff min/max  = {min(diffs)} / {max(diffs)}")

        zero_diff = sum(1 for x in diffs if x == 0)
        neg_diff = sum(1 for x in diffs if x < 0)
        gap_diff = sum(1 for x in diffs if x > 1)

        print(f"  zero diffs             = {zero_diff}")
        print(f"  negative diffs         = {neg_diff}")
        print(f"  gap diffs > 1          = {gap_diff}")

    if duplicated:
        print("  top duplicated abs slots:")
        for abs_slot, count in sorted(duplicated.items(), key=lambda x: x[1], reverse=True)[:10]:
            examples = []
            for d in data:
                if d["abs_slot"] == abs_slot:
                    examples.append(
                        f"idx={d['idx']}, sfn={d['sfn']}, slot={d['slot']}, payload={d['payload']:.6f}"
                    )
                if len(examples) >= 5:
                    break

            print(f"    abs_slot={abs_slot}, count={count}")
            for ex in examples:
                print(f"      {ex}")


# ============================================================
# PNF aggregation and mapping
# ============================================================
def aggregate_pnf_events(events, method):
    """
    同一個 abs_slot 可能有多筆 PNF event。
    為了畫在 VNF 每 slot time axis 上，需要壓成一個值。

    method:
      min     : 取最小 Δt_arrive，最保守，適合 deadline miss 分析
      max     : 取最大
      first   : 取該 slot 第一筆
      last    : 取該 slot 最後一筆
      mean    : 取平均
      median  : 取中位數
    """

    values = [e["payload"] for e in events]

    if method == "min":
        return min(values)
    elif method == "max":
        return max(values)
    elif method == "first":
        return events[0]["payload"]
    elif method == "last":
        return events[-1]["payload"]
    elif method == "mean":
        return sum(values) / len(values)
    elif method == "median":
        values_sorted = sorted(values)
        n = len(values_sorted)
        mid = n // 2
        if n % 2 == 1:
            return values_sorted[mid]
        return 0.5 * (values_sorted[mid - 1] + values_sorted[mid])
    else:
        raise ValueError(f"Unknown aggregate method: {method}")


def map_pnf_to_vnf_by_abs_slot(vnf_data, pnf_data, aggregate="min", pnf_slot_offset=0):
    """
    正確 mapping 邏輯：

    VNF:
      每 slot 一筆，所以 VNF abs_slot 是完整 x-axis。

    PNF:
      只有有事件才有資料，所以 PNF abs_slot 是 sparse。
      因此 mapping 時：
        PNF target slot = PNF abs_slot + pnf_slot_offset

    如果同一個 target slot 有多筆 PNF event，使用 aggregate 壓成一筆。
    """

    pnf_by_slot = defaultdict(list)

    for p in pnf_data:
        target_slot = p["abs_slot"] + pnf_slot_offset
        pnf_by_slot[target_slot].append(p)

    aligned_pnf = []
    aligned_pnf_event_count = []
    aligned_pnf_negative_event_count = []

    mapped_slots = 0
    duplicate_slots = 0

    violation_rows = []

    for v_idx, v in enumerate(vnf_data):
        abs_slot = v["abs_slot"]
        events = pnf_by_slot.get(abs_slot, [])

        if not events:
            aligned_pnf.append(None)
            aligned_pnf_event_count.append(0)
            aligned_pnf_negative_event_count.append(0)
            continue

        mapped_slots += 1

        if len(events) > 1:
            duplicate_slots += 1

        value = aggregate_pnf_events(events, aggregate)
        aligned_pnf.append(value)

        negative_event_count = sum(1 for e in events if e["payload"] < 0)
        aligned_pnf_event_count.append(len(events))
        aligned_pnf_negative_event_count.append(negative_event_count)

        if v["payload"] < value:
            chosen_examples = ", ".join([f"{e['payload']:.3f}" for e in events[:5]])
            violation_rows.append({
                "vnf_idx": v_idx,
                "abs_slot": abs_slot,
                "vnf_sfn": v["sfn"],
                "vnf_slot": v["slot"],
                "vnf_payload": v["payload"],
                "pnf_aggregated": value,
                "pnf_event_count": len(events),
                "pnf_examples": chosen_examples,
                "diff": v["payload"] - value,
            })

    print(f"\n[MAPPING RESULT]")
    print(f"  mapping key                  = reconstructed absolute slot")
    print(f"  PNF slot offset              = {pnf_slot_offset}")
    print(f"  PNF aggregate method         = {aggregate}")
    print(f"  VNF total slots              = {len(vnf_data)}")
    print(f"  mapped PNF slots             = {mapped_slots}")
    print(f"  missing PNF slots            = {len(vnf_data) - mapped_slots}")
    print(f"  slots with multiple PNF evts = {duplicate_slots}")
    print(f"  violation count              = {len(violation_rows)}")
    print(f"  violation condition          = VNF ahead time < aggregated Δt_arrive")

    if violation_rows:
        print("\n[WARNING] 前 30 筆 violation:")
        for r in violation_rows[:30]:
            print(
                f"  vnf_idx={r['vnf_idx']}, "
                f"abs_slot={r['abs_slot']}, "
                f"vnf=({r['vnf_sfn']},{r['vnf_slot']}), "
                f"vnf_payload={r['vnf_payload']:.6f} ms, "
                f"pnf_agg={r['pnf_aggregated']:.6f} ms, "
                f"pnf_event_count={r['pnf_event_count']}, "
                f"pnf_examples=[{r['pnf_examples']}], "
                f"diff={r['diff']:.6f} ms"
            )

    return aligned_pnf, aligned_pnf_event_count, aligned_pnf_negative_event_count, violation_rows


# ============================================================
# plotting
# ============================================================
def plot_data(vnf_y, pnf_y, x_indices, output_name, x_limit=None, title=None):
    """
    繪製 VNF ahead time 與 PNF Δt_arrive。
    """

    fig, ax = plt.subplots(figsize=SQUARE_FIGSIZE)

    ax.plot(
        x_indices,
        vnf_y,
        marker=VNF_MARKER,
        linestyle='-',
        color='#3498db',
        linewidth=2,
        markersize=MARKER_SIZE,
        label='VNF ahead time',
        zorder=4
    )

    pnf_x_valid = [x for x, y in zip(x_indices, pnf_y) if y is not None]
    pnf_y_valid = [y for y in pnf_y if y is not None]

    ax.plot(
        pnf_x_valid,
        pnf_y_valid,
        marker=PNF_MARKER,
        linestyle='--',
        color='#9b59b6',
        linewidth=2,
        markersize=MARKER_SIZE,
        label=r'$\Delta t_{\mathrm{arrive}}$',
        zorder=3
    )

    ax.set_ylim(-1, 5)

    ax.axhline(
        0,
        color=DEADLINE_COLOR,
        linewidth=2,
        linestyle='-',
        zorder=2
    )

    if x_indices:
        ax.annotate(
            'deadline',
            xy=(x_indices[0], 0),
            xytext=(5, 5),
            textcoords='offset points',
            color=DEADLINE_COLOR,
            fontweight='bold'
        )

    if x_limit is not None:
        ax.set_xlim(x_limit[0], x_limit[1])

    if title:
        ax.set_title(title)

    ax.set_xlabel('VNF Index')
    ax.set_ylabel('Time (ms)')
    ax.grid(True, linestyle=':', alpha=0.7)
    ax.legend(loc='upper left')

    fig.tight_layout()
    plt.savefig(output_name, dpi=300, bbox_inches='tight')
    plt.close(fig)

    print(f"[OUTPUT] 圖表已儲存: {output_name}")


def plot_cumulative_negative(
    pnf_y,
    pnf_negative_event_count,
    pnf_event_count,
    x_indices,
    output_name,
    x_limit=None,
    count_mode="slot"
):
    """
    畫 custom 區間的 Δt_arrive < 0 累積統計。

    count_mode:
      slot  : 一個 VNF slot 只要 aggregated PNF < 0，就累計 1
      event : 統計該 slot 內所有 PNF event 中 payload < 0 的事件數
    """

    cumulative_counts = []
    running_count = 0

    if count_mode == "slot":
        for y in pnf_y:
            if y is not None and y < 0:
                running_count += 1
            cumulative_counts.append(running_count)

        total_valid = sum(1 for y in pnf_y if y is not None)
        total_negative = running_count
        stat_name = "negative mapped slots"

    elif count_mode == "event":
        for neg_count in pnf_negative_event_count:
            running_count += neg_count
            cumulative_counts.append(running_count)

        total_valid = sum(pnf_event_count)
        total_negative = running_count
        stat_name = "negative PNF events"

    else:
        raise ValueError(f"Unknown count_mode: {count_mode}")

    ratio = (total_negative / total_valid * 100.0) if total_valid > 0 else 0.0

    print(f"\n[CUSTOM INTERVAL STATISTICS]")
    print(f"  count mode             = {count_mode}")
    print(f"  valid denominator      = {total_valid}")
    print(f"  Δt_arrive < 0 count    = {total_negative}")
    print(f"  Δt_arrive < 0 ratio    = {ratio:.3f}%")
    print(f"  statistic              = {stat_name}")

    fig, ax = plt.subplots(figsize=SQUARE_FIGSIZE)

    ax.plot(
        x_indices,
        cumulative_counts,
        marker=PNF_MARKER,
        linestyle='-',
        color='#9b59b6',
        linewidth=2,
        markersize=MARKER_SIZE,
        label=r'Cumulative $\Delta t_{\mathrm{arrive}} < 0$',
        zorder=3
    )

    ax.fill_between(
        x_indices,
        cumulative_counts,
        color='#9b59b6',
        alpha=0.10,
        zorder=2
    )

    if x_limit is not None:
        ax.set_xlim(x_limit[0], x_limit[1])

    ax.set_ylim(bottom=0)
    ax.set_xlabel('VNF Index')
    ax.set_ylabel('Cumulative count')
    ax.grid(True, linestyle=':', alpha=0.7)
    ax.legend(loc='upper left')

    fig.tight_layout()
    plt.savefig(output_name, dpi=300, bbox_inches='tight')
    plt.close(fig)

    print(f"[OUTPUT] 累積圖表已儲存: {output_name}")


# ============================================================
# main
# ============================================================
def main():
    parser = argparse.ArgumentParser(
        description="Analyze VNF/PNF timing logs using reconstructed absolute slot mapping"
    )

    parser.add_argument(
        "--vnf-file",
        required=True,
        type=str,
        help="VNF .bin file path"
    )

    parser.add_argument(
        "--pnf-file",
        required=True,
        type=str,
        help="PNF .bin file path"
    )

    parser.add_argument(
        "--start",
        type=int,
        default=0,
        help="Custom interval start VNF index"
    )

    parser.add_argument(
        "--end",
        type=int,
        default=100,
        help="Custom interval end VNF index, inclusive"
    )

    parser.add_argument(
        "--max-points",
        type=int,
        default=100000,
        help="Maximum number of points to read from each file"
    )

    parser.add_argument(
        "--slots-per-frame",
        type=int,
        default=20,
        help="NR slots per frame. For mu=1, this is 20."
    )

    parser.add_argument(
        "--sfn-mod",
        type=int,
        default=1024,
        help="SFN modulo value. Usually 1024."
    )

    parser.add_argument(
        "--aggregate",
        type=str,
        default="min",
        choices=["min", "max", "first", "last", "mean", "median"],
        help="How to aggregate multiple PNF events in the same absolute slot"
    )

    parser.add_argument(
        "--pnf-slot-offset",
        type=int,
        default=0,
        help=(
            "Optional slot offset applied to PNF abs_slot before mapping. "
            "Use this if PNF event semantically corresponds to VNF slot+k."
        )
    )

    parser.add_argument(
        "--count-mode",
        type=str,
        default="slot",
        choices=["slot", "event"],
        help=(
            "Cumulative <0 statistic mode. "
            "slot: count aggregated negative slots. "
            "event: count all negative PNF events."
        )
    )

    parser.add_argument(
        "--search-cycles",
        type=int,
        default=8,
        help="Search range for automatic PNF/VNF cycle offset estimation"
    )

    args = parser.parse_args()

    # ========================================================
    # 1. read raw logs
    # ========================================================
    vnf_raw = read_packed_log(args.vnf_file, max_points=args.max_points)
    pnf_raw = read_packed_log(args.pnf_file, max_points=args.max_points)

    if not vnf_raw:
        print("[ERROR] VNF 資料為空，結束程式。")
        return

    if not pnf_raw:
        print("[ERROR] PNF 資料為空，結束程式。")
        return

    print_data_range(vnf_raw, "VNF RAW")
    print_data_range(pnf_raw, "PNF RAW")

    # ========================================================
    # 2. reconstruct abs slot independently
    # ========================================================
    vnf_abs = reconstruct_absolute_slot(
        vnf_raw,
        slots_per_frame=args.slots_per_frame,
        sfn_mod=args.sfn_mod,
        name="VNF"
    )

    pnf_abs_raw = reconstruct_absolute_slot(
        pnf_raw,
        slots_per_frame=args.slots_per_frame,
        sfn_mod=args.sfn_mod,
        name="PNF"
    )

    # ========================================================
    # 3. estimate and apply PNF cycle offset
    # ========================================================
    auto_offset = estimate_pnf_cycle_offset(
        vnf_abs,
        pnf_abs_raw,
        slots_per_frame=args.slots_per_frame,
        sfn_mod=args.sfn_mod,
        search_cycles=args.search_cycles
    )

    pnf_abs = apply_abs_slot_offset(
        pnf_abs_raw,
        auto_offset,
        name="PNF"
    )

    # ========================================================
    # 4. diagnostics after abs slot reconstruction
    # ========================================================
    check_abs_slot_pattern(vnf_abs, "VNF")
    check_abs_slot_pattern(pnf_abs, "PNF")

    # ========================================================
    # 5. map sparse PNF to full VNF axis
    # ========================================================
    aligned_pnf, aligned_event_count, aligned_negative_event_count, violation_rows = map_pnf_to_vnf_by_abs_slot(
        vnf_data=vnf_abs,
        pnf_data=pnf_abs,
        aggregate=args.aggregate,
        pnf_slot_offset=args.pnf_slot_offset
    )

    vnf_y = [d["payload"] for d in vnf_abs]
    x_indices = list(range(len(vnf_y)))

    # ========================================================
    # 6. plot full raw data
    # ========================================================
    plot_data(
        vnf_y=vnf_y,
        pnf_y=aligned_pnf,
        x_indices=x_indices,
        output_name="raw_data_all.png",
        x_limit=None,
        title=None
    )

    # ========================================================
    # 7. plot custom interval
    # ========================================================
    start_idx = max(0, args.start)
    end_idx = min(len(vnf_y) - 1, args.end)

    if start_idx < end_idx:
        custom_vnf = vnf_y[start_idx:end_idx + 1]
        custom_pnf = aligned_pnf[start_idx:end_idx + 1]
        custom_event_count = aligned_event_count[start_idx:end_idx + 1]
        custom_negative_event_count = aligned_negative_event_count[start_idx:end_idx + 1]
        custom_x = x_indices[start_idx:end_idx + 1]

        plot_data(
            vnf_y=custom_vnf,
            pnf_y=custom_pnf,
            x_indices=custom_x,
            output_name="raw_data_custom_interval.png",
            x_limit=(start_idx, end_idx),
            title=None
        )

        plot_cumulative_negative(
            pnf_y=custom_pnf,
            pnf_negative_event_count=custom_negative_event_count,
            pnf_event_count=custom_event_count,
            x_indices=custom_x,
            output_name="raw_data_custom_interval_cumulative.png",
            x_limit=(start_idx, end_idx),
            count_mode=args.count_mode
        )
    else:
        print("[ERROR] 自訂區間索引錯誤或範圍過小，無法繪製 custom interval。")
        print(f"        start_idx = {start_idx}")
        print(f"        end_idx   = {end_idx}")


if __name__ == "__main__":
    main()