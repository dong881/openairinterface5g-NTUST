import struct
import ctypes
import matplotlib.pyplot as plt
import os
import argparse
import re
import time as time_module
from concurrent.futures import ThreadPoolExecutor, as_completed
import tempfile
import shutil
import threading

# ============================================================
# 論文圖表全局設定
# ============================================================
plt.rcParams['font.family'] = 'serif'
plt.rcParams['font.size'] = 14
plt.rcParams['axes.labelsize'] = 16
plt.rcParams['xtick.labelsize'] = 14
plt.rcParams['ytick.labelsize'] = 14
plt.rcParams['legend.fontsize'] = 12

# Matplotlib mathtext/rendering is not thread-safe.
# Serialize layout/render/save to avoid intermittent ParseException in batch mode.
PLOT_RENDER_LOCK = threading.Lock()


def safe_tight_layout_and_save(fig, output_img, pad=0.5, apply_tight_layout=True):
    output_dir = os.path.dirname(output_img)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    print(f"Saving plot to {output_img} (dir exists: {os.path.isdir(output_dir)})")

    with PLOT_RENDER_LOCK:
        if apply_tight_layout:
            fig.tight_layout(pad=pad)
        atomic_save_figure(fig, output_img, dpi=300, bbox_inches='tight')


# ============================================================
# 參數設定
# ============================================================

# 所有區間圖固定顯示的總點數
DISPLAY_TOTAL_POINTS = 80

# 採樣率標記（用於檔案名稱）
PTS_MARK = f'@{DISPLAY_TOTAL_POINTS}pts'

# Y 軸範圍常數
# 時間類型圖: -1 ~ 5
TIME_Y_MIN = -1
TIME_Y_MAX = 5
# 計數 / 累積類型: 0 ~ 30
COUNT_Y_MIN = 0
COUNT_Y_MAX = 30

# PNF (紫色) 統一符號
PNF_MARKER = 's'
PNF_MARKERSIZE = 7
VNF_MARKER = 'o'
VNF_MARKERSIZE = 7

# 統一方形輸出比例
SQUARE_FIGSIZE = (4.8, 4.8)
SUMMARY_FIGSIZE = (7.6, 7.6)

# 顏色設定
VNF_COLOR = '#3498db'
PNF_COLOR = '#9b59b6'
DEADLINE_COLOR = '#e74c3c'
FALLING_COLOR = '#e67e22'

# 第三張完整圖，從 rise 到 fall 的前後保留點數
FULL_WINDOW_MARGIN = 15

# zoom 圖在 rise / fall 邊緣前後保留的點數
ZOOM_MARGIN = 15

# 差分門檻
DIFF_THRESHOLD = 0

# 階梯狀跳變建議設 1
MIN_CONSECUTIVE_POINTS = 1

# 是否輸出所有上升/下降區間
PLOT_ALL_INTERVALS = False

# PNF 壓力響應模式檢測參數
# 檢測模式：VNF平穩 → PNF下降 → VNF上升
VNF_FLAT_THRESHOLD = 50  # VNF 變化率門檻 (μs/sample)，低於此值視為平穩
PNF_DROP_THRESHOLD = -100  # PNF 下降門檻 (μs)，負於此值視為下降
RISE_START_THRESHOLD = 50  # VNF 開始上升的差分門檻 (μs)，用於檢測響應開始
PNF_FLAT_WINDOW = 10  # 確認 VNF 平穩的窗口大小（連續點數）
PNF_RESPONSE_DELAY = 5  # PNF 下降和 VNF 上升之間允許的最大延遲（點數）

# 最多讀取幾筆資料
MAX_POINTS = 100000

# 自動挑選區間策略
# first: 選第一個 rise/fall
# active: 選跳動最密集區域附近的 rise/fall
INTERVAL_SELECTION_MODE = "active"

# active 模式下搜尋跳動密集區的窗口大小
ACTIVE_WINDOW_SIZE = 400
# PNF-aware 區間選擇時，rise/fall 前後搜尋 PNF 點的範圍
PNF_MARGIN = 40

# 預設檔名前綴（用於資料夾批次配對）
VNF_PREFIX = "vnf_advance_time-us.000"
PNF_PREFIX = "pnf_timing_window-us.000"

# PNF 壓力響應顏色
PNF_RESPONSE_COLOR = '#2ecc71'  # 綠色用於標示 PNF 壓力響應區間


def safe_tag(name):
    # 轉成可用於資料夾名稱的安全字串
    return re.sub(r'[^A-Za-z0-9._-]+', '_', str(name)).strip('_') or 'base'


def parse_file_label(filename, prefix):
    """
    從檔名擷取標籤。
    例:
      vnf_advance_time-us.000 (1k).bin -> 1k
      vnf_advance_time-us.000.bin -> base
    """
    escaped = re.escape(prefix)
    pattern = rf'^{escaped}(?:\s*\(([^)]+)\))?\.bin$'
    match = re.match(pattern, filename)
    if not match:
        return None
    return match.group(1) or 'base'


def discover_vnf_pnf_pairs(input_dir):
    vnf_map = {}
    pnf_map = {}

    for name in os.listdir(input_dir):
        full_path = os.path.join(input_dir, name)
        if not os.path.isfile(full_path):
            continue

        vnf_label = parse_file_label(name, VNF_PREFIX)
        if vnf_label is not None:
            vnf_map[vnf_label] = full_path
            continue

        pnf_label = parse_file_label(name, PNF_PREFIX)
        if pnf_label is not None:
            pnf_map[pnf_label] = full_path

    labels = sorted(set(vnf_map.keys()) & set(pnf_map.keys()))
    pairs = []
    for label in labels:
        pairs.append((label, vnf_map[label], pnf_map[label]))

    missing_vnf = sorted(set(pnf_map.keys()) - set(vnf_map.keys()))
    missing_pnf = sorted(set(vnf_map.keys()) - set(pnf_map.keys()))

    return pairs, missing_vnf, missing_pnf


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description='Plot VNF/PNF timing figures for one pair or all pairs in a folder.'
    )
    parser.add_argument('--vnf-file', type=str, help='Path to one VNF .bin file')
    parser.add_argument('--pnf-file', type=str, help='Path to one PNF .bin file')
    parser.add_argument('--input-dir', type=str, help='Folder containing VNF/PNF .bin files for batch plotting')
    parser.add_argument('--output-dir', type=str, default='plot_outputs', help='Output root folder for generated images')
    parser.add_argument('--custom-start', type=int, help='Start index for custom interval plotting')
    parser.add_argument('--custom-end', type=int, help='End index for custom interval plotting')
    parser.add_argument('--no-parallel', action='store_true', help='Run batch processing in single-threaded mode (for debugging)')

    return parser


# ============================================================
# 讀取 packed binary log
# ============================================================
def read_packed_log(filepath, max_points=100000):
    data = []

    if not os.path.exists(filepath):
        print(f"警告: 找不到檔案 {filepath}")
        return data

    with open(filepath, "rb") as f:
        while len(data) < max_points:
            chunk = f.read(8)

            if len(chunk) < 8:
                break

            raw_value, = struct.unpack("<q", chunk)

            sfn = (raw_value >> 48) & 0xFFFF
            slot = (raw_value >> 32) & 0xFFFF
            payload = ctypes.c_int32(raw_value & 0xFFFFFFFF).value

            data.append({
                'sfn': sfn,
                'slot': slot,
                'payload': payload
            })

    return data

