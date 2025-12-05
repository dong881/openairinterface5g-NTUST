#!/bin/bash

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
screen -S VNF_SESSION -X quit