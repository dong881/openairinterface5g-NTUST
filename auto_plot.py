#!/usr/bin/env python3
import os
import sys
import subprocess
import shutil
import glob
import re

# ================= 設定區 (Configuration) =================
REMOTE_HOST = "hpe"
IMG_REMOTE_DEST = "mingwsl:~/scpData"

# 來源檔案路徑
SRC_VNF_PATH = "gNB-logs/nfapi-VNF-pegatron-localcn-2025.w44-ming-develop.log"
SRC_PNF_PATH = os.path.expanduser("~/gNB-logs/nfapi-PNF-Split-pegatron-localcn-2025.w44-f-ming-develop.log")
SRC_MEASURE_PATH = os.path.expanduser("~/oai_mp_f_ming/openairinterface5g/cmake_targets/ran_build/build/measure.txt")

# Sync Log 來源 (VNF or PNF)
SYNC_LOG_SOURCE = "VNF" 

# 目標目錄
BASE_DEBUG_DIR = os.path.expanduser("~/nfapi-debugger")
RAW_DATA_DIR = os.path.join(BASE_DEBUG_DIR, "raw_data")
OVERVIEW_DIR = os.path.join(BASE_DEBUG_DIR, "overview")
FIGURE_DIR = os.path.join(BASE_DEBUG_DIR, "figure")
SCRIPT_SOURCE_DIR = os.path.join(BASE_DEBUG_DIR, "overview")
# =========================================================

class Colors:
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    RESET = '\033[0m'
    CYAN = '\033[96m'

def get_latest_plot_script(custom_script=None):
    """
    取得版本號最大的繪圖腳本 (模糊搜尋)
    搜尋規則: 檔名包含 'plot' 且包含 'v數字'
    """
    if custom_script:
        if os.path.exists(custom_script): return os.path.abspath(custom_script)
        cand = os.path.join(SCRIPT_SOURCE_DIR, custom_script)
        if os.path.exists(cand): return cand
        print(f"{Colors.YELLOW}⚠️  找不到指定腳本 {custom_script}，轉為自動搜尋...{Colors.RESET}")

    # 1. 抓取目錄下所有 .py
    all_py_files = glob.glob(os.path.join(SCRIPT_SOURCE_DIR, "*.py"))
    
    if not all_py_files:
        raise FileNotFoundError(f"在 {SCRIPT_SOURCE_DIR} 找不到任何 .py 檔案")

    candidates = []
    
    # 2. Regex 模糊匹配: plot開頭 ... v(數字) ...
    # 例如: plot_sync_margin_v5.py, plot_sync_margin_raw_log_v4.py 都會匹配
    pattern = re.compile(r'plot.*v(\d+).*\.py$', re.IGNORECASE)

    for f in all_py_files:
        filename = os.path.basename(f)
        match = pattern.search(filename)
        if match:
            version = int(match.group(1))
            candidates.append((version, f, filename))

    if not candidates:
        # 如果模糊搜尋找不到，就列出所有 py 檔讓使用者看
        print(f"{Colors.RED}❌ 找不到符合 'plot...v[數字]' 格式的腳本。{Colors.RESET}")
        print(f"目錄內容: {[os.path.basename(f) for f in all_py_files]}")
        sys.exit(1)

    # 3. 排序 (版本號大 -> 小)
    candidates.sort(key=lambda x: x[0], reverse=True)
    
    best_version, best_path, best_name = candidates[0]
    
    # 顯示搜尋結果 (方便 Debug)
    print(f"{Colors.CYAN}🔍 腳本搜尋結果 (Top 3):{Colors.RESET}")
    for ver, path, name in candidates[:3]:
        print(f"   - [v{ver}] {name}")
    
    print(f"📜 選定腳本: {Colors.GREEN}{best_name}{Colors.RESET}")
    return best_path

def parse_args():
    args = sys.argv[1:]
    if not args:
        print(f"Usage: python3 {sys.argv[0]} [Tag Suffixes...] [t/u] [bw] [optional: script.py]")
        sys.exit(1)

    custom_script = None
    if args[-1].endswith('.py'):
        custom_script = args[-1]
        args = args[:-1]

    proto_idx = -1
    proto_val = 'tcp'
    mapping = {'t': 'tcp', 'u': 'udp', 'tcp': 'tcp', 'udp': 'udp'}

    for i, arg in enumerate(args):
        if arg.lower() in mapping:
            proto_idx = i
            proto_val = mapping[arg.lower()]
            break
    
    if proto_idx == -1:
        print(f"{Colors.RED}❌ 錯誤: 找不到 Protocol 參數 (t, u, tcp, udp){Colors.RESET}")
        sys.exit(1)

    tags = args[:proto_idx]
    bw_raw = args[proto_idx+1]
    file_prefix = "-".join(tags)

    bws = []
    if len(bw_raw) > 4 and bw_raw.startswith('d') and bw_raw[1:].isdigit():
        bw_map = {'1': 'd100', '2': 'd200', '3': 'd300', '5': 'd500', '7': 'd700'}
        for c in bw_raw[1:]:
            if c in bw_map: bws.append(bw_map[c])
        if not bws: bws = [bw_raw]
    else:
        bws = [bw_raw]

    return file_prefix, proto_val, bws, custom_script

