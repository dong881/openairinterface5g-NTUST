#!/bin/bash
#===============================================================================
# NFAPI Universal Start Script (Enhanced Version)
# 
# Usage: ./run_nfapi.sh <MODE> [AUTO_STOP]
#===============================================================================
# local: ~/oai_mp_f_ming/openairinterface5g
# local-orig: ~/oai_mp_f_ming_2025w44/openairinterface5g
# split: hpe:~/openairinterface5g
# split-orig: hpe:~/openairinterface5g-2025w44
#-------------------------------------------------------------------------------

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

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
    local component_name=${2:-"Component"}
    echo ""
    echo -e "${YELLOW}========================================${NC}"
    echo -e "${YELLOW}🔨 Building ${component_name}${NC}"
    echo -e "${YELLOW}📁 Build Path: $build_path${NC}"
    echo -e "${YELLOW}========================================${NC}"
    
    cd "$build_path" || { 
        echo -e "${RED}❌ ERROR: Cannot access build directory: $build_path${NC}"
        exit 1
    }
    
    # Build command
    local cmd="sudo ninja nr-softmodem nr-uesoftmodem dfts ldpc params_libconfig"
    echo -e "${YELLOW}CMD:${NC} $cmd"
    echo ""
    
    # Execute build and capture output in real-time
    if eval "$cmd" 2>&1; then
        echo ""
        echo -e "${GREEN}========================================${NC}"
        echo -e "${GREEN}✅ ${component_name} Build Successful${NC}"
        echo -e "${GREEN}========================================${NC}"
        echo ""
        return 0
    else
        echo ""
        echo -e "${RED}========================================${NC}"
        echo -e "${RED}❌ ${component_name} Build FAILED!${NC}"
        echo -e "${RED}🛑 Stopping execution.${NC}"
        echo -e "${RED}========================================${NC}"
        exit 1
    fi
}

#-------------------------------------------------------------------------------
# Function: Stop existing sessions
#-------------------------------------------------------------------------------
stop_sessions() {
    local sessions=("$@")
    echo -e "${YELLOW}🛑 Stopping existing sessions: ${sessions[*]}${NC}"
    for session in "${sessions[@]}"; do
        local cmd="screen -S \"$session\" -X quit 2>/dev/null"
        echo -e "${YELLOW}CMD:${NC} $cmd"
        eval "$cmd"
    done
}

#-------------------------------------------------------------------------------
# Function: Start VNF session locally (execution only, build separately)
#-------------------------------------------------------------------------------
start_vnf_local() {
    local build_path=$1
    local conf_file=$2
    local log_suffix=$3
    
    cd "$build_path" || { 
        echo -e "${RED}❌ ERROR: Cannot access build directory: $build_path${NC}"
        exit 1
    }
    
    local log_file="$LOG_DIR/nfapi-VNF-pegatron-localcn-${DATE_TAG}-${log_suffix}.log"
    
    # CLEANUP LOG to prevent auto-tester from reading old data
    if [ -f "$log_file" ]; then
        echo -e "${YELLOW}🧹 Removing old log: $log_file${NC}"
        rm -f "$log_file"
    fi
    
    echo -e "${GREEN}🚀 Starting VNF...${NC}"
    local cmd="screen -dmS VNF_SESSION bash -c \"sudo NFAPI_TRACE_LEVEL=info gdb -ex run --args ./nr-softmodem -q -O ${conf_file} --nfapi VNF 2>&1 | tee ${log_file}\""
    echo -e "${YELLOW}CMD:${NC} $cmd"
    eval "$cmd"
    
    echo "VNF Started in screen session 'VNF_SESSION'"
    echo "Log: $log_file"
}

