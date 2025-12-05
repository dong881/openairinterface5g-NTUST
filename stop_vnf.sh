#!/bin/bash

# Close VNF: Automatically inject gdb commands
# screen -S VNF_SESSION -X stuff $'\003\n'
# sleep 2
# screen -S VNF_SESSION -X stuff "bt\n"
# sleep 1
# screen -S VNF_SESSION -X stuff "q\n"
# sleep 1
# screen -S VNF_SESSION -X stuff "y\n"

# Wait for cleanup
# sleep 2
screen -S VNF_SESSION -X quit