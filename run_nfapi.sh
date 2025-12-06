#!/bin/bash
#===============================================================================
# NFAPI Universal Start Script
# 
# Usage: ./run_nfapi.sh <MODE> [AUTO_STOP]
#
# MODE:
#   local         - Run VNF + PNF locally (oai_mp_f_ming path)
#   local-orig    - Run VNF + PNF locally (oai_mp_f_ming_2025w44 - original path)
#   split         - Run VNF on HPE + PNF locally (oai_mp_f_ming path)
#   split-orig    - Run VNF on HPE + PNF locally (oai_mp_f_ming_2025w44 - original path)
#   vnf           - Run VNF only locally (oai_mp_f_ming path)
#   vnf-orig      - Run VNF only locally (oai_mp_f_ming_2025w44 - original path)
#   pnf           - Run PNF only locally (oai_mp_f_ming path)
#   pnf-orig      - Run PNF only locally (oai_mp_f_ming_2025w44 - original path)
#   hpe-vnf       - Run VNF on HPE server (called via SSH from split mode)
#   hpe-vnf-orig  - Run VNF on HPE server original (called via SSH from split-orig mode)
#   help          - Show this help message
#
# AUTO_STOP: 0 (default) or 1 (auto-stop after 120 seconds)
#===============================================================================

#-------------------------------------------------------------------------------
# Function: Show help message
#-------------------------------------------------------------------------------
show_help() {
    echo "==============================================================================="
    echo "NFAPI Universal Start Script"
    echo "==============================================================================="
    echo ""
    echo "Usage: ./run_nfapi.sh <MODE> [AUTO_STOP]"
    echo ""
    echo "MODES:"
    echo "  local         - Run VNF + PNF locally (oai_mp_f_ming path)"
    echo "  local-orig    - Run VNF + PNF locally (oai_mp_f_ming_2025w44 - original path)"
    echo "  split         - Run VNF on HPE + PNF locally (oai_mp_f_ming path)"
    echo "  split-orig    - Run VNF on HPE + PNF locally (oai_mp_f_ming_2025w44 - original path)"
    echo "  vnf           - Run VNF only locally (oai_mp_f_ming path)"
    echo "  vnf-orig      - Run VNF only locally (oai_mp_f_ming_2025w44 - original path)"
    echo "  pnf           - Run PNF only locally (oai_mp_f_ming path)"
    echo "  pnf-orig      - Run PNF only locally (oai_mp_f_ming_2025w44 - original path)"
    echo "  hpe-vnf       - Run VNF on HPE server (internal use for split mode)"
    echo "  hpe-vnf-orig  - Run VNF on HPE server original (internal use for split-orig)"
    echo "  help          - Show this help message"
    echo ""
    echo "OPTIONS:"
    echo "  AUTO_STOP     - 0 (default): No auto-stop"
    echo "                  1: Auto-stop after 120 seconds"
    echo ""
    echo "LOG FILES:"
    echo "  PNF Log:       \$HOME/gNB-logs/nfapi-PNF-pegatron-localcn-2025.w44-f-ming-develop.log"
    echo "  PNF Split Log: \$HOME/gNB-logs/nfapi-PNF-Split-pegatron-localcn-2025.w44-f-ming-develop.log"
    echo "  VNF Log:       \$HOME/gNB-logs/nfapi-VNF-pegatron-localcn-2025.w44-ming-develop.log"
    echo "  Measure Log:   \$HOME/oai_mp_f_ming/openairinterface5g/cmake_targets/ran_build/build/measure.txt"
    echo ""
    echo "EXAMPLES:"
    echo "  ./run_nfapi.sh local          # Run VNF + PNF locally"
    echo "  ./run_nfapi.sh split 1        # Run split mode with auto-stop"
    echo "  ./run_nfapi.sh local-orig     # Run original version locally"
    echo ""
    echo "ENVIRONMENT VARIABLES (for convenience):"
    echo "  export MY_PNF_LOG=\"\$HOME/gNB-logs/nfapi-PNF-pegatron-localcn-2025.w44-f-ming-develop.log\""
    echo "  export MY_PNF_SPLIT_LOG=\"\$HOME/gNB-logs/nfapi-PNF-Split-pegatron-localcn-2025.w44-f-ming-develop.log\""
    echo "  export MY_VNF_LOG=\"\$HOME/gNB-logs/nfapi-VNF-pegatron-localcn-2025.w44-ming-develop.log\""
    echo "  export MY_MEASURE_LOG=\"\$HOME/oai_mp_f_ming/openairinterface5g/cmake_targets/ran_build/build/measure.txt\""
    echo "==============================================================================="
    exit 0
}

MODE=${1:-local}
AUTO_STOP=${2:-0}

# Check for help mode first
if [ "$MODE" = "help" ] || [ "$MODE" = "-h" ] || [ "$MODE" = "--help" ]; then
    show_help
