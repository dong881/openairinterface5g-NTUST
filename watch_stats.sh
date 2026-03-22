#!/bin/bash
# Watch OAI gNB Statistics in Real-Time
# Usage: Run this AFTER starting gNB with -q flag

STATS_FILE="cmake_targets/ran_build/build/nrL1_stats.log"

if [ ! -f "$STATS_FILE" ]; then
    echo "ERROR: Statistics file not found at $STATS_FILE"
    echo ""
    echo "Make sure you started nr-softmodem with the -q flag:"
    echo "  sudo ./nr-softmodem -O <config> -q ..."
    echo ""
    exit 1
fi

echo "=== OAI gNB Real-Time Statistics ==="
echo "Press Ctrl+C to exit"
echo ""
echo "Key metrics to watch:"
echo "  - L1 Tx processing: Total PHY TX time (should be < 500 us)"
echo "  - DLSCH encoding: LDPC + rate matching (should be < 200 us with ACC100)"
echo ""
echo "Comparing with/without do_CSIRS=1:"
echo "  - Look for 30-50% increase in L1 Tx processing time"
echo "  - Enable MAC debug logs to see nr_csirs_scheduling time"
echo ""
echo "=========================================="
echo ""

# Watch with color highlighting for important metrics
watch -n1 -c "tail -50 $STATS_FILE | grep --color=always -E 'L1 Tx processing|DLSCH encoding|$'"