# #!/bin/bash
ssh hpe "~/openairinterface5g/stop_pnf.sh"

# 關閉：自動注入 gdb 指令
for session in VNF_SESSION PNF_SESSION; do
    screen -S $session -X stuff $'\003\n'
done
sleep 2
for session in VNF_SESSION PNF_SESSION; do
    screen -S $session -X stuff "bt\n"
done
sleep 1
for session in VNF_SESSION PNF_SESSION; do
    screen -S $session -X stuff "q\n"
done
sleep 1
for session in VNF_SESSION PNF_SESSION; do
    screen -S $session -X stuff "y\n"
done

# 可再等個幾秒進行清理
sleep 2
screen -S VNF_SESSION -X quit
screen -S PNF_SESSION -X quit



# # 關閉 VNF：自動注入 gdb 指令
# screen -S VNF_SESSION -X stuff $'\003\n'
# sleep 2
# screen -S VNF_SESSION -X stuff "bt\n"
# sleep 1
# screen -S VNF_SESSION -X stuff "q\n"
# sleep 1
# screen -S VNF_SESSION -X stuff "y\n"

# # 等待清理
# sleep 2
# screen -S VNF_SESSION -X quit

#!/bin/bash

# # 關閉 PNF：自動注入 gdb 指令
# screen -S PNF_SESSION -X stuff $'\003\n'
# sleep 2
# screen -S PNF_SESSION -X stuff "bt\n"
# sleep 1
# screen -S PNF_SESSION -X stuff "q\n"
# sleep 1
# screen -S PNF_SESSION -X stuff "y\n"

# # 等待清理
# sleep 2
# screen -S PNF_SESSION -X quit
