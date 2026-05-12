import struct
import ctypes
import matplotlib.pyplot as plt
import argparse
import os

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
MARKER_SIZE = 3  # 統一縮小 marker size
DEADLINE_COLOR = '#e74c3c'

def read_packed_log(filepath, max_points=100000):
    """讀取二進位 Log，並解析 sfn, slot, payload"""
    data = []
    if not os.path.exists(filepath):
        print(f"找不到檔案: {filepath}")
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
                'payload': payload / 1000.0  # 轉換單位為 ms
            })
    return data

def align_pnf_to_vnf(vnf_data, pnf_data):
    """利用 (sfn % 1024, slot) 進行 Strict exact mapping"""
    pnf_dict = {}
    for d in pnf_data:
        key = (d['sfn'] % 1024, d['slot'])
        if key not in pnf_dict:
            pnf_dict[key] = d['payload']

    aligned_pnf = []
    mapped_count = 0
    for v in vnf_data:
        key = (v['sfn'] % 1024, v['slot'])
        if key in pnf_dict:
            aligned_pnf.append(pnf_dict[key])
            mapped_count += 1
        else:
            aligned_pnf.append(None)
            
    print(f"Mapping 完成: 成功對齊 {mapped_count}/{len(vnf_data)} 筆點位。")
    return aligned_pnf

def plot_data(vnf_y, pnf_y, x_indices, output_name, x_limit=None):
    """繪製原始時序圖：固定 Y 軸 -1~5，並標示 0 的 deadline"""
    fig, ax = plt.subplots(figsize=SQUARE_FIGSIZE)

    # 繪製 VNF
    ax.plot(x_indices, vnf_y, marker=VNF_MARKER, linestyle='-', color='#3498db', 
            linewidth=2, markersize=MARKER_SIZE, label='VNF ahead time', zorder=4)

    # 繪製 PNF (過濾 None 值避免繪製斷線)
    pnf_x_valid = [x for x, y in zip(x_indices, pnf_y) if y is not None]
    pnf_y_valid = [y for y in pnf_y if y is not None]
    ax.plot(pnf_x_valid, pnf_y_valid, marker=PNF_MARKER, linestyle='--', color='#9b59b6', 
            linewidth=2, markersize=MARKER_SIZE, label=r'$\Delta t_{\text{arrive}}$', zorder=3)

    # 設定 Y 軸範圍與 Deadline 基準線
    ax.set_ylim(-1, 5)
    ax.axhline(0, color=DEADLINE_COLOR, linewidth=2, linestyle='-', zorder=2)
    ax.annotate('deadline', xy=(x_indices[0] if x_indices else 0, 0), 
                xytext=(5, 5), textcoords='offset points', 
                color=DEADLINE_COLOR, fontweight='bold')

    # 設定 X 軸範圍 (如有自訂)
    if x_limit:
        ax.set_xlim(x_limit[0], x_limit[1])

    ax.set_xlabel('Index')
    ax.set_ylabel('Time (ms)')
    ax.grid(True, linestyle=':', alpha=0.7)
    
    ax.legend(loc='upper left')

    fig.tight_layout()
    plt.savefig(output_name, dpi=300, bbox_inches='tight')
    plt.close(fig)
    print(f"圖表已儲存: {output_name}")

def plot_cumulative_count(pnf_y, x_indices, output_name, x_limit=None):
    """繪製 PNF 超過 deadline (小於 0) 的數量累積圖"""
    fig, ax = plt.subplots(figsize=SQUARE_FIGSIZE)

    cumulative_counts = []
    running_count = 0
    for y in pnf_y:
        if y is not None and y < 0:
            running_count += 1
        cumulative_counts.append(running_count)

    ax.plot(x_indices, cumulative_counts, marker=PNF_MARKER, linestyle='-', color='#9b59b6', 
            linewidth=2, markersize=MARKER_SIZE, label='Cumulative ' + r'$\Delta t_{\text{arrive}} < 0$', zorder=3)
    ax.fill_between(x_indices, cumulative_counts, color='#9b59b6', alpha=0.10, zorder=2)

    # 設定 X 軸範圍 (如有自訂)
    if x_limit:
        ax.set_xlim(x_limit[0], x_limit[1])

    ax.set_ylim(bottom=0)
    ax.set_xlabel('Index')
    ax.set_ylabel('Cumulative count')
    ax.grid(True, linestyle=':', alpha=0.7)
    ax.legend(loc='upper left')

    fig.tight_layout()
    plt.savefig(output_name, dpi=300, bbox_inches='tight')
    plt.close(fig)
    print(f"累積圖表已儲存: {output_name}")

def main():
    parser = argparse.ArgumentParser(description='Simple VNF/PNF Mapping and Plotting')
    parser.add_argument('--vnf-file', required=True, type=str, help='VNF .bin file path')
    parser.add_argument('--pnf-file', required=True, type=str, help='PNF .bin file path')
    parser.add_argument('--start', type=int, default=0, help='Custom interval start index')
    parser.add_argument('--end', type=int, default=100, help='Custom interval end index')
    args = parser.parse_args()

    # 1. 讀取資料
    vnf_data = read_packed_log(args.vnf_file)
    pnf_data = read_packed_log(args.pnf_file)

    if not vnf_data:
        print("VNF 資料為空，結束程式。")
        return

    # 2. Mapping
    aligned_pnf = align_pnf_to_vnf(vnf_data, pnf_data)
    vnf_y = [d['payload'] for d in vnf_data]
    x_indices = list(range(len(vnf_y)))

    # 3. 畫出 mapping 後的所有 raw data
    plot_data(vnf_y, aligned_pnf, x_indices, 'raw_data_all.png')

    # 4. 畫出自訂區間的圖表與累積圖
    start_idx = max(0, args.start)
    end_idx = min(len(vnf_y) - 1, args.end)
    
    if start_idx < end_idx:
        custom_vnf = vnf_y[start_idx:end_idx + 1]
        custom_pnf = aligned_pnf[start_idx:end_idx + 1]
        custom_x = x_indices[start_idx:end_idx + 1]
        
        # 自訂區間時序圖
        plot_data(custom_vnf, custom_pnf, custom_x, 'raw_data_custom_interval.png', x_limit=(start_idx, end_idx))
        
        # 自訂區間的 PNF 負值累積圖
        plot_cumulative_count(custom_pnf, custom_x, 'raw_data_custom_interval_cumulative.png', x_limit=(start_idx, end_idx))
    else:
        print("自訂區間索引錯誤或範圍過小，無法繪製自訂區間。")

if __name__ == "__main__":
    main()