fi

# Path configurations
PATH_MING="oai_mp_f_ming/openairinterface5g"
PATH_ORIG="oai_mp_f_ming_2025w44/openairinterface5g"
PATH_HPE="openairinterface5g"
PATH_HPE_ORIG="openairinterface5g-2025w44"

# Configuration file paths (relative to build directory)
CONF_VNF="../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-vnf.sa.band78.273prb.nfapi-bmw.conf"
CONF_VNF_SPLIT="../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-vnf-split.sa.band78.273prb.nfapi-bmw.conf"
CONF_PNF="../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-pnf.sa.band78.fhi72.nfapi.4x4-pegatron.conf"
CONF_PNF_SPLIT="../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-pnf-split.sa.band78.fhi72.nfapi.4x4-pegatron.conf"

# Thread pool settings
THREAD_POOL="1,3,5,7,9,11,13,15"

# Log directory
LOG_DIR=~/gNB-logs
mkdir -p "$LOG_DIR"

# Fixed date tag for log filename
DATE_TAG="2025.w44"

#-------------------------------------------------------------------------------
# Function: Compile the project
#-------------------------------------------------------------------------------
compile_project() {
    local build_path=$1
    echo "Compiling in: $build_path"
    cd "$build_path" || { echo "ERROR: Cannot access build directory: $build_path"; exit 1; }
    sudo ninja nr-softmodem nr-uesoftmodem dfts ldpc params_libconfig
}

#-------------------------------------------------------------------------------
# Function: Stop existing sessions
#-------------------------------------------------------------------------------
stop_sessions() {
    local sessions=("$@")
    for session in "${sessions[@]}"; do
        screen -S "$session" -X quit 2>/dev/null
    done
}

#-------------------------------------------------------------------------------
# Function: Start VNF session locally
#-------------------------------------------------------------------------------
start_vnf_local() {
    local build_path=$1
    local conf_file=$2
    local log_suffix=$3
    
    compile_project "$build_path"
    
    local log_file="$LOG_DIR/nfapi-VNF-pegatron-localcn-${DATE_TAG}-${log_suffix}.log"
    
    screen -dmS VNF_SESSION bash -c "sudo NFAPI_TRACE_LEVEL=info gdb -ex run --args ./nr-softmodem -q -O ${conf_file} --nfapi VNF 2>&1 | tee ${log_file}"
    
    echo "VNF Started in screen session 'VNF_SESSION'"
    echo "Log: $log_file"
}

#-------------------------------------------------------------------------------
# Function: Start PNF session locally
#-------------------------------------------------------------------------------
start_pnf_local() {
    local build_path=$1
    local conf_file=$2
    local log_suffix=$3
    local is_split=${4:-0}
    
    compile_project "$build_path"
    
    local log_file
    if [ "$is_split" -eq 1 ]; then
        log_file="$LOG_DIR/nfapi-PNF-Split-pegatron-localcn-${DATE_TAG}-${log_suffix}.log"
    else
        log_file="$LOG_DIR/nfapi-PNF-pegatron-localcn-${DATE_TAG}-${log_suffix}.log"
    fi
    
    screen -dmS PNF_SESSION bash -c "sudo NFAPI_TRACE_LEVEL=info gdb -ex run --args ./nr-softmodem -O ${conf_file} --thread-pool ${THREAD_POOL} --nfapi PNF 2>&1 | tee ${log_file}"
    
    echo "PNF Started in screen session 'PNF_SESSION'"
    echo "Log: $log_file"
}

#-------------------------------------------------------------------------------
# Function: Start VNF on HPE via SSH
#-------------------------------------------------------------------------------
start_vnf_hpe() {
    local hpe_script_path=$1
    echo "Starting VNF on HPE server..."
    ssh hpe "~/${hpe_script_path}/run_nfapi.sh hpe-vnf"
}

#-------------------------------------------------------------------------------
# Function: Handle auto-stop
#-------------------------------------------------------------------------------
handle_auto_stop() {
    local stop_mode=$1
    if [ "$AUTO_STOP" -eq 1 ]; then
        echo "Auto-stop enabled. Will stop after 120 seconds..."
        sleep 120
        "$(dirname "$0")/stop_nfapi.sh" "$stop_mode"
        echo "Auto-stop completed."
    fi
}

#-------------------------------------------------------------------------------
# Main execution
#-------------------------------------------------------------------------------
echo "========================================"
echo "NFAPI Start Script"
echo "Mode: $MODE"
echo "Auto-stop: $AUTO_STOP"
echo "========================================"

