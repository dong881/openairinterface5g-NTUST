#!/bin/bash
#===============================================================================
# NFAPI Universal Stop Script
# 
# Usage: ./stop_nfapi_all.sh <MODE>
#
# MODE:
#   local       - Stop VNF + PNF locally
#   split       - Stop VNF on HPE + PNF locally
#   vnf         - Stop VNF only locally
#   pnf         - Stop PNF only locally
#   hpe-vnf     - Stop VNF on HPE server (called via SSH from split mode)
#   all         - Stop all sessions (VNF + PNF locally + VNF on HPE)
#   help        - Show this help message
#
# Default: all (stops everything)
#===============================================================================

#-------------------------------------------------------------------------------
# Function: Show help message
#-------------------------------------------------------------------------------
show_help() {
    echo "==============================================================================="
    echo "NFAPI Universal Stop Script"
    echo "==============================================================================="
    echo ""
    echo "Usage: ./stop_nfapi_all.sh <MODE>"
    echo ""
    echo "MODES:"
    echo "  local    - Stop VNF + PNF locally"
    echo "  split    - Stop VNF on HPE + PNF locally"
    echo "  vnf      - Stop VNF only locally"
    echo "  pnf      - Stop PNF only locally"
    echo "  hpe-vnf  - Stop VNF on HPE server"
    echo "  all      - Stop all sessions (default)"
    echo "  help     - Show this help message"
    echo ""
    echo "EXAMPLES:"
    echo "  ./stop_nfapi_all.sh          # Stop all sessions (default)"
    echo "  ./stop_nfapi_all.sh local    # Stop local VNF + PNF"
    echo "  ./stop_nfapi_all.sh split    # Stop split mode (HPE VNF + local PNF)"
    echo "  ./stop_nfapi_all.sh pnf      # Stop PNF only"
    echo "==============================================================================="
    exit 0
}

MODE=${1:-all}

# Check for help mode first
if [ "$MODE" = "help" ] || [ "$MODE" = "-h" ] || [ "$MODE" = "--help" ]; then
    show_help
fi

#-------------------------------------------------------------------------------
# Function: Gracefully stop a screen session with GDB
#-------------------------------------------------------------------------------
stop_session_graceful() {
    local session=$1
    echo "Stopping session: $session"
    
    # Check if session exists
    if ! screen -list | grep -q "$session"; then
        echo "  Session $session not found, skipping..."
        return 0
    fi
    
    # Send Ctrl+C to interrupt
    screen -S "$session" -X stuff $'\003\n'
    sleep 1
    
    # Get backtrace from GDB
    screen -S "$session" -X stuff "bt\n"
    sleep 1
    
    # Quit GDB
    screen -S "$session" -X stuff "q\n"
    sleep 1
    
    # Confirm quit
    screen -S "$session" -X stuff "y\n"
    sleep 1
    
    # Force quit the screen session
    screen -S "$session" -X quit 2>/dev/null
    
    echo "  Session $session stopped."
}

#-------------------------------------------------------------------------------
# Function: Quick stop (just kill the session)
#-------------------------------------------------------------------------------
stop_session_quick() {
    local session=$1
    echo "Quick stopping session: $session"
    screen -S "$session" -X quit 2>/dev/null
}

#-------------------------------------------------------------------------------
# Function: Stop VNF locally
#-------------------------------------------------------------------------------
stop_vnf_local() {
    stop_session_graceful VNF_SESSION
}

#-------------------------------------------------------------------------------
# Function: Stop PNF locally
#-------------------------------------------------------------------------------
stop_pnf_local() {
    stop_session_graceful PNF_SESSION
}

#-------------------------------------------------------------------------------
# Function: Stop VNF on HPE via SSH
#-------------------------------------------------------------------------------
stop_vnf_hpe() {
    echo "Stopping VNF on HPE server..."
    ssh hpe "screen -S VNF_SESSION -X stuff $'\003\n'; sleep 1; screen -S VNF_SESSION -X stuff 'bt\n'; sleep 1; screen -S VNF_SESSION -X stuff 'q\n'; sleep 1; screen -S VNF_SESSION -X stuff 'y\n'; sleep 1; screen -S VNF_SESSION -X quit"
    echo "VNF on HPE stopped."
}

#-------------------------------------------------------------------------------
# Main execution
#-------------------------------------------------------------------------------
echo "========================================"
echo "NFAPI Stop Script"
echo "Mode: $MODE"
echo "========================================"

case "$MODE" in
    #---------------------------------------------------------------------------
    # Local mode: Stop VNF + PNF locally
    #---------------------------------------------------------------------------
    local)
        stop_vnf_local
        stop_pnf_local
        ;;
    
    #---------------------------------------------------------------------------
    # Split mode: Stop VNF on HPE + PNF locally
    #---------------------------------------------------------------------------
    split)
        stop_vnf_hpe
        stop_pnf_local
        ;;
    
    #---------------------------------------------------------------------------
    # VNF only locally
    #---------------------------------------------------------------------------
    vnf)
        stop_vnf_local
        ;;
    
    #---------------------------------------------------------------------------
    # PNF only locally
    #---------------------------------------------------------------------------
    pnf)
        stop_pnf_local
        ;;
    
    #---------------------------------------------------------------------------
    # HPE VNF mode (called via SSH or directly on HPE)
    #---------------------------------------------------------------------------
    hpe-vnf)
        stop_vnf_local
        ;;
    
    #---------------------------------------------------------------------------
    # All: Stop everything
    #---------------------------------------------------------------------------
    all)
        # Try to stop VNF on HPE first (ignore errors if not running)
        stop_vnf_hpe 2>/dev/null || true
        # Stop local sessions
        stop_vnf_local
        stop_pnf_local
        ;;
    
    #---------------------------------------------------------------------------
    # Invalid mode
    #---------------------------------------------------------------------------
    *)
        echo "ERROR: Invalid mode '$MODE'"
        echo ""
        echo "Run './stop_nfapi_all.sh help' for usage information."
        exit 1
        ;;
esac

echo "========================================"
echo "All specified sessions stopped!"
echo "========================================"

exit 0
