#!/bin/bash

AUTO_STOP=${1:-0}  # Default to not auto-stop; passing 1 enables the auto-stop feature

# Clear old PNF session
screen -S PNF_SESSION -X quit

# Compile (keep the path unchanged)
cd ~/oai_mp_f_ming/openairinterface5g/cmake_targets/ran_build/build || exit 1
sudo ninja nr-softmodem nr-uesoftmodem dfts ldpc params_libconfig

# Ensure log directory exists
mkdir -p ~/gNB-logs

# Start PNF paging (including GDB and log recording)
# Using Pegatron RU configuration and thread pool settings
screen -dmS PNF_SESSION bash -c "sudo NFAPI_TRACE_LEVEL=info gdb -ex run --args ./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-pnf-split.sa.band78.fhi72.nfapi.4x4-pegatron.conf --thread-pool 1,3,5,7,9,11,13,15 --nfapi PNF 2>&1 | tee ~/gNB-logs/nfapi-PNF-Split-pegatron-localcn-2025.w44-f-ming-develop.log"

echo "PNF Started in screen session 'PNF_SESSION'"

if [ "$AUTO_STOP" -eq 1 ]; then
    # Wait time (originally set to 120 seconds)
    sleep 120

    # Send Ctrl+C (interrupt signal) to PNF
    screen -S PNF_SESSION -X stuff $'\003'
    sleep 2

    # Send "bt" (Backtrace) command to GDB
    screen -S PNF_SESSION -X stuff "bt\n"
    sleep 1

    # Send "q" (Quit) command to GDB
    screen -S PNF_SESSION -X stuff "q\n"
    sleep 1

    # Confirm exit from GDB (respond with y)
    screen -S PNF_SESSION -X stuff "y\n"

    # Clean up session
    sleep 2
    screen -S PNF_SESSION -X quit
else
    echo "Auto-stop is disabled. The session will continue running."
fi

exit 0