#-------------------------------------------------------------------------------
# Function: Start PNF session locally (execution only, build separately)
#-------------------------------------------------------------------------------
start_pnf_local() {
    local build_path=$1
    local conf_file=$2
    local log_suffix=$3
    local is_split=${4:-0}
    
    cd "$build_path" || { 
        echo -e "${RED}❌ ERROR: Cannot access build directory: $build_path${NC}"
        exit 1
    }
    
    local log_file
    if [ "$is_split" -eq 1 ]; then
        log_file="$LOG_DIR/nfapi-PNF-Split-pegatron-localcn-${DATE_TAG}-${log_suffix}.log"
    else
        log_file="$LOG_DIR/nfapi-PNF-pegatron-localcn-${DATE_TAG}-${log_suffix}.log"
    fi
    
    # CLEANUP LOG to prevent auto-tester from reading old data
    if [ -f "$log_file" ]; then
        echo -e "${YELLOW}🧹 Removing old log: $log_file${NC}"
        rm -f "$log_file"
    fi
    
    echo -e "${GREEN}🚀 Starting PNF...${NC}"
    local cmd="screen -dmS PNF_SESSION bash -c \"sudo NFAPI_TRACE_LEVEL=info gdb -ex run --args ./nr-softmodem -O ${conf_file} --thread-pool ${THREAD_POOL} --nfapi PNF 2>&1 | tee ${log_file}\""
    echo -e "${YELLOW}CMD:${NC} $cmd"
    eval "$cmd"
    
    echo "PNF Started in screen session 'PNF_SESSION'"
    echo "Log: $log_file"
}

#-------------------------------------------------------------------------------
# Function: Build and Start VNF on HPE via SSH (Blocking build, show logs)
#-------------------------------------------------------------------------------
start_vnf_hpe() {
    local hpe_path=$1
    local is_orig=$2
    
    echo ""
    echo -e "${YELLOW}========================================${NC}"
    echo -e "${YELLOW}🌐 Building Remote VNF on HPE${NC}"
    echo -e "${YELLOW}========================================${NC}"
    
    # Determine paths and config based on original or not
    local build_path conf_file log_suffix
    if [ "$is_orig" -eq 1 ]; then
        build_path="~/${hpe_path}/cmake_targets/ran_build/build"
        conf_file="../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-vnf-split.sa.band78.273prb.nfapi-bmw.conf"
        log_suffix="orig-develop"
    else
        build_path="~/${hpe_path}/cmake_targets/ran_build/build"
        conf_file="../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-vnf-split.sa.band78.273prb.nfapi-bmw.conf"
        log_suffix="ming-develop"
    fi
    
    # Step 1: Build on HPE (BLOCKING - show all build output)
    echo -e "${YELLOW}📁 Remote Build Path: $build_path${NC}"
    echo -e "${YELLOW}CMD:${NC} ssh hpe 'cd $build_path && sudo ninja nr-softmodem nr-uesoftmodem dfts ldpc params_libconfig'"
    echo ""
    
    if ssh hpe "cd $build_path && sudo ninja nr-softmodem nr-uesoftmodem dfts ldpc params_libconfig" 2>&1; then
        echo ""
        echo -e "${GREEN}========================================${NC}"
        echo -e "${GREEN}✅ Remote VNF Build Successful${NC}"
        echo -e "${GREEN}========================================${NC}"
        echo ""
    else
        echo ""
        echo -e "${RED}========================================${NC}"
        echo -e "${RED}❌ Remote VNF Build FAILED!${NC}"
        echo -e "${RED}🛑 Aborting execution.${NC}"
        echo -e "${RED}========================================${NC}"
        exit 1
    fi
    
    # Step 2: Stop existing VNF session on HPE
    echo -e "${YELLOW}🛑 Stopping existing VNF session on HPE...${NC}"
    ssh hpe "screen -S VNF_SESSION -X quit 2>/dev/null" || true
    
    # Step 3: Start VNF on HPE
    echo -e "${GREEN}🚀 Starting Remote VNF on HPE...${NC}"
    local log_file="~/gNB-logs/nfapi-VNF-pegatron-localcn-${DATE_TAG}-${log_suffix}.log"
    
    # Clean old log
    ssh hpe "rm -f $log_file" || true
    
    # Start VNF in screen session
    local start_cmd="screen -dmS VNF_SESSION bash -c 'cd $build_path && sudo NFAPI_TRACE_LEVEL=info gdb -ex run --args ./nr-softmodem -q -O ${conf_file} --nfapi VNF 2>&1 | tee ${log_file}'"
    echo -e "${YELLOW}CMD:${NC} ssh hpe \"$start_cmd\""
    
    if ssh hpe "$start_cmd"; then
        echo -e "${GREEN}✅ Remote VNF started in screen session 'VNF_SESSION'${NC}"
        echo -e "${GREEN}📄 Remote Log: $log_file${NC}"
    else
        echo -e "${RED}❌ Failed to start remote VNF${NC}"
        exit 1
    fi
}