def align_pnf_to_vnf_sequence(vnf_data, pnf_data):
    """
    Strict exact SFN/slot mapping.

    規則：
    1. 不允許 arbitrary slot offset。
    2. 不允許 nearest-neighbor。
    3. 不允許 per-sample drift。
    4. 只允許 VNF 和 PNF 有相同 (sfn % 1024, slot) 時才 mapping。
    5. 如果沒有共同 SFN/slot，就回傳全 None，並明確印出 mapping failed。
    """

    from collections import defaultdict, deque
    import time as time_module

    if not vnf_data or not pnf_data:
        return [None] * len(vnf_data)

    t0 = time_module.time()

    SLOTS_PER_FRAME_OVERRIDE = 20

    if SLOTS_PER_FRAME_OVERRIDE is not None:
        slots_per_frame = int(SLOTS_PER_FRAME_OVERRIDE)
    else:
        max_slot = 0
        for d in vnf_data:
            max_slot = max(max_slot, int(d.get('slot', 0)))
        for d in pnf_data:
            max_slot = max(max_slot, int(d.get('slot', 0)))
        slots_per_frame = max_slot + 1

    print("  正在執行 strict exact (SFN, slot) mapping...")
    print(f"  slots_per_frame={slots_per_frame}")

    print(
        f"  VNF first: sfn={vnf_data[0]['sfn']} slot={vnf_data[0]['slot']} "
        f"payload={vnf_data[0]['payload']}"
    )
    print(
        f"  PNF first: sfn={pnf_data[0]['sfn']} slot={pnf_data[0]['slot']} "
        f"payload={pnf_data[0]['payload']}"
    )

    def key_of(d):
        return (int(d['sfn']) % 1024, int(d['slot']))

    # 建立 PNF exact key map
    pnf_by_key = defaultdict(deque)

    for pi, d in enumerate(pnf_data):
        key = key_of(d)
        pnf_by_key[key].append({
            'payload': d['payload'],
            'pnf_index': pi,
            'sfn': d['sfn'],
            'slot': d['slot'],
        })

    # 診斷 common key 數量
    vnf_keys = [key_of(d) for d in vnf_data]
    pnf_key_set = set(pnf_by_key.keys())
    common_key_count = sum(1 for k in vnf_keys if k in pnf_key_set)

    print(
        f"  VNF samples whose (sfn,slot) exists in PNF key set: "
        f"{common_key_count}/{len(vnf_data)}"
    )

    aligned_pnf = []
    mapped_count = 0
    mapped_debug = []
    unmatched_debug = []

    for vi, vd in enumerate(vnf_data):
        key = key_of(vd)

        if pnf_by_key[key]:
            item = pnf_by_key[key].popleft()
            aligned_pnf.append(item['payload'])
            mapped_count += 1

            if len(mapped_debug) < 30:
                mapped_debug.append({
                    'vnf_index': vi,
                    'vnf_sfn': vd['sfn'],
                    'vnf_slot': vd['slot'],
                    'vnf_payload': vd['payload'],
                    'pnf_index': item['pnf_index'],
                    'pnf_sfn': item['sfn'],
                    'pnf_slot': item['slot'],
                    'pnf_payload': item['payload'],
                })
        else:
            aligned_pnf.append(None)

            if len(unmatched_debug) < 20:
                unmatched_debug.append({
                    'vnf_index': vi,
                    'vnf_sfn': vd['sfn'],
                    'vnf_slot': vd['slot'],
                    'vnf_payload': vd['payload'],
                })

    total_vnf = len(vnf_data)
    total_pnf = len(pnf_data)

    vnf_coverage_rate = mapped_count / total_vnf if total_vnf else 0.0
    pnf_used_rate = mapped_count / total_pnf if total_pnf else 0.0

    elapsed = time_module.time() - t0

    print(
        f"PNF->VNF strict exact (SFN,slot) 映射: "
        f"{mapped_count}/{total_vnf} VNF points ({vnf_coverage_rate:.2%}), "
        f"{mapped_count}/{total_pnf} PNF points used ({pnf_used_rate:.2%}) "
        f"[{elapsed:.2f}s]"
    )

    if mapped_count == 0:
        print("\n錯誤: strict exact mapping 完全失敗。")
        print("沒有任何 VNF sample 找到相同的 PNF (sfn, slot)。")
        print("這代表：")
        print("1. VNF/PNF 的 SFN/slot 不是同一個時間基準。")
        print("2. PNF log 的 SFN/slot 不是 event slot，而是另一個 reference slot。")
        print("3. 兩個檔案不是同一段 run。")
        print("4. 如果你想 mapping，必須在 log 裡寫入共同 timestamp 或明確定義固定 event offset。")

        print("\n前 20 筆 unmatched VNF:")
        for m in unmatched_debug:
            print(
                f"    VNF[{m['vnf_index']}] "
                f"sfn={m['vnf_sfn']} slot={m['vnf_slot']} "
                f"payload={m['vnf_payload']}"
            )

        print()
        return aligned_pnf

    print("\n  前 30 筆 successful strict exact mapping debug:")
    for m in mapped_debug:
        print(
            f"    VNF[{m['vnf_index']}] "
            f"sfn={m['vnf_sfn']} slot={m['vnf_slot']} payload={m['vnf_payload']} "
            f"<-- PNF[{m['pnf_index']}] "
            f"sfn={m['pnf_sfn']} slot={m['pnf_slot']} payload={m['pnf_payload']}"
        )
    print()

    return aligned_pnf


def count_pnf_points_in_window(aligned_pnf, start_idx, end_idx, margin=40):
    if not aligned_pnf:
        return 0

    n = len(aligned_pnf)
    s = max(0, start_idx - margin)
    e = min(n - 1, end_idx + margin)

    return sum(1 for x in aligned_pnf[s:e + 1] if x is not None)

def select_intervals_with_pnf_coverage(
    rise_intervals,
    fall_intervals,
    diffs,
    aligned_pnf,
    mode="active",
    active_window_size=400,
    pnf_margin=40
):
    """
    在原本 active selection 的基礎上，優先選擇附近有 PNF 點的 rise/fall。
    避免畫出只有 VNF、沒有 PNF 的圖。
    """

    if not rise_intervals or not fall_intervals:
        return None, None, None

    active_window = find_most_active_window(diffs, window_size=active_window_size)

    rise_candidates = [
        it for it in rise_intervals
        if _overlap_len(it, active_window) > 0
    ]
    fall_candidates = [
        it for it in fall_intervals
        if _overlap_len(it, active_window) > 0
    ]

    if not rise_candidates:
        rise_candidates = rise_intervals

    if not fall_candidates:
        fall_candidates = fall_intervals

    best_pair = None
    best_score = None

    for r in rise_candidates:
        for f in fall_candidates:
            if f[0] < r[0]:
                continue

            span = (r[0], f[1])
            active_overlap = _overlap_len(span, active_window)
            span_len = span[1] - span[0] + 1

            rise_pnf_count = count_pnf_points_in_window(
                aligned_pnf,
                r[0],
                r[1],
                margin=pnf_margin
            )
            fall_pnf_count = count_pnf_points_in_window(
                aligned_pnf,
                f[0],
                f[1],
                margin=pnf_margin
            )
            span_pnf_count = count_pnf_points_in_window(
                aligned_pnf,
                span[0],
                span[1],
                margin=pnf_margin
            )

            # 分數優先順序：
            # 1. span 裡 PNF 點越多越好
            # 2. rise/fall 附近都有 PNF 越好
            # 3. active overlap 越大越好
            # 4. span 越長越好
            score = (
                span_pnf_count,
                min(rise_pnf_count, fall_pnf_count),
                rise_pnf_count + fall_pnf_count,
                active_overlap,
                span_len
            )

            if best_score is None or score > best_score:
                best_score = score
                best_pair = (r, f)

    if best_pair is not None:
        r, f = best_pair

        print(
            f"PNF-aware selection score={best_score}, "
            f"rise={r}, fall={f}"
        )

        return r, f, active_window

    return select_intervals(
        rise_intervals,
        fall_intervals,
        diffs,
        mode=mode,
        active_window_size=active_window_size
    )


