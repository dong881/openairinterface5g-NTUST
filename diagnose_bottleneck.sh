#!/bin/bash
# OAI 5G gNB Performance Diagnostic Script
# Usage: Run while gNB is active with UE connected and iperf running

echo "=== OAI 5G gNB Performance Diagnostics ==="
echo "Timestamp: $(date)"
echo ""

# 1. Check gNB process
echo "--- gNB Process Status ---"
pgrep -a nr-softmodem && echo "✓ gNB running" || echo "✗ gNB NOT running"
echo ""

# 2. Check O-RAN fronthaul interface
echo "--- O-RAN Fronthaul Interface ---"
FHI_IF=$(ip link | grep -B1 "00:11:22:33:44" | head -1 | awk '{print $2}' | tr -d ':')
if [ -n "$FHI_IF" ]; then
    echo "Interface: $FHI_IF"
    ethtool -S $FHI_IF | grep -E "rx_packets|tx_packets|rx_dropped|tx_dropped|rx_errors|tx_errors" | head -6
else
    echo "✗ O-RAN interface not found"
fi
echo ""

# 3. Check ACC100 LDPC accelerator
echo "--- ACC100 LDPC Status ---"
lspci -s 87:00.0 | grep -q "Intel" && echo "✓ ACC100 detected" || echo "✗ ACC100 NOT detected"
ls /dev/vfio/* 2>/dev/null | wc -l | xargs echo "VFIO devices:"
echo ""

# 4. Check CPU utilization for PHY threads
echo "--- PHY Thread CPU Usage (cores 6-14) ---"
for core in 6 7 8 9 10 11 13 14; do
    cpu_usage=$(top -bn1 | grep "Cpu${core}" | awk '{print $2}' | cut -d'%' -f1)
    printf "Core %2d: %5s%%\n" $core "$cpu_usage"
done
echo ""

# 5. Check DPDK memory
echo "--- DPDK Hugepages ---"
grep Huge /proc/meminfo | grep -E "HugePages_Total|HugePages_Free"
echo ""

# 6. Recent gNB errors (last 20 lines)
echo "--- Recent gNB Errors ---"
journalctl -u nr-softmodem -n 20 --no-pager | grep -E "ERROR|WARN|deadline" | tail -10
echo ""

# 7. Iperf server check
echo "--- Iperf Server Status ---"
pgrep -a iperf3 && echo "✓ iperf3 server running" || echo "✗ iperf3 server NOT running"
echo ""

# 8. Network throughput snapshot
echo "--- Network Throughput (last 5 seconds) ---"
if command -v nload &> /dev/null; then
    timeout 5 nload -t 1000 | head -20
else
    echo "(Install 'nload' for real-time throughput monitoring)"
fi
echo ""

# 9. Configuration check
echo "--- Configuration Check ---"
CONFIG="/home/kelvin/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.273prb.fhi72.4x4-liteon.conf"
if [ -f "$CONFIG" ]; then
    echo "do_CSIRS: $(grep 'do_CSIRS' $CONFIG | grep -v '#')"
    echo "csi_rs_periodicity: $(grep 'csi_rs_periodicity' $CONFIG | grep -v '#')"
else
    echo "Config file not found at expected location"
fi
echo ""

echo "=== Diagnostic Complete ==="
echo ""
echo "Next steps:"
echo "1. Watch gNB console - statistics appear automatically every few seconds"
echo "2. Look for these key metrics in console output:"
echo "   - phy_procedures_gNB_TX: should be < 500 us"
echo "   - nr_csirs_scheduling: should be < 100 us (if do_CSIRS=1)"
echo "   - Check for 'Late TX' or 'Late RX' deadline misses"
echo "3. Compare throughput with do_CSIRS=0 vs do_CSIRS=1"
echo "4. Run T-tracer for detailed MAC PDU analysis"