def run_workflow(tag_prefix, proto, bw, script_path):
    full_suffix = f"{tag_prefix}-iperf-{proto}-{bw}"
    print(f"\n{Colors.GREEN}🚀 開始任務: {bw} ({proto}) | Suffix: {full_suffix}{Colors.RESET}")

    vnf_file = f"vnf-pegatron-localcn-develop-latest-{full_suffix}.log"
    pnf_file = f"pnf-pegatron-localcn-develop-latest-{full_suffix}.log"
    measure_file = f"m-{full_suffix}.txt"

    local_vnf_path = os.path.join(RAW_DATA_DIR, vnf_file)
    local_pnf_path = os.path.join(RAW_DATA_DIR, pnf_file)
    local_measure_path = os.path.join(RAW_DATA_DIR, measure_file)

    for d in [RAW_DATA_DIR, OVERVIEW_DIR, FIGURE_DIR]:
        os.makedirs(d, exist_ok=True)

    print(f"   📥 下載 VNF Log ({REMOTE_HOST})...")
    subprocess.run(["scp", f"{REMOTE_HOST}:~/{SRC_VNF_PATH}", local_vnf_path], check=True)

    print(f"   📥 複製 PNF Log...")
    if os.path.exists(SRC_PNF_PATH):
        shutil.copy(SRC_PNF_PATH, local_pnf_path)
    else:
        # 這裡雖然是警告，但如果不複製，變數就不存在，下面可能會報錯。
        # 這裡做一個空檔案或是略過
        pass

    print(f"   📥 複製 Measure Log...")
    shutil.copy(SRC_MEASURE_PATH, local_measure_path)

    script_name = os.path.basename(script_path)
    target_script = os.path.join(OVERVIEW_DIR, script_name)

    # [Fix] 檢查來源路徑與目標路徑是否相同，不同才複製
    if os.path.abspath(script_path) != os.path.abspath(target_script):
        shutil.copy(script_path, target_script)
    else:
        # 僅顯示除錯訊息 (可選)
        # print(f"   ℹ️ 繪圖腳本已在目標目錄，跳過複製。")
        pass

    rel_vnf = os.path.join("../raw_data", vnf_file)
    rel_pnf = os.path.join("../raw_data", pnf_file)
    rel_measure = os.path.join("../raw_data", measure_file)

    arg_sync = rel_vnf if SYNC_LOG_SOURCE == "VNF" else rel_pnf
    arg_margin = rel_measure

    cwd_original = os.getcwd()
    try:
        os.chdir(OVERVIEW_DIR)
        
        # 顯示指令
        full_cmd = f"python3 {script_name} {arg_sync} {arg_margin}"
        print(f"      {Colors.YELLOW}👉 Cmd: {full_cmd}{Colors.RESET}")
        
        # 執行 Python 腳本
        subprocess.run(full_cmd, shell=True, check=True)

        # 圖片上傳區段
        print(f"   📤 上傳圖片到 {IMG_REMOTE_DEST}...")
        
        # 策略 1: 預測檔名 (通常腳本會用 arg1 的檔名 + 后綴)
        # 例如: vnf-....log_full_analysis.png
        possible_names = [
            f"{vnf_file}_full_analysis.png",
            f"{os.path.basename(arg_sync)}_full_analysis.png", # 如果 arg_sync 是 pnf
            f"analysis_result.png" # 有些腳本可能是固定檔名
        ]

        found_img = None
        # 掃描 FIGURE_DIR 和 OVERVIEW_DIR
        for name in possible_names:
            for folder in [FIGURE_DIR, OVERVIEW_DIR]:
                path = os.path.join(folder, name)
                if os.path.exists(path):
                    found_img = path
                    break
            if found_img: break
        
        # 如果策略 1 失敗，策略 2: 找此資料夾最新產生的 png
        if not found_img:
            print(f"      {Colors.YELLOW}⚠️  預測檔名失敗，嘗試搜尋最新產生的 PNG...{Colors.RESET}")
            pngs = glob.glob(os.path.join(FIGURE_DIR, "*.png")) + glob.glob(os.path.join(OVERVIEW_DIR, "*.png"))
            if pngs:
                # 找修改時間最新的
                latest_png = max(pngs, key=os.path.getmtime)
                # 簡單防呆：修改時間要在最近 1 分鐘內
                import time
                if time.time() - os.path.getmtime(latest_png) < 60:
                    found_img = latest_png
                    print(f"      💡 找到最新的圖片: {os.path.basename(found_img)}")

        if found_img:
            subprocess.run(["scp", found_img, IMG_REMOTE_DEST], check=True)
            print(f"      {Colors.GREEN}✅ 圖片上傳成功！{Colors.RESET}")
        else:
            print(f"      {Colors.RED}❌ 找不到產生的圖片，無法上傳。{Colors.RESET}")

    except subprocess.CalledProcessError:
        print(f"      {Colors.RED}❌ 繪圖腳本執行失敗 (Error Code)。{Colors.RESET}")
    finally:
        os.chdir(cwd_original)

def main():
    tag_prefix, proto, bws, custom_script = parse_args()
    script_path = get_latest_plot_script(custom_script)
    
    for bw in bws:
        run_workflow(tag_prefix, proto, bw, script_path)

if __name__ == "__main__":
    main()