def normalize_timing_values(vnf_values, pnf_values, scale_factor=None):
    valid_y_values = vnf_values + [y for y in pnf_values if y is not None]
    max_abs_y = max((abs(y) for y in valid_y_values), default=0)
    auto_scale = scale_factor is None

    if scale_factor is None:
        scale_factor = 1000.0 if max_abs_y >= 1000 else 1.0

    if auto_scale and scale_factor >= 1000.0:
        print("偵測到數值較大，自動將單位從 μs 轉換為 ms...")
        return (
            [y / scale_factor for y in vnf_values],
            [y / scale_factor if y is not None else None for y in pnf_values],
            'Time (ms)',
            scale_factor
        )

    return vnf_values, pnf_values, 'Time (μs)', scale_factor


def clamp_window_around_interval(interval_start, interval_end, total_points, data_len):
    if data_len <= 0:
        return 0, 0

    total_points = max(1, min(int(total_points), data_len))
    interval_start = max(0, min(interval_start, data_len - 1))
    interval_end = max(interval_start, min(interval_end, data_len - 1))

    interval_len = interval_end - interval_start + 1

    if interval_len >= total_points:
        start_idx = interval_start
        end_idx = min(data_len - 1, start_idx + total_points - 1)

        if end_idx - start_idx + 1 < total_points:
            start_idx = max(0, end_idx - total_points + 1)

        return start_idx, end_idx

    extra_points = total_points - interval_len
    left_pad = extra_points // 2
    right_pad = extra_points - left_pad

    start_idx = max(0, interval_start - left_pad)
    end_idx = min(data_len - 1, interval_end + right_pad)

    current_len = end_idx - start_idx + 1
    if current_len < total_points:
        deficit = total_points - current_len
        start_idx = max(0, start_idx - deficit)
        current_len = end_idx - start_idx + 1
        if current_len < total_points:
            end_idx = min(data_len - 1, end_idx + (total_points - current_len))

    return start_idx, end_idx


def apply_standard_timing_axes(ax, x_start, x_end, y_label):
    ax.set_xlim(x_start - 0.5, x_end + 0.5)
    # 根據 y_label 自動選擇適合的 Y 範圍
    ylab = (y_label or '').lower()
    if 'count' in ylab or 'cumulative' in ylab:
        ymin, ymax = COUNT_Y_MIN, COUNT_Y_MAX
    else:
        # 預設視為時間單位
        ymin, ymax = TIME_Y_MIN, TIME_Y_MAX

    ax.set_ylim(ymin, ymax)
    ax.set_xlabel('Index(count)', fontweight='bold', labelpad=10)
    ax.set_ylabel(y_label, fontweight='bold', labelpad=10)
    ax.grid(True, linestyle=':', alpha=0.7)
    ax.margins(x=0.03, y=0.05)

    ax.axhline(
        0,
        color=DEADLINE_COLOR,
        linewidth=2.2,
        alpha=0.95,
        zorder=2
    )
    ax.annotate(
        'deadline',
        xy=(0, 0),
        xycoords=('axes fraction', 'data'),
        xytext=(4, 2),
        textcoords='offset points',
        color=DEADLINE_COLOR,
        fontweight='bold',
        va='bottom',
        ha='left',
        clip_on=False
    )


def finalize_square_axes(ax):
    ax.spines['top'].set_visible(True)
    ax.spines['right'].set_visible(True)

    for spine in ax.spines.values():
        spine.set_linewidth(1.2)


def get_windowed_plot_data(vnf_data, aligned_pnf, start_idx, end_idx, display_total_points=DISPLAY_TOTAL_POINTS):
    window_start, window_end = clamp_window_around_interval(
        start_idx,
        end_idx,
        display_total_points,
        len(vnf_data)
    )

    target_vnf = vnf_data[window_start:window_end + 1]
    vnf_y = [d['payload'] for d in target_vnf]
    pnf_y = aligned_pnf[window_start:window_end + 1]
    vnf_y, pnf_y, y_label, _ = normalize_timing_values(vnf_y, pnf_y)

    return window_start, window_end, vnf_y, pnf_y, y_label


def add_interval_span(ax, start_idx, end_idx, color):
    ax.axvspan(
        start_idx,
        end_idx,
        color=color,
        alpha=0.18,
        zorder=1
    )


def style_timing_axes(ax, x_start, x_end, y_label):
    apply_standard_timing_axes(ax, x_start, x_end, y_label)

    ax.legend(
        loc='upper left',
        framealpha=0.96,
        edgecolor='#aaaaaa',
        borderpad=0.6,
        labelspacing=0.45,
        handlelength=2.4
    )

    finalize_square_axes(ax)


def tune_timing_layout(fig):
    fig.subplots_adjust(
        left=0.14,
        right=0.98,
        bottom=0.13,
        top=0.95,
        wspace=0.28,
        hspace=0.34
    )


# ============================================================

def atomic_save_figure(fig, output_path, **savefig_kwargs):
    """Save a Matplotlib figure atomically to avoid race/partial-write issues.

    Saves to a temporary file in the target directory then replaces the target.
    Prints diagnostic info if directory is missing.
    """
    output_dir = os.path.dirname(output_path)
    if output_dir and not os.path.isdir(output_dir):
        try:
            os.makedirs(output_dir, exist_ok=True)
        except Exception as e:
            print(f"Failed to create directory {output_dir}: {e}")
    # Use a temp file in the same directory to ensure atomic replace
    dir_for_tmp = output_dir if output_dir else os.getcwd()
    fd, tmpname = tempfile.mkstemp(prefix='.tmp_plot_', dir=dir_for_tmp, suffix='.png')
    os.close(fd)
    try:
        fig.savefig(tmpname, **savefig_kwargs)
        # Ensure data flushed to disk before replace
        try:
            shutil.move(tmpname, output_path)
        except Exception:
            # fallback to os.replace
            os.replace(tmpname, output_path)
    finally:
        # cleanup if still exists
        if os.path.exists(tmpname):
            try:
                os.remove(tmpname)
            except Exception:
                pass

# 自動找出 VNF 上升與下降區間
# ============================================================
def find_vnf_intervals(vnf_data, diff_threshold=0, min_points=1):
    payloads = [d['payload'] for d in vnf_data]

    if len(payloads) < 2:
        return [], [], []

    diffs = [
        payloads[i] - payloads[i - 1]
        for i in range(1, len(payloads))
    ]

    rise_intervals = []
    fall_intervals = []

    current_type = None
    start_idx = None

    for i, diff in enumerate(diffs, start=1):

        if diff > diff_threshold:
            direction = "rise"
        elif diff < -diff_threshold:
            direction = "fall"
        else:
            direction = "flat"

        if direction in ["rise", "fall"]:

            if current_type is None:
                current_type = direction
                start_idx = i - 1

            elif direction != current_type:
                end_idx = i - 1

                if end_idx - start_idx + 1 >= min_points:
                    if current_type == "rise":
                        rise_intervals.append((start_idx, end_idx))
                    else:
                        fall_intervals.append((start_idx, end_idx))

                current_type = direction
                start_idx = i - 1

        else:
            if current_type is not None:
                end_idx = i - 1

                if end_idx - start_idx + 1 >= min_points:
                    if current_type == "rise":
                        rise_intervals.append((start_idx, end_idx))
                    else:
                        fall_intervals.append((start_idx, end_idx))

                current_type = None
                start_idx = None

    # 收尾處理
    if current_type is not None and start_idx is not None:
        end_idx = len(payloads) - 1

        if end_idx - start_idx + 1 >= min_points:
            if current_type == "rise":
                rise_intervals.append((start_idx, end_idx))
            else:
                fall_intervals.append((start_idx, end_idx))

    return rise_intervals, fall_intervals, diffs


