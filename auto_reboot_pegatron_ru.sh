#!/bin/bash

# 設定參數
TARGET_IP="192.168.9.9"
USER="padmin"
PASS="Pega@2025"
NMAP_CMD="nmap -p 1-65535 -sS $TARGET_IP"

echo "=== 開始執行 RU 自動重啟流程 ==="

while true; do
    echo "[Step 1] 嘗試 SSH 連線至 $TARGET_IP..."
    
    # 使用 expect 處理 SSH 互動
    # 返回值: 0=成功重啟, 1=需要 Trigger (Nmap), 2=其他錯誤
    expect -c "
        set timeout 10
        spawn ssh $USER@$TARGET_IP
        
        expect {
            # 如果匹配到 Banner 中的特殊區塊符號，代表狀態正確
            \"██\" {
                expect \"password:\"
                send \"$PASS\r\"
                expect \"#\"
                send \"reboot\r\"
                puts \"\n>> Reboot command sent successfully.\"
                expect eof
                exit 0
            }
            # 如果在看到 Banner 之前直接出現密碼提示，代表狀態錯誤
            \"$USER@$TARGET_IP's password:\" {
                puts \"\n>> Error: No Banner detected. Host needs triggering.\"
                exit 1
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
    
    # 獲取 expect 的退出代碼
    RET_VAL=$?

    if [ $RET_VAL -eq 0 ]; then
        echo "[Success] 重啟指令已發送。"
        break
    elif [ $RET_VAL -eq 1 ]; then
        echo "[Trigger Needed] 偵測到 Host 未觸發，執行 Nmap..."
        # 這裡執行 sudo nmap，可能會要求輸入本地 sudo 密碼
        echo ">> Running: sudo $NMAP_CMD"
        sudo $NMAP_CMD
        echo ">> 等待 2 秒後重試..."
        sleep 2
    else
        echo "[Error] 連線發生預期外的錯誤，5秒後重試..."
        sleep 5
    fi
done

echo "------------------------------------------------"
echo "[Step 2] 等待 RU 重啟完成 (Ping Monitoring)..."

# 簡單的等待邏輯：先等它斷線(通常reboot指令後就斷了)，再等它上線
# 為了保險，先睡 10 秒讓它完全關機
sleep 10

echo ">> 開始 Ping 偵測，等待回應..."
while ! ping -c 1 -W 1 $TARGET_IP &> /dev/null; do
    printf "."
    sleep 1
done

echo ""
echo "[Online] $TARGET_IP 已恢復連線！"
echo "------------------------------------------------"
echo "[Step 3] 執行 alias 'pegam'..."

# 因為 pegam 是 alias，普通 script 讀不到
# 我們呼叫 zsh 的互動模式 (-i) 來執行它
# 請確保您的 .zshrc 中有定義 pegam
zsh -i -c "pegam"
