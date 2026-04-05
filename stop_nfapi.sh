#!/bin/bash
#===============================================================================
# NFAPI Universal Stop Script
# 
# Usage: ./stop_nfapi_all.sh <MODE>
#===============================================================================

#-------------------------------------------------------------------------------
# Function: Show help message
#-------------------------------------------------------------------------------
show_help() {
    echo "==============================================================================="
    echo "NFAPI Universal Stop Script"
    echo "==============================================================================="
    echo "Usage: ./stop_nfapi_all.sh <MODE>"
    echo ""
    echo "MODES:"
    echo "  local / local-orig           - Stop VNF + PNF locally"
    echo "  split / split-orig           - Stop VNF on HPE + PNF locally"
    echo "  vnf / vnf-orig               - Stop VNF only locally"
    echo "  pnf / pnf-orig               - Stop PNF only locally"
    echo "  hpe-vnf                      - Stop VNF on HPE server"
    echo "  monolithic / monolithic-orig - Stop gNB monolithic session"
    echo "  rfsim / rfsim-orig           - Stop PNF + UE + Test tools locally"
    echo "  all                          - Stop all sessions (default)"
    echo "  help                         - Show this help message"
    echo "==============================================================================="
    exit 0
}

MODE=${1:-all}

if [ "$MODE" = "help" ] || [ "$MODE" = "-h" ] || [ "$MODE" = "--help" ]; then
    show_help
fi

#-------------------------------------------------------------------------------
# Function: Gracefully stop a screen session
#-------------------------------------------------------------------------------
stop_session_graceful() {
    local session=$1
    if screen -list | grep -q "$session"; then
        echo "Stopping session: $session"
        screen -S "$session" -X stuff $'\003\n' # Ctrl+C
        sleep 1
        screen -S "$session" -X quit 2>/dev/null
    fi
}

#-------------------------------------------------------------------------------
# Function: Clean up Test Tools (Iperf/Ping)
#-------------------------------------------------------------------------------
clean_test_tools() {
    echo "Cleaning up test tools (Iperf/Ping)..."
    # 強制關閉測試用的 screen session
    screen -S iperf_cn_server -X quit 2>/dev/null
    screen -S iperf_cn_client -X quit 2>/dev/null
    screen -S ping_cn_ue -X quit 2>/dev/null
    screen -S IPERF_SERVER -X quit 2>/dev/null # 新增對應 start 腳本 rfsim 的 session
    
    # 確保殘留 process 被清除
    pkill -f iperf3 2>/dev/null
    pkill -f ping 2>/dev/null
}

#-------------------------------------------------------------------------------
# Helper Functions
#-------------------------------------------------------------------------------
stop_vnf_local() { stop_session_graceful VNF_SESSION; }
stop_pnf_local() { stop_session_graceful PNF_SESSION; }
stop_gnb_local() { stop_session_graceful GNB_SESSION; }
stop_ue_local()  { stop_session_graceful UE_SESSION; } # 新增 UE 停止
stop_vnf_hpe() {
    echo "Stopping VNF on HPE server..."
    # 這裡假設 hpe 是 ssh config 中設定好的 host alias
    ssh hpe "screen -S VNF_SESSION -X stuff $'\003\n'; sleep 1; screen -S VNF_SESSION -X quit" 2>/dev/null
}

#-------------------------------------------------------------------------------
# Main execution
#-------------------------------------------------------------------------------
echo "========================================"
echo "NFAPI Stop Script (Mode: $MODE)"
echo "========================================"

# 無論哪種模式，都先清理測試工具
clean_test_tools

case "$MODE" in
    local|local-orig)
        stop_vnf_local
        stop_pnf_local
        ;;
    split|split-orig)
        stop_vnf_hpe
        stop_pnf_local
        ;;
    vnf|vnf-orig)
        stop_vnf_local
        ;;
    pnf|pnf-orig)
        stop_pnf_local
        ;;
    hpe-vnf)
        stop_vnf_hpe # 只停 HPE 上的
        ;;
    monolithic|monolithic-orig)
        stop_gnb_local
        ;;
    rfsim|rfsim-orig)
        stop_pnf_local
        stop_ue_local
        ;;
    all)
        stop_vnf_hpe
        stop_vnf_local
        stop_pnf_local
        stop_gnb_local
        stop_ue_local
        ;;
    *)
        echo "ERROR: Invalid mode '$MODE'"
        exit 1
        ;;
esac

echo "========================================"
echo "Done."
echo "========================================"
exit 0