def detect_pnf_pressure_response_intervals(
    vnf_data,
    aligned_pnf,
    vnf_flat_threshold=VNF_FLAT_THRESHOLD,
    pnf_drop_threshold=PNF_DROP_THRESHOLD,
    rise_start_threshold=RISE_START_THRESHOLD,
    flat_window=PNF_FLAT_WINDOW,
    response_delay=PNF_RESPONSE_DELAY,
):
    """
    偵測 PNF 壓力回應模式：
    1. VNF 平穩（連續幾點變化小）
    2. PNF 下降（delta-t arrive 顯著下降）
    3. VNF 上升（VNF ahead 隨後開始上升）
    
    回傳：[(flat_start, pnf_drop_idx, rise_start, rise_end), ...]
    """
    if not vnf_data or not aligned_pnf:
        return []
    
    payloads = [d['payload'] for d in vnf_data]
    n = len(payloads)
    
    if n < flat_window + response_delay:
        return []
    
    diffs = [payloads[i] - payloads[i - 1] for i in range(1, n)]
    
    patterns = []
    
    # 遍歷每個可能的起始點
    for i in range(n - flat_window - response_delay):
        # 第1階段：檢查 VNF 平穩 (連續 flat_window 點的變化都小)
        flat_region = diffs[i:i + flat_window]
        if not all(abs(d) <= vnf_flat_threshold for d in flat_region):
            continue
        
        flat_start = i
        flat_end = i + flat_window - 1
        
        # 第2階段：在 flat_end 之後查找 PNF 下降
        pnf_drop_found = False
        pnf_drop_idx = None
        
        for j in range(flat_end + 1, min(flat_end + response_delay + 1, n)):
            if aligned_pnf[j] is not None and aligned_pnf[j - 1] is not None:
                pnf_change = aligned_pnf[j] - aligned_pnf[j - 1]
                if pnf_change <= pnf_drop_threshold:
                    pnf_drop_found = True
                    pnf_drop_idx = j
                    break
        
        if not pnf_drop_found:
            continue
        
        # 第3階段：在 PNF 下降之後查找 VNF 上升
        rise_found = False
        rise_start = None
        rise_end = None
        
        for k in range(pnf_drop_idx + 1, min(pnf_drop_idx + response_delay + 1, n)):
            if diffs[k - 1] >= rise_start_threshold:
                # 找到上升的起點，繼續往前查找上升的端點
                rise_start = k - 1
                rise_end = k
                
                # 延伸上升區間
                for m in range(k, n):
                    if diffs[m - 1] >= rise_start_threshold:
                        rise_end = m
                    else:
                        break
                
                rise_found = True
                break
        
        if rise_found:
            patterns.append((flat_start, pnf_drop_idx, rise_start, rise_end))
    
    return patterns


def _overlap_len(a, b):
    a0, a1 = a
    b0, b1 = b
    left = max(a0, b0)
    right = min(a1, b1)
    return max(0, right - left + 1)


def find_most_active_window(diffs, window_size=400):
    """回傳非零差分最密集的 [start, end] 視窗。"""
    n = len(diffs) + 1
    if n <= 1:
        return (0, 0)

    if window_size <= 0:
        window_size = 1
    if window_size > n:
        window_size = n

    nz = [1 if d != 0 else 0 for d in diffs]
    prefix = [0]
    for x in nz:
        prefix.append(prefix[-1] + x)

    best_count = -1
    best_start = 0

    max_start = n - window_size
    for start in range(max_start + 1):
        end = start + window_size - 1
        # 對應 diffs[start:end]
        count = prefix[end] - prefix[start]
        if count > best_count:
            best_count = count
            best_start = start

    return best_start, best_start + window_size - 1


def select_intervals(rise_intervals, fall_intervals, diffs, mode="active", active_window_size=400):
    """回傳 (rise_interval, fall_interval, active_window)。"""
    if not rise_intervals or not fall_intervals:
        return None, None, None

    if mode == "first":
        return rise_intervals[0], fall_intervals[0], None

    active_window = find_most_active_window(diffs, window_size=active_window_size)

    rise_candidates = [it for it in rise_intervals if _overlap_len(it, active_window) > 0]
    fall_candidates = [it for it in fall_intervals if _overlap_len(it, active_window) > 0]

    if not rise_candidates:
        rise_candidates = rise_intervals
    if not fall_candidates:
        fall_candidates = fall_intervals

    # 目標：選一組 rise/fall，使得 [rise.start, fall.end] 與 active window 重疊最大，
    # 避免只挑到中心附近的一小段，導致圖看起來像只跳一次。
    best_pair = None
    best_score = (-1, -1)  # (active_overlap, span_len)

    for r in rise_candidates:
        for f in fall_candidates:
            if f[0] < r[0]:
                continue

            span = (r[0], f[1])
            active_overlap = _overlap_len(span, active_window)
            span_len = span[1] - span[0] + 1
            score = (active_overlap, span_len)

            if score > best_score:
                best_score = score
                best_pair = (r, f)

    if best_pair is not None:
        return best_pair[0], best_pair[1], active_window

    # fallback: 至少保證可回傳一組合法區間
    rise_candidates = sorted(rise_candidates, key=lambda x: x[0])
    fall_candidates = sorted(fall_candidates, key=lambda x: x[0])
    for r in rise_candidates:
        right_falls = [f for f in fall_candidates if f[0] >= r[0]]
        if right_falls:
            return r, right_falls[0], active_window

    return rise_candidates[0], fall_candidates[-1], active_window


# ============================================================
# 除錯資訊
# ============================================================
def print_debug_info(vnf_data, diffs):
    payloads = [d['payload'] for d in vnf_data]

    print("\n========== VNF Debug Info ==========")
    print(f"VNF data count       : {len(payloads)}")

    if payloads:
        print(f"Payload min          : {min(payloads)}")
        print(f"Payload max          : {max(payloads)}")
        print(f"Unique payload count : {len(set(payloads))}")
        print(f"First 30 payloads    : {payloads[:30]}")
        print(f"First 30 diffs       : {diffs[:30]}")

    print("====================================\n")


# ============================================================
# 單一區間繪圖函式：rise 或 fall 各一張
# ============================================================
def plot_interval(
    vnf_data,
    aligned_pnf,
    interval,
    interval_type,
    display_total_points=DISPLAY_TOTAL_POINTS,
    output_prefix="vnf_pnf"
):
    interval_start, interval_end = interval

    print(f"\n準備繪製 {interval_type} 區間")
    print(f"原始區間 index: {interval_start} 到 {interval_end}")

    start_idx, end_idx, vnf_y, pnf_y, y_label = get_windowed_plot_data(
        vnf_data,
        aligned_pnf,
        interval_start,
        interval_end,
        display_total_points=display_total_points
    )

    print(f"繪圖視窗 index: {start_idx} 到 {end_idx}")

    x_indices = list(range(len(vnf_y)))

    fig, ax = plt.subplots(figsize=SQUARE_FIGSIZE)

    color_vnf = VNF_COLOR
    color_pnf = PNF_COLOR

    ax.plot(
        x_indices,
        vnf_y,
        marker=VNF_MARKER,
        linestyle='-',
        color=color_vnf,
        linewidth=2.5,
        markersize=VNF_MARKERSIZE,
        label='VNF ahead time',
        zorder=4
    )

    ax.plot(
        x_indices,
        pnf_y,
        marker=PNF_MARKER,
        linestyle='--',
        color=color_pnf,
        linewidth=2.5,
        markersize=PNF_MARKERSIZE,
        label=r'$\Delta t_{\text{arrive}}$',
        zorder=3
    )

    local_start = interval_start - start_idx
    local_end = interval_end - start_idx

    output_img = f"{output_prefix}_{interval_type}_interval{PTS_MARK}.png"

    output_dir = os.path.dirname(output_img)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    if interval_type == "fall":
        add_interval_span(ax, local_start, local_end, FALLING_COLOR)

    style_timing_axes(ax, 0, len(x_indices) - 1, y_label)

    safe_tight_layout_and_save(fig, output_img, pad=0.5)
    plt.close(fig)

    print(f"圖表已儲存為: {output_img}")

