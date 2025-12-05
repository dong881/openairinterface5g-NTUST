#!/bin/bash

AUTO_STOP=${1:-0}  # 預設不自動停止，帶入參數 1 表示啟用自動停止功能

# 清除舊的 VNF session
screen -S VNF_SESSION -X quit

# 編譯
cd ~/openairinterface5g/cmake_targets/ran_build/build || exit 1
sudo ninja nr-softmodem nr-uesoftmodem dfts ldpc params_libconfig

# 確保 Log 資料夾存在
mkdir -p ~/gNB-logs

# 啟動 VNF 分頁 (包含 GDB 與 Log 紀錄)
screen -dmS VNF_SESSION bash -c "sudo NFAPI_TRACE_LEVEL=info gdb -ex run --args ./nr-softmodem -q -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-vnf-split.sa.band78.273prb.nfapi-bmw.conf --nfapi VNF 2>&1 | tee ~/gNB-logs/nfapi-VNF-pegatron-localcn-2025.w44-ming-develop.log"

echo "VNF Started in screen session 'VNF_SESSION'"

if [ "$AUTO_STOP" -eq 1 ]; then
    # 等待時間 (原本設定 120 秒)
    sleep 120

    # 傳送 Ctrl+C (中斷訊號) 給 VNF
    screen -S VNF_SESSION -X stuff $'\003'
    sleep 2

    # 傳送 "bt" (Backtrace) 指令給 GDB
    screen -S VNF_SESSION -X stuff "bt\n"
    sleep 1

    # 傳送 "q" (Quit) 指令給 GDB
    screen -S VNF_SESSION -X stuff "q\n"
    sleep 1

    # 確認離開 GDB (回答 y)
    screen -S VNF_SESSION -X stuff "y\n"

    # 清理 session
    sleep 2
    screen -S VNF_SESSION -X quit
else
    echo "Auto-stop is disabled. The session will continue running."
fi

exit 0
