#!/bin/bash

AUTO_STOP=${1:-0}  # Default: do not auto-stop. Pass parameter 1 to enable auto-stop functionality

screen -S VNF_SESSION -X quit
screen -S PNF_SESSION -X quit

cd ~/oai_mp_f_ming/openairinterface5g/cmake_targets/ran_build/build || exit 1
sudo ninja nr-softmodem nr-uesoftmodem dfts ldpc params_libconfig

mkdir -p ~/gNB-logs

screen -dmS VNF_SESSION bash -c "sudo NFAPI_TRACE_LEVEL=info gdb -ex run --args ./nr-softmodem -q -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-vnf.sa.band78.273prb.nfapi-bmw.conf --nfapi VNF 2>&1 | tee ~/gNB-logs/nfapi-VNF-pegatron-localcn-2025.w44-ming-develop.log"

screen -dmS PNF_SESSION bash -c "sudo NFAPI_TRACE_LEVEL=info gdb -ex run --args ./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-pnf.sa.band78.fhi72.nfapi.4x4-pegatron.conf --thread-pool 1,3,5,7,9,11,13,15 --nfapi PNF 2>&1 | tee ~/gNB-logs/nfapi-PNF-pegatron-localcn-2025.w44-f-ming-develop.log"

echo "VNF and PNF Started in screen sessions 'VNF_SESSION' and 'PNF_SESSION'"

if [ "$AUTO_STOP" -eq 1 ]; then
    # Wait 120 seconds before stopping
    sleep 120

    # Send Ctrl+C to interrupt gdb
    for session in VNF_SESSION PNF_SESSION; do
        screen -S $session -X stuff $'\003'
    done
    sleep 2
    
    # Print backtrace
    for session in VNF_SESSION PNF_SESSION; do
        screen -S $session -X stuff "bt\n"
    done
    sleep 1
    
    # Quit gdb
    for session in VNF_SESSION PNF_SESSION; do
        screen -S $session -X stuff "q\n"
    done
    sleep 1
    
    # Confirm quit
    for session in VNF_SESSION PNF_SESSION; do
        screen -S $session -X stuff "y\n"
    done

    sleep 2
    screen -S VNF_SESSION -X quit
    screen -S PNF_SESSION -X quit
    
    echo "Auto-stop completed for VNF and PNF sessions"
else
    echo "Auto-stop is disabled. Sessions will continue running."
fi

exit 0