# ============================================================
# 完整區間繪圖函式：從 rise 到 fall 畫在同一張圖
# ============================================================
def plot_rise_to_fall_interval(
    vnf_data,
    aligned_pnf,
    rise_interval,
    fall_interval,
    display_total_points=DISPLAY_TOTAL_POINTS,
    output_img=None
):
    rise_start, rise_end = rise_interval
    fall_start, fall_end = fall_interval

    # 確保區間順序正確
    first_idx = min(rise_start, fall_start)
    last_idx = max(rise_end, fall_end)

    start_idx, end_idx = clamp_window_around_interval(
        first_idx,
        last_idx,
        display_total_points,
        len(vnf_data)
    )

    print("\n準備繪製完整 rise-to-fall 區間")
    print(f"Rise 區間 index: {rise_start} 到 {rise_end}")
    print(f"Fall 區間 index: {fall_start} 到 {fall_end}")
    print(f"完整繪圖視窗 index: {start_idx} 到 {end_idx}")

    target_vnf = vnf_data[start_idx:end_idx + 1]

    x_indices = list(range(len(target_vnf)))

    vnf_y = [d['payload'] for d in target_vnf]
    pnf_y = aligned_pnf[start_idx:end_idx + 1]
    vnf_y, pnf_y, y_label, _ = normalize_timing_values(vnf_y, pnf_y)

    fig, ax = plt.subplots(figsize=SQUARE_FIGSIZE)

    color_vnf = VNF_COLOR
    color_pnf = PNF_COLOR

    ax.plot(
        x_indices,
        vnf_y,
        marker=VNF_MARKER,
        linestyle='-',
        color=color_vnf,
        linewidth=2.2,
        markersize=VNF_MARKERSIZE - 2,
        label='VNF ahead time',
        zorder=4
    )

    ax.plot(
        x_indices,
        pnf_y,
        marker=PNF_MARKER,
        linestyle='--',
        color=color_pnf,
        linewidth=2.2,
        markersize=max(3, PNF_MARKERSIZE - 2),
        label=r'$\Delta t_{\text{arrive}}$',
        zorder=3
    )

    # 轉換成局部 x 座標
    local_rise_start = rise_start - start_idx
    local_rise_end = rise_end - start_idx

    local_fall_start = fall_start - start_idx
    local_fall_end = fall_end - start_idx

    # 標示 fall
    add_interval_span(ax, local_fall_start, local_fall_end, FALLING_COLOR)

    style_timing_axes(ax, 0, len(x_indices) - 1, y_label)

    output_dir = os.path.dirname(output_img)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)
    safe_tight_layout_and_save(fig, output_img, pad=0.5)
    plt.close(fig)

    print(f"完整 rise-to-fall 圖表已儲存為: {output_img}")

def plot_summary_overview_with_zoom(
    vnf_data,
    aligned_pnf,
    rise_interval,
    fall_interval,
    overview_margin=15,
    zoom_margin=15,
    output_img="vnf_pnf_summary_overview_zoom.png"
):
    rise_start, rise_end = rise_interval
    fall_start, fall_end = fall_interval

    first_idx = min(rise_start, fall_start)
    last_idx = max(rise_end, fall_end)

    overview_start, overview_end = clamp_window_around_interval(
        first_idx - overview_margin,
        last_idx + overview_margin,
        max(DISPLAY_TOTAL_POINTS, (last_idx - first_idx + 1) + overview_margin * 2),
        len(vnf_data)
    )

    overview_data = vnf_data[overview_start:overview_end + 1]
    overview_x = list(range(overview_start, overview_end + 1))

    overview_vnf = [d['payload'] for d in overview_data]
    overview_pnf = aligned_pnf[overview_start:overview_end + 1]

    # rise zoom 範圍
    rise_zoom_start = max(0, rise_start - zoom_margin)
    rise_zoom_end = min(len(vnf_data) - 1, rise_end + zoom_margin)

    rise_data = vnf_data[rise_zoom_start:rise_zoom_end + 1]
    rise_x = list(range(rise_zoom_start, rise_zoom_end + 1))

    rise_vnf = [d['payload'] for d in rise_data]
    rise_pnf = aligned_pnf[rise_zoom_start:rise_zoom_end + 1]

    # fall zoom 範圍
    fall_zoom_start = max(0, fall_start - zoom_margin)
    fall_zoom_end = min(len(vnf_data) - 1, fall_end + zoom_margin)

    fall_data = vnf_data[fall_zoom_start:fall_zoom_end + 1]
    fall_x = list(range(fall_zoom_start, fall_zoom_end + 1))

    fall_vnf = [d['payload'] for d in fall_data]
    fall_pnf = aligned_pnf[fall_zoom_start:fall_zoom_end + 1]

    all_values = (
        overview_vnf
        + [y for y in overview_pnf if y is not None]
        + rise_vnf
        + [y for y in rise_pnf if y is not None]
        + fall_vnf
        + [y for y in fall_pnf if y is not None]
    )

    _, _, y_label, scale_factor = normalize_timing_values(all_values, [])

    overview_vnf, overview_pnf, _, _ = normalize_timing_values(
        overview_vnf,
        overview_pnf,
        scale_factor=scale_factor
    )
    rise_vnf, rise_pnf, _, _ = normalize_timing_values(
        rise_vnf,
        rise_pnf,
        scale_factor=scale_factor
    )
    fall_vnf, fall_pnf, _, _ = normalize_timing_values(
        fall_vnf,
        fall_pnf,
        scale_factor=scale_factor
    )

    # 建立 2x2 layout，上方 overview 橫跨兩欄
    fig = plt.figure(figsize=SUMMARY_FIGSIZE)

    gs = fig.add_gridspec(
        2,
        2,
        height_ratios=[1.0, 1.15],
        hspace=0.28,
        wspace=0.28
    )

    ax_overview = fig.add_subplot(gs[0, :])
    ax_rise = fig.add_subplot(gs[1, 0])
    ax_fall = fig.add_subplot(gs[1, 1])

    color_vnf = VNF_COLOR
    color_pnf = PNF_COLOR

    # ========================================================
    # Overview plot
    # ========================================================
    ax_overview.plot(
        overview_x,
        overview_vnf,
        marker=VNF_MARKER,
        linestyle='-',
        color=color_vnf,
        linewidth=2.2,
        markersize=VNF_MARKERSIZE,
        label='VNF ahead time',
        zorder=4
    )

    ax_overview.plot(
        overview_x,
        overview_pnf,
        marker=PNF_MARKER,
        linestyle='--',
        color=color_pnf,
        linewidth=1.8,
        markersize=max(3, PNF_MARKERSIZE - 2),
        label=r'$\Delta t_{\text{arrive}}$',
        zorder=3
    )

    ax_overview.axvspan(
        fall_start,
        fall_end,
        color=FALLING_COLOR,
        alpha=0.25,
        zorder=1
    )

    style_timing_axes(
        ax_overview,
        overview_start,
        overview_end,
        y_label
    )
    ax_overview.legend(
        loc='upper center',
        bbox_to_anchor=(0.5, 1.02),
        ncol=2,
        framealpha=0.96,
        edgecolor='#aaaaaa'
    )

    # ========================================================
    # Rising zoom plot
    # ========================================================
    ax_rise.plot(
        rise_x,
        rise_vnf,
        marker=VNF_MARKER,
        linestyle='-',
        color=color_vnf,
        linewidth=2.4,
        markersize=VNF_MARKERSIZE,
        label='VNF ahead time',
        zorder=4
    )

    ax_rise.plot(
        rise_x,
        rise_pnf,
        marker=PNF_MARKER,
        linestyle='--',
        color=color_pnf,
        linewidth=2.0,
        markersize=PNF_MARKERSIZE,
        label=r'$\Delta t_{\text{arrive}}$',
        zorder=3
    )

    style_timing_axes(
        ax_rise,
        rise_zoom_start,
        rise_zoom_end,
        y_label
    )

    # ========================================================
    # Falling zoom plot
    # ========================================================
    ax_fall.plot(
        fall_x,
        fall_vnf,
        marker=VNF_MARKER,
        linestyle='-',
        color=color_vnf,
        linewidth=2.4,
        markersize=VNF_MARKERSIZE,
        label='VNF ahead time',
        zorder=4
    )

    ax_fall.plot(
        fall_x,
        fall_pnf,
        marker=PNF_MARKER,
        linestyle='--',
        color=color_pnf,
        linewidth=2.0,
        markersize=PNF_MARKERSIZE,
        label=r'$\Delta t_{\text{arrive}}$',
        zorder=3
    )

    add_interval_span(ax_fall, fall_start, fall_end, FALLING_COLOR)

    style_timing_axes(
        ax_fall,
        fall_zoom_start,
        fall_zoom_end,
        y_label
    )

    # 統一邊框風格
    for ax in [ax_overview, ax_rise, ax_fall]:
        finalize_square_axes(ax)

    safe_tight_layout_and_save(fig, output_img, pad=0.5, apply_tight_layout=False)
    plt.close(fig)