#-------------------------------------------------------------------------------
# Function: Wait for local VNF to be ready
#-------------------------------------------------------------------------------
wait_for_vnf_ready_local() {
    local timeout=${1:-60}
    local elapsed=0
    echo -e "${YELLOW}⏳ Waiting for local VNF to be ready (timeout ${timeout}s)...${NC}"
    while [ $elapsed -lt $timeout ]; do
        if screen -list | grep -q "VNF_SESSION"; then
            if pgrep -f "nr-softmodem.*--nfapi VNF" >/dev/null 2>&1; then
                echo -e "${GREEN}✅ Local VNF is running.${NC}"
                return 0
            fi
        fi
        sleep 1
        elapsed=$((elapsed+1))
    done
    echo -e "${RED}❌ Timeout waiting for local VNF.${NC}"
    exit 1
}

#-------------------------------------------------------------------------------
# Function: Wait for remote (HPE) VNF to be ready
#-------------------------------------------------------------------------------
wait_for_vnf_ready_remote() {
    local remote_mode=$1
    local timeout=${2:-60}
    local elapsed=0
    echo -e "${YELLOW}⏳ Waiting for remote VNF (${remote_mode}) to be ready (timeout ${timeout}s)...${NC}"
    while [ $elapsed -lt $timeout ]; do
        if ssh hpe "pgrep -f 'nr-softmodem.*--nfapi VNF' >/dev/null 2>&1 || screen -list | grep -q VNF_SESSION" >/dev/null 2>&1; then
            echo -e "${GREEN}✅ Remote VNF is running.${NC}"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed+1))
    done
    echo -e "${RED}❌ Timeout waiting for remote VNF.${NC}"
    exit 1
}

#-------------------------------------------------------------------------------
# Function: Handle auto-stop
#-------------------------------------------------------------------------------
handle_auto_stop() {
    local stop_mode=$1
    if [ "$AUTO_STOP" -eq 1 ]; then
        echo "Auto-stop enabled. Will stop after 120 seconds..."
        sleep 120
        local cmd="$(dirname "$0")/stop_nfapi.sh \"$stop_mode\""
        echo -e "${YELLOW}CMD:${NC} $cmd"
        eval "$cmd"
        echo "Auto-stop completed."
    fi
}

#-------------------------------------------------------------------------------
# Main execution
#-------------------------------------------------------------------------------
echo "========================================"
echo -e "NFAPI Start Script | Mode: ${CYAN}$MODE${NC} | Auto-stop: ${CYAN}$AUTO_STOP${NC}"
echo "========================================"

