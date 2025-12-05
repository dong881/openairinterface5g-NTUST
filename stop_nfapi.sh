#!/bin/bash
ssh hpe "~/openairinterface5g/stop_vnf.sh"

# Close: automatically inject gdb commands
for session in VNF_SESSION PNF_SESSION; do
    screen -S $session -X stuff $'\003\n'
done
# sleep 2
# for session in VNF_SESSION PNF_SESSION; do
#     screen -S $session -X stuff "bt\n"
# done
# sleep 1
# for session in VNF_SESSION PNF_SESSION; do
#     screen -S $session -X stuff "q\n"
# done
# sleep 1
# for session in VNF_SESSION PNF_SESSION; do
#     screen -S $session -X stuff "y\n"
# done

# Wait a few seconds for cleanup
sleep 2
screen -S VNF_SESSION -X quit
screen -S PNF_SESSION -X quit

# # Close VNF: automatically inject gdb commands
# screen -S VNF_SESSION -X stuff $'\003\n'
# sleep 2
# screen -S VNF_SESSION -X stuff "bt\n"
# sleep 1
# screen -S VNF_SESSION -X stuff "q\n"
# sleep 1
# screen -S VNF_SESSION -X stuff "y\n"

# # Wait for cleanup
# sleep 2
# screen -S VNF_SESSION -X quit

# # Close PNF: automatically inject gdb commands
# screen -S PNF_SESSION -X stuff $'\003\n'
# sleep 2
# screen -S PNF_SESSION -X stuff "bt\n"
# sleep 1
# screen -S PNF_SESSION -X stuff "q\n"
# sleep 1
# screen -S PNF_SESSION -X stuff "y\n"

# # Wait for cleanup
# sleep 2
# screen -S PNF_SESSION -X quit