def plot_custom_interval(
    vnf_data,
    aligned_pnf,
    start_idx,
    end_idx,
    output_img="vnf_pnf_custom_interval.png",
    y_label=None
):
    """
    以使用者定義的 start_idx/end_idx 繪製區間。

    start_idx and end_idx 是相對於整個 vnf_data 的索引（inclusive）。
    如果索引超出範圍會自動 clamp；若 start_idx > end_idx 則不會繪圖。
    """
    n = len(vnf_data)
    if n == 0:
        print("VNF data is empty, cannot plot custom interval.")
        return

    # clamp
    start_idx = max(0, int(start_idx))
    end_idx = min(n - 1, int(end_idx))

    if start_idx > end_idx:
        print(f"Invalid interval: start_idx ({start_idx}) > end_idx ({end_idx}).")
        return

    print(f"準備繪製自訂區間: index {start_idx} 到 {end_idx}")

    target_vnf = vnf_data[start_idx:end_idx + 1]

    # Use global indices so plotted points correspond to original data indices
    x_indices = list(range(len(target_vnf)))
    x_global = list(range(start_idx, end_idx + 1))
    vnf_y = [d['payload'] for d in target_vnf]
    pnf_y = aligned_pnf[start_idx:end_idx + 1]

    # Force units to milliseconds for custom plot
    vnf_y = [y / 1000.0 for y in vnf_y]
    pnf_y = [y / 1000.0 if y is not None else None for y in pnf_y]
    y_label = 'Time (ms)'

    fig, ax = plt.subplots(figsize=SQUARE_FIGSIZE)

    color_vnf = VNF_COLOR
    color_pnf = PNF_COLOR

    # Plot lines using global x indices so markers align with original sample indices
    ax.plot(
        x_global,
        vnf_y,
        marker=VNF_MARKER,
        linestyle='-',
        color=color_vnf,
        linewidth=2.5,
        markersize=VNF_MARKERSIZE,
        label='VNF ahead time',
        zorder=4
    )

    # For PNF, skip None values to avoid plotting gaps as zeros
    pnf_x = [x for x, y in zip(x_global, pnf_y) if y is not None]
    pnf_vals = [y for y in pnf_y if y is not None]
    ax.plot(
        pnf_x,
        pnf_vals,
        marker=PNF_MARKER,
        linestyle='--',
        color=color_pnf,
        linewidth=2.5,
        markersize=PNF_MARKERSIZE,
        label=r'$\Delta t_{\text{arrive}}$',
        zorder=3
    )

    # Provide original index range to axis styling so ticks/limits reflect global indices
    style_timing_axes(ax, start_idx, end_idx, y_label)

    # For custom interval plots, use fixed Y range as requested
    ax.set_ylim(-1, 5)
    ax.set_ylabel(y_label, fontweight='bold', labelpad=10)

    safe_tight_layout_and_save(fig, output_img, pad=0.5)
    plt.close(fig)

    print(f"自訂區間圖表已儲存為: {output_img}")


def plot_pnf_pressure_response_pattern(
    vnf_data,
    aligned_pnf,
    pattern_tuple,
    display_total_points=DISPLAY_TOTAL_POINTS,
    output_prefix="vnf_pnf"
):
    """
    繪製 PNF 壓力回應模式。
    pattern_tuple: (flat_start, pnf_drop_idx, rise_start, rise_end)
    """
    flat_start, pnf_drop_idx, rise_start, rise_end = pattern_tuple
    
    # 計算合理的視窗範圍
    first_idx = max(0, flat_start - 5)
    last_idx = min(len(vnf_data) - 1, rise_end + 5)
    
    window_start, window_end = clamp_window_around_interval(
        first_idx,
        last_idx,
        display_total_points,
        len(vnf_data)
    )
    
    target_vnf = vnf_data[window_start:window_end + 1]
    x_indices = list(range(len(target_vnf)))
    
    vnf_y = [d['payload'] for d in target_vnf]
    pnf_y = aligned_pnf[window_start:window_end + 1]
    vnf_y, pnf_y, y_label, _ = normalize_timing_values(vnf_y, pnf_y)
    
    fig, ax = plt.subplots(figsize=SQUARE_FIGSIZE)
    
    color_vnf = VNF_COLOR
    color_pnf = PNF_COLOR
    
    ax.plot(
        x_indices,
        vnf_y,
        marker=VNF_MARKER,
        linestyle='-',
        color=color_vnf,
        linewidth=2.5,
        markersize=VNF_MARKERSIZE,
        label='VNF ahead time',
        zorder=4
    )
    
    ax.plot(
        x_indices,
        pnf_y,
        marker=PNF_MARKER,
        linestyle='--',
        color=color_pnf,
        linewidth=2.5,
        markersize=PNF_MARKERSIZE,
        label=r'$\Delta t_{\text{arrive}}$',
        zorder=3
    )
    
    # 轉換成局部座標
    local_flat_start = flat_start - window_start
    local_flat_end = min(flat_start + PNF_FLAT_WINDOW - 1 - window_start, len(x_indices) - 1)
    local_pnf_drop = pnf_drop_idx - window_start
    local_rise_start = rise_start - window_start
    local_rise_end = rise_end - window_start
    
    # 標示三個階段
    # 1. VNF 平穩區域
    if 0 <= local_flat_start < len(x_indices):
        ax.axvspan(
            local_flat_start,
            local_flat_end,
            color=PNF_RESPONSE_COLOR,
            alpha=0.15,
            zorder=1,
            label='VNF flat region'
        )
    
    # 2. PNF 下降點
    if 0 <= local_pnf_drop < len(x_indices):
        ax.axvline(
            local_pnf_drop,
            color='#e74c3c',
            linestyle=':',
            linewidth=2.0,
            alpha=0.8,
            zorder=2,
            label='PNF drop'
        )
    
    # 3. VNF 上升區域
    if 0 <= local_rise_start < len(x_indices):
        ax.axvspan(
            local_rise_start,
            local_rise_end,
            color=FALLING_COLOR,
            alpha=0.20,
            zorder=1,
            label='VNF rise response'
        )
    
    style_timing_axes(ax, 0, len(x_indices) - 1, y_label)
    
    output_img = os.path.join(
        os.path.dirname(output_prefix) if output_prefix.endswith('.png') 
        else output_prefix,
        "vnf_pnf_pressure_response_pattern" + PTS_MARK + ".png"
    )
    
    safe_tight_layout_and_save(fig, output_img, pad=0.5)
    plt.close(fig)
    
    print(f"PNF 壓力回應模式圖表已儲存為: {output_img}")
    return output_img


