#!/bin/bash

AUTO_STOP=${1:-0}  # Default to not auto-stop; passing 1 enables the auto-stop feature

# Clear old VNF session
screen -S VNF_SESSION -X quit

# Compile
cd ~/openairinterface5g/cmake_targets/ran_build/build || exit 1
sudo ninja nr-softmodem nr-uesoftmodem dfts ldpc params_libconfig

# Ensure log directory exists
mkdir -p ~/gNB-logs

# Start VNF paging (including GDB and log recording)
screen -dmS VNF_SESSION bash -c "sudo NFAPI_TRACE_LEVEL=info gdb -ex run --args ./nr-softmodem -q -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-vnf-split.sa.band78.273prb.nfapi-bmw.conf --nfapi VNF 2>&1 | tee ~/gNB-logs/nfapi-VNF-pegatron-localcn-2025.w44-ming-develop.log"

echo "VNF Started in screen session 'VNF_SESSION'"

if [ "$AUTO_STOP" -eq 1 ]; then
    # Wait time (originally set to 120 seconds)
    sleep 120

    # Send Ctrl+C (interrupt signal) to VNF
    screen -S VNF_SESSION -X stuff $'\003'
    sleep 2

    # Send "bt" (Backtrace) command to GDB
    screen -S VNF_SESSION -X stuff "bt\n"
    sleep 1

    # Send "q" (Quit) command to GDB
    screen -S VNF_SESSION -X stuff "q\n"
    sleep 1

    # Confirm exit from GDB (respond with y)
    screen -S VNF_SESSION -X stuff "y\n"

    # Clean up session
    sleep 2
    screen -S VNF_SESSION -X quit
else
    echo "Auto-stop is disabled. The session will continue running."
fi

exit 0