case "$MODE" in
    #---------------------------------------------------------------------------
    # Local mode: VNF + PNF on local machine (ming path)
    #---------------------------------------------------------------------------
    local)
        stop_sessions VNF_SESSION PNF_SESSION
        BUILD_PATH=~/${PATH_MING}/cmake_targets/ran_build/build
        start_vnf_local "$BUILD_PATH" "$CONF_VNF" "ming-develop"
        start_pnf_local "$BUILD_PATH" "$CONF_PNF" "f-ming-develop" 0
        handle_auto_stop "local"
        ;;
    
    #---------------------------------------------------------------------------
    # Local original mode: VNF + PNF on local machine (2025w44 path)
    #---------------------------------------------------------------------------
    local-orig)
        stop_sessions VNF_SESSION PNF_SESSION
        BUILD_PATH=~/${PATH_ORIG}/cmake_targets/ran_build/build
        start_vnf_local "$BUILD_PATH" "$CONF_VNF" "orig-develop"
        start_pnf_local "$BUILD_PATH" "$CONF_PNF" "f-orig-develop" 0
        handle_auto_stop "local"
        ;;
    
    #---------------------------------------------------------------------------
    # Split mode: VNF on HPE + PNF locally (ming path)
    #---------------------------------------------------------------------------
    split)
        stop_sessions PNF_SESSION
        start_vnf_hpe "$PATH_HPE"
        BUILD_PATH=~/${PATH_MING}/cmake_targets/ran_build/build
        start_pnf_local "$BUILD_PATH" "$CONF_PNF_SPLIT" "f-ming-develop" 1
        handle_auto_stop "split"
        ;;
    
    #---------------------------------------------------------------------------
    # Split original mode: VNF on HPE + PNF locally (2025w44 path)
    #---------------------------------------------------------------------------
    split-orig)
        stop_sessions PNF_SESSION
        start_vnf_hpe "$PATH_HPE_ORIG"
        BUILD_PATH=~/${PATH_ORIG}/cmake_targets/ran_build/build
        start_pnf_local "$BUILD_PATH" "$CONF_PNF_SPLIT" "f-orig-develop" 1
        handle_auto_stop "split"
        ;;
    
    #---------------------------------------------------------------------------
    # VNF only locally (ming path)
    #---------------------------------------------------------------------------
    vnf)
        stop_sessions VNF_SESSION
        BUILD_PATH=~/${PATH_MING}/cmake_targets/ran_build/build
        start_vnf_local "$BUILD_PATH" "$CONF_VNF" "ming-develop"
        handle_auto_stop "vnf"
        ;;
    
    #---------------------------------------------------------------------------
    # VNF only locally original (2025w44 path)
    #---------------------------------------------------------------------------
    vnf-orig)
        stop_sessions VNF_SESSION
        BUILD_PATH=~/${PATH_ORIG}/cmake_targets/ran_build/build
        start_vnf_local "$BUILD_PATH" "$CONF_VNF" "orig-develop"
        handle_auto_stop "vnf"
        ;;
    
    #---------------------------------------------------------------------------
    # PNF only locally (ming path)
    #---------------------------------------------------------------------------
    pnf)
        stop_sessions PNF_SESSION
        BUILD_PATH=~/${PATH_MING}/cmake_targets/ran_build/build
        start_pnf_local "$BUILD_PATH" "$CONF_PNF" "f-ming-develop" 0
        handle_auto_stop "pnf"
        ;;
    
    #---------------------------------------------------------------------------
    # PNF only locally original (2025w44 path)
    #---------------------------------------------------------------------------
    pnf-orig)
        stop_sessions PNF_SESSION
        BUILD_PATH=~/${PATH_ORIG}/cmake_targets/ran_build/build
        start_pnf_local "$BUILD_PATH" "$CONF_PNF" "f-orig-develop" 0
        handle_auto_stop "pnf"
        ;;
    
    #---------------------------------------------------------------------------
    # HPE VNF mode (called via SSH from split mode) - ming path
    #---------------------------------------------------------------------------
    hpe-vnf)
        stop_sessions VNF_SESSION
        BUILD_PATH=~/${PATH_HPE}/cmake_targets/ran_build/build
        start_vnf_local "$BUILD_PATH" "$CONF_VNF_SPLIT" "ming-develop"
        ;;
    
    #---------------------------------------------------------------------------
    # HPE VNF original mode (called via SSH from split-orig mode)
    #---------------------------------------------------------------------------
    hpe-vnf-orig)
        stop_sessions VNF_SESSION
        BUILD_PATH=~/${PATH_HPE_ORIG}/cmake_targets/ran_build/build
        start_vnf_local "$BUILD_PATH" "$CONF_VNF_SPLIT" "orig-develop"
        ;;
    
    #---------------------------------------------------------------------------
    # Invalid mode
    #---------------------------------------------------------------------------
    *)
        echo "ERROR: Invalid mode '$MODE'"
        echo ""
        echo "Run './run_nfapi.sh help' for usage information."
        exit 1
        ;;
esac

echo "========================================"
echo "Done!"
echo "========================================"

exit 0