def plot_pnf_negative_cumulative_curve(
    aligned_pnf,
    start_idx,
    end_idx,
    output_img=f"vnf_pnf_negative_cumulative{PTS_MARK}.png",
    apply_display_limit=True
):
    if not aligned_pnf:
        print("PNF data is empty, cannot plot cumulative curve.")
        return

    start_idx = max(0, int(start_idx))
    end_idx = min(len(aligned_pnf) - 1, int(end_idx))

    if start_idx > end_idx:
        print(f"Invalid cumulative interval: start_idx ({start_idx}) > end_idx ({end_idx}).")
        return

    # 如果受限於 DISPLAY_TOTAL_POINTS，則應用視窗裁切
    if apply_display_limit:
        window_start, window_end = clamp_window_around_interval(
            start_idx,
            end_idx,
            DISPLAY_TOTAL_POINTS,
            len(aligned_pnf)
        )
    else:
        window_start = start_idx
        window_end = end_idx

    window_pnf = aligned_pnf[window_start:window_end + 1]
    cumulative_negative = []
    running_count = 0

    for value in window_pnf:
        if value is not None and value < 0:
            running_count += 1
        cumulative_negative.append(running_count)

    x_indices = list(range(len(window_pnf)))
    final_count = cumulative_negative[-1] if cumulative_negative else 0

    fig, ax = plt.subplots(figsize=SQUARE_FIGSIZE)

    ax.plot(
        x_indices,
        cumulative_negative,
        marker=PNF_MARKER,
        linestyle='-',
        color=PNF_COLOR,
        linewidth=2.4,
        markersize=PNF_MARKERSIZE,
        label='Cumulative ' + r'$\Delta t_{\text{arrive}}$' + ' < 0 count',
        zorder=3
    )

    ax.fill_between(
        x_indices,
        cumulative_negative,
        color=PNF_COLOR,
        alpha=0.10,
        zorder=2
    )

    ax.set_xlim(-0.5, len(x_indices) - 0.5)
    ax.set_ylim(COUNT_Y_MIN, COUNT_Y_MAX)
    ax.set_xlabel('Index(count)', fontweight='bold', labelpad=10)
    ax.set_ylabel('Cumulative count', fontweight='bold', labelpad=10)
    ax.grid(True, linestyle=':', alpha=0.7)
    ax.margins(x=0.03, y=0.05)

    ax.legend(loc='upper left', framealpha=0.96, edgecolor='#aaaaaa')
    finalize_square_axes(ax)

    safe_tight_layout_and_save(fig, output_img, pad=0.5)
    plt.close(fig)

    print(f"PNF 負值累積曲線圖表已儲存為: {output_img}")


def process_one_pair(vnf_file, pnf_file, output_dir, pair_label='single'):
    os.makedirs(output_dir, exist_ok=True)

    print("正在讀取並解析檔案...")
    print(f"Pair label: {pair_label}")
    print(f"VNF 檔案路徑: {vnf_file}")
    print(f"PNF 檔案路徑: {pnf_file}")
    print(f"輸出資料夾: {output_dir}")

    vnf_data = read_packed_log(
        vnf_file,
        max_points=MAX_POINTS
    )

    pnf_data = read_packed_log(
        pnf_file,
        max_points=MAX_POINTS
    )

    if not vnf_data or not pnf_data:
        print("檔案讀取失敗，請確認路徑與檔案是否存在。")
        return

    print(f"VNF 資料筆數: {len(vnf_data)}")
    print(f"PNF 資料筆數: {len(pnf_data)}")

    aligned_pnf = align_pnf_to_vnf_sequence(vnf_data, pnf_data)

    # ========================================================
    # 新增：PNF 壓力回應模式偵測
    # ========================================================
    print("\n執行 PNF 壓力回應模式偵測...")
    pnf_response_patterns = detect_pnf_pressure_response_intervals(
        vnf_data=vnf_data,
        aligned_pnf=aligned_pnf,
        vnf_flat_threshold=VNF_FLAT_THRESHOLD,
        pnf_drop_threshold=PNF_DROP_THRESHOLD,
        rise_start_threshold=RISE_START_THRESHOLD,
        flat_window=PNF_FLAT_WINDOW,
        response_delay=PNF_RESPONSE_DELAY
    )
    
    if pnf_response_patterns:
        print(f"找到 {len(pnf_response_patterns)} 個 PNF 壓力回應模式")
        print("前 5 個模式:", pnf_response_patterns[:5])
        
        # 繪製第一個找到的模式
        best_pattern = pnf_response_patterns[0]
        print(f"\n繪製最佳 PNF 壓力回應模式: {best_pattern}")
        plot_pnf_pressure_response_pattern(
            vnf_data=vnf_data,
            aligned_pnf=aligned_pnf,
            pattern_tuple=best_pattern,
            display_total_points=DISPLAY_TOTAL_POINTS,
            output_prefix=os.path.join(output_dir, "vnf_pnf")
        )
    else:
        print("未找到符合條件的 PNF 壓力回應模式")
    # ========================================================

    rise_intervals, fall_intervals, diffs = find_vnf_intervals(
        vnf_data,
        diff_threshold=DIFF_THRESHOLD,
        min_points=MIN_CONSECUTIVE_POINTS
    )

    print(f"找到 VNF 上升區間數量: {len(rise_intervals)}")
    print(f"找到 VNF 下降區間數量: {len(fall_intervals)}")

    print("前 10 個上升區間:", rise_intervals[:10])
    print("前 10 個下降區間:", fall_intervals[:10])

    if not rise_intervals and not fall_intervals:
        print("沒有找到任何 VNF 上升或下降區間。")
        print_debug_info(vnf_data, diffs)

        print("可能原因：")
        print("1. VNF payload 全部一樣，沒有變化")
        print("2. DIFF_THRESHOLD 設太大")
        print("3. MIN_CONSECUTIVE_POINTS 設太大")
        print("4. payload 解碼格式可能不正確")
        return

    non_zero_diffs = sum(1 for d in diffs if d != 0)
    print(f"非零差分點數: {non_zero_diffs} / {len(diffs)}")

    sel_rise, sel_fall, _ = select_intervals_with_pnf_coverage(
        rise_intervals=rise_intervals,
        fall_intervals=fall_intervals,
        diffs=diffs,
        aligned_pnf=aligned_pnf,
        mode=INTERVAL_SELECTION_MODE,
        active_window_size=ACTIVE_WINDOW_SIZE,
        pnf_margin=PNF_MARGIN
    )

    if sel_rise is None and rise_intervals:
        sel_rise = rise_intervals[0]
    if sel_fall is None and fall_intervals:
        sel_fall = fall_intervals[0]

    output_prefix = os.path.join(output_dir, "vnf_pnf")

    if sel_rise is not None:
        print(f"選擇的 rise 區間: {sel_rise}")
        plot_interval(
            vnf_data=vnf_data,
            aligned_pnf=aligned_pnf,
            interval=sel_rise,
            interval_type="rise",
            display_total_points=DISPLAY_TOTAL_POINTS,
            output_prefix=output_prefix
        )
    else:
        print("沒有找到上升區間，不輸出 rise 圖。")

    if sel_fall is not None:
        print(f"選擇的 fall 區間: {sel_fall}")
        plot_interval(
            vnf_data=vnf_data,
            aligned_pnf=aligned_pnf,
            interval=sel_fall,
            interval_type="fall",
            display_total_points=DISPLAY_TOTAL_POINTS,
            output_prefix=output_prefix
        )
    else:
        print("沒有找到下降區間，不輸出 fall 圖。")

    if sel_rise is not None and sel_fall is not None and sel_fall[0] >= sel_rise[0]:
        plot_rise_to_fall_interval(
            vnf_data=vnf_data,
            aligned_pnf=aligned_pnf,
            rise_interval=sel_rise,
            fall_interval=sel_fall,
            display_total_points=DISPLAY_TOTAL_POINTS,
            output_img=os.path.join(output_dir, f"vnf_pnf_rise_to_fall_interval{PTS_MARK}.png")
        )

        plot_summary_overview_with_zoom(
            vnf_data=vnf_data,
            aligned_pnf=aligned_pnf,
            rise_interval=sel_rise,
            fall_interval=sel_fall,
            overview_margin=FULL_WINDOW_MARGIN,
            zoom_margin=ZOOM_MARGIN,
            output_img=os.path.join(output_dir, f"vnf_pnf_summary_overview_zoom{PTS_MARK}.png")
        )

        cumulative_start = min(sel_rise[0], sel_fall[0])
        cumulative_end = max(sel_rise[1], sel_fall[1])
        plot_pnf_negative_cumulative_curve(
            aligned_pnf=aligned_pnf,
            start_idx=cumulative_start,
            end_idx=cumulative_end,
            output_img=os.path.join(output_dir, f"vnf_pnf_negative_cumulative{PTS_MARK}.png"),
            apply_display_limit=True
        )
        plot_pnf_negative_cumulative_curve(
            aligned_pnf=aligned_pnf,
            start_idx=cumulative_start,
            end_idx=cumulative_end,
            output_img=os.path.join(output_dir, "vnf_pnf_negative_cumulative_full.png"),
            apply_display_limit=False
        )
    elif sel_rise is not None:
        plot_pnf_negative_cumulative_curve(
            aligned_pnf=aligned_pnf,
            start_idx=sel_rise[0],
            end_idx=sel_rise[1],
            output_img=os.path.join(output_dir, f"vnf_pnf_negative_cumulative{PTS_MARK}.png"),
            apply_display_limit=True
        )
        plot_pnf_negative_cumulative_curve(
            aligned_pnf=aligned_pnf,
            start_idx=sel_rise[0],
            end_idx=sel_rise[1],
            output_img=os.path.join(output_dir, "vnf_pnf_negative_cumulative_full.png"),
            apply_display_limit=False
        )


