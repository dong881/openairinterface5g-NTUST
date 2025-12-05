#!/bin/bash
# 清除舊的 PNF session
screen -S PNF_SESSION -X quit

# 編譯 (路徑維持不變)
cd ~/oai_mp_f_ming/openairinterface5g/cmake_targets/ran_build/build || exit 1
sudo ninja nr-softmodem nr-uesoftmodem dfts ldpc params_libconfig

# 確保 Log 資料夾存在
mkdir -p ~/gNB-logs

# 啟動 PNF 分頁 (包含 GDB 與 Log 紀錄)
# 使用 Pegatron RU 配置與 Thread Pool 設定
screen -dmS PNF_SESSION bash -c "sudo NFAPI_TRACE_LEVEL=info gdb -ex run --args ./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-pnf-split.sa.band78.fhi72.nfapi.4x4-pegatron.conf --thread-pool 1,3,5,7,9,11,13,15 --nfapi PNF 2>&1 | tee ~/gNB-logs/nfapi-PNF-Split-pegatron-localcn-2025.w44-f-ming-develop.log"

echo "PNF Started in screen session 'PNF_SESSION'"

exit 0

# ==========================================
# 下方為自動停止並抓取 GDB Backtrace 的邏輯
# 若需使用，請將上方的 'exit 0' 註解掉
# ==========================================

# 等待時間 (原本設定 120 秒)
sleep 120

# 傳送 Ctrl+C (中斷訊號) 給 PNF
screen -S PNF_SESSION -X stuff $'\003'
sleep 2

# 傳送 "bt" (Backtrace) 指令給 GDB
screen -S PNF_SESSION -X stuff "bt\n"
sleep 1

# 傳送 "q" (Quit) 指令給 GDB
screen -S PNF_SESSION -X stuff "q\n"
sleep 1

# 確認離開 GDB (回答 y)
screen -S PNF_SESSION -X stuff "y\n"

# 清理 session
sleep 2
screen -S PNF_SESSION -X quit