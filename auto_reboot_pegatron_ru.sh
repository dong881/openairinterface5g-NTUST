#!/bin/bash

# Configuration parameters
TARGET_IP="192.168.109.9"
USER="padmin"
PASS="Pega@2025"
NMAP_CMD="nmap -p 1-65535 -sS $TARGET_IP"

echo "=== Starting RU Auto-Reboot Process ==="

while true; do
    echo "[Step 1] Attempting SSH connection to $TARGET_IP..."
    
    # Use expect to handle SSH interaction
    # Return values: 0=reboot success, 1=trigger needed (Nmap), 2=other error
    expect -c "
        set timeout 10
        spawn ssh $USER@$TARGET_IP
        
        expect {
            # Check length of the buffer before password prompt. Long buffer = banner is present.
            \"$USER@$TARGET_IP's password:\" {
                if { [string length \$expect_out(buffer)] > 100 } {
                    send \"$PASS\r\"
                    expect \"#\"
                    send \"reboot\r\"
                    puts \"\n>> Reboot command sent successfully.\"
                    expect eof
                    exit 0
                } else {
                    puts \"\n>> Error: No Banner detected. Host needs triggering.\"
                    exit 1
                }
            }
            timeout {
                puts \"\n>> SSH Timeout.\"
                exit 2
            }
            eof {
                puts \"\n>> Connection closed unexpectedly.\"
                exit 2
            }
        }
    "
    
    # Get expect exit code
    RET_VAL=$?

    if [ $RET_VAL -eq 0 ]; then
        echo "[Success] Reboot command has been sent."
        break
    elif [ $RET_VAL -eq 1 ]; then
        echo "[Trigger Needed] Host not triggered, running Nmap..."
        echo ">> Running: sudo $NMAP_CMD"
        sudo $NMAP_CMD
        echo ">> Waiting 2 seconds before retry..."
        sleep 2
    else
        echo "[Error] Unexpected connection error, retrying in 5 seconds..."
        sleep 5
    fi
done

echo "------------------------------------------------"
echo "[Step 2] Waiting for RU reboot to complete (Ping Monitoring)..."

sleep 10

echo ">> Starting ping detection, waiting for response..."
while ! ping -c 1 -W 1 $TARGET_IP &> /dev/null; do
    printf "."
    sleep 1
done

echo ""
echo "[Online] $TARGET_IP is back online!"
echo "------------------------------------------------"
echo "[Step 3] Running NETCONF configuration (will retry until success)..."

sleep 30

PEGAM_SCRIPT="$HOME/SMO-Mplane/Pegatron/Mplane_pega.sh"
MAX_RETRIES=30
RETRY_INTERVAL=10

for ((i=1; i<=MAX_RETRIES; i++)); do
    echo ">> Attempting NETCONF configuration (attempt $i/$MAX_RETRIES)..."
    
    # Run the expect script natively
    if $PEGAM_SCRIPT; then
        echo ""
        echo "=========================================="
        echo "[Success] NETCONF configuration completed!"
        echo "=========================================="
        break
    else
        echo ">> [Failed] NETCONF configuration failed, waiting ${RETRY_INTERVAL} seconds before retry..."
        sleep $RETRY_INTERVAL
    fi
    
    if [ $i -eq $MAX_RETRIES ]; then
        echo ""
        echo "=========================================="
        echo "[Failed] Maximum retries reached ($MAX_RETRIES)"
        echo "Please manually check RU status and run: $PEGAM_SCRIPT"
        echo "=========================================="
        exit 1
    fi
done

echo ""
echo "=== RU Auto-Reboot Process Completed ==="