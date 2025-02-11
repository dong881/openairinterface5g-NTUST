# Overview

This implements a shared memory realtime and near-realtime radio interface.
This is performed using `shm_td_iq_channel` which handles the shared memory
interface. 

# Limitations

 - Only 1gNB to 1nrUE
 - Only 1rx and 1tx antennas
 - No channel modelling

# Usage

On the UE and gNB: use `device.name shm_radio` command line argument.

Additionally on gNB use `shm_radio.role server` and optionally
`shm_radio.timescale <timescale>` to set the timescale. Timescale 1.0
is the default and means realtime.