case "$MODE" in
    #---------------------------------------------------------------------------
    # Local mode: Build VNF/PNF locally, then run both
    #---------------------------------------------------------------------------
    local)
        stop_sessions VNF_SESSION PNF_SESSION
        BUILD_PATH="${HOME}/${PATH_MING}/cmake_targets/ran_build/build"
        
        # Step 1: Build (show logs)
        compile_project "$BUILD_PATH" "Local VNF/PNF"
        
        # Step 2: Start VNF
        start_vnf_local "$BUILD_PATH" "$CONF_VNF" "ming-develop"
        wait_for_vnf_ready_local 60
        
        # Step 3: Start PNF
        start_pnf_local "$BUILD_PATH" "$CONF_PNF" "f-ming-develop" 0
        handle_auto_stop "local"
        ;;
    
    #---------------------------------------------------------------------------
    # Local original mode: Build VNF/PNF locally (original path), then run both
    #---------------------------------------------------------------------------
    local-orig)
        stop_sessions VNF_SESSION PNF_SESSION
        BUILD_PATH="${HOME}/${PATH_ORIG}/cmake_targets/ran_build/build"
        
        # Step 1: Build (show logs)
        compile_project "$BUILD_PATH" "Local VNF/PNF (Original)"
        
        # Step 2: Start VNF
        start_vnf_local "$BUILD_PATH" "$CONF_VNF" "orig-develop"
        wait_for_vnf_ready_local 60
        
        # Step 3: Start PNF
        start_pnf_local "$BUILD_PATH" "$CONF_PNF" "f-orig-develop" 0
        handle_auto_stop "local"
        ;;
    
    #---------------------------------------------------------------------------
    # Split mode: Build remote VNF on HPE, build local PNF, then run both
    #---------------------------------------------------------------------------
    split)
        stop_sessions PNF_SESSION
        
        # Step 1: Build and start remote VNF on HPE (blocking build with logs)
        start_vnf_hpe "$PATH_HPE" 0
        wait_for_vnf_ready_remote "split" 120
        
        # Step 2: Build local PNF (show logs)
        BUILD_PATH="${HOME}/${PATH_MING}/cmake_targets/ran_build/build"
        compile_project "$BUILD_PATH" "Local PNF"
        
        # Step 3: Start local PNF
        start_pnf_local "$BUILD_PATH" "$CONF_PNF_SPLIT" "f-ming-develop" 1
        handle_auto_stop "split"
        ;;
    
    #---------------------------------------------------------------------------
    # Split original mode: Build remote VNF on HPE (orig), build local PNF (orig), then run both
    #---------------------------------------------------------------------------
    split-orig)
        stop_sessions PNF_SESSION
        
        # Step 1: Build and start remote VNF on HPE (blocking build with logs)
        start_vnf_hpe "$PATH_HPE_ORIG" 1
        wait_for_vnf_ready_remote "split-orig" 120
        
        # Step 2: Build local PNF (show logs)
        BUILD_PATH="${HOME}/${PATH_ORIG}/cmake_targets/ran_build/build"
        compile_project "$BUILD_PATH" "Local PNF (Original)"
        
        # Step 3: Start local PNF
        start_pnf_local "$BUILD_PATH" "$CONF_PNF_SPLIT" "f-orig-develop" 1
        handle_auto_stop "split"
        ;;
    
    #---------------------------------------------------------------------------
    # VNF only modes: Build then run VNF only
    #---------------------------------------------------------------------------
    vnf)
        stop_sessions VNF_SESSION
        BUILD_PATH="${HOME}/${PATH_MING}/cmake_targets/ran_build/build"
        
        # Build then start
        compile_project "$BUILD_PATH" "Local VNF"
        start_vnf_local "$BUILD_PATH" "$CONF_VNF" "ming-develop"
        handle_auto_stop "vnf"
        ;;
    vnf-orig)
        stop_sessions VNF_SESSION
        BUILD_PATH="${HOME}/${PATH_ORIG}/cmake_targets/ran_build/build"
        
        # Build then start
        compile_project "$BUILD_PATH" "Local VNF (Original)"
        start_vnf_local "$BUILD_PATH" "$CONF_VNF" "orig-develop"
        handle_auto_stop "vnf"
        ;;
    
    #---------------------------------------------------------------------------
    # PNF only modes: Build then run PNF only
    #---------------------------------------------------------------------------
    pnf)
        stop_sessions PNF_SESSION
        BUILD_PATH="${HOME}/${PATH_MING}/cmake_targets/ran_build/build"
        
        # Build then start
        compile_project "$BUILD_PATH" "Local PNF"
        start_pnf_local "$BUILD_PATH" "$CONF_PNF" "f-ming-develop" 0
        handle_auto_stop "pnf"
        ;;
    pnf-orig)
        stop_sessions PNF_SESSION
        BUILD_PATH="${HOME}/${PATH_ORIG}/cmake_targets/ran_build/build"
        
        # Build then start
        compile_project "$BUILD_PATH" "Local PNF (Original)"
        start_pnf_local "$BUILD_PATH" "$CONF_PNF" "f-orig-develop" 0
        handle_auto_stop "pnf"
        ;;
    
    #---------------------------------------------------------------------------
    # Invalid mode
    #---------------------------------------------------------------------------
    *)
        echo -e "${RED}ERROR: Invalid mode '$MODE'${NC}"
        echo ""
        echo "Run './run_nfapi.sh help' for usage information."
        exit 1
        ;;
esac

echo "========================================"
echo -e "${GREEN}Done!${NC}"
echo "========================================"

exit 0