def main():
    parser = build_arg_parser()
    args = parser.parse_args()

    output_root = os.path.abspath(args.output_dir)
    os.makedirs(output_root, exist_ok=True)

    if args.input_dir:
        input_dir = os.path.abspath(args.input_dir)
        if not os.path.isdir(input_dir):
            print(f"輸入資料夾不存在: {input_dir}")
            return

        pairs, missing_vnf, missing_pnf = discover_vnf_pnf_pairs(input_dir)
        print(f"找到可配對檔案數: {len(pairs)}")

        if missing_vnf:
            print(f"缺少 VNF 的標籤: {missing_vnf}")
        if missing_pnf:
            print(f"缺少 PNF 的標籤: {missing_pnf}")

        if not pairs:
            print("沒有找到任何可配對的 VNF/PNF 檔案。")
            return

        # Worker function for parallel processing
        def process_pair_task(label, vnf_file, pnf_file, output_root, args):
            pair_out = os.path.join(output_root, safe_tag(label))
            print("\n" + "=" * 68)
            print(f"開始處理 pair: {label}")
            process_one_pair(vnf_file, pnf_file, pair_out, pair_label=label)

            # 如果指定了自訂區間，為每個 pair 都繪製
            if args.custom_start is not None and args.custom_end is not None:
                print(f"\n繪製自訂區間 [{args.custom_start}, {args.custom_end}]...")
                vnf_data = read_packed_log(vnf_file, max_points=MAX_POINTS)
                pnf_data = read_packed_log(pnf_file, max_points=MAX_POINTS)
                aligned_pnf = align_pnf_to_vnf_sequence(vnf_data, pnf_data)
                plot_custom_interval(
                    vnf_data=vnf_data,
                    aligned_pnf=aligned_pnf,
                    start_idx=args.custom_start,
                    end_idx=args.custom_end,
                    output_img=os.path.join(pair_out, f"vnf_pnf_custom_interval{PTS_MARK}.png"),
                    y_label='Custom Interval Time'
                )

        # Use ThreadPoolExecutor for parallel processing
        if args.no_parallel:
            num_workers = 1
        else:
            num_workers = min(os.cpu_count() or 1, len(pairs))
        with ThreadPoolExecutor(max_workers=num_workers) as executor:
            futures = [
                executor.submit(process_pair_task, label, vnf_file, pnf_file, output_root, args)
                for label, vnf_file, pnf_file in pairs
            ]
            for future in as_completed(futures):
                try:
                    future.result()
                except Exception as e:
                    print(f"Error processing pair: {e}")

        print("\n全部 pair 處理完成。")
        return

    vnf_file = args.vnf_file
    pnf_file = args.pnf_file

    if not vnf_file:
        vnf_file = input("請輸入 VNF 檔案路徑: ").strip()
    if not pnf_file:
        pnf_file = input("請輸入 PNF 檔案路徑: ").strip()

    if not vnf_file or not pnf_file:
        print("VNF/PNF 路徑不可為空。")
        return

    output_single_dir = os.path.join(output_root, 'single')
    process_one_pair(
        vnf_file=os.path.abspath(vnf_file),
        pnf_file=os.path.abspath(pnf_file),
        output_dir=output_single_dir,
        pair_label='single'
    )

    # 單檔模式也支援自訂區間
    if args.custom_start is not None and args.custom_end is not None:
        print(f"\n繪製自訂區間 [{args.custom_start}, {args.custom_end}]...")
        vnf_data = read_packed_log(os.path.abspath(vnf_file), max_points=MAX_POINTS)
        pnf_data = read_packed_log(os.path.abspath(pnf_file), max_points=MAX_POINTS)
        aligned_pnf = align_pnf_to_vnf_sequence(vnf_data, pnf_data)
        plot_custom_interval(
            vnf_data=vnf_data,
            aligned_pnf=aligned_pnf,
            start_idx=args.custom_start,
            end_idx=args.custom_end,
            output_img=os.path.join(output_single_dir, f"vnf_pnf_custom_interval{PTS_MARK}.png"),
            y_label='Custom Interval Time'
        )

    print("\n全部處理完成。")


if __name__ == "__main__":
    main()