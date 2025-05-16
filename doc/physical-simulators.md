This document describes the physical simulators, also called "unitary
simulators", or short "physims".

[[_TOC_]]

# Introduction

There are several unitary simulations for the physical layer, testing
individual L1 transport/control channels using Monte-Carlo simulations, for
instance:

- `nr_pucchsim` for 5G PUCCH,
- `nr_dlsim` for 5G DLSCH/PDSCH,
- `nr_prachsim` for 5G PRACH,
- `dlsim` for 4G DLSCH/PDSCH, etc.

These simulators constitute the starting point for testing any new code in the
PHY layer, and are required tests to be performed before committing any new
code to the repository, in the sense that all of these should compile and
execute correctly. They provide estimates of the transport block error rates
and HARQ thoughput, DCI error rates, etc. The simulators reside in
`openair1/SIMULATION/LTE_PHY` and `openair1/SIMULATION/NR_PHY`. For instance,
`nr_dlsim` is built from `openair1/SIMULATION/NR_PHY/dlsim.c`.

Further unitary simulation of the coding subsystem components also exists in
`openair1/PHY/CODING/TESTBENCH`, e.g., `ldpctest` for encoding/decoding of
LDPC.

These are the simulators known to work properly and tested in CI:
- 4G: `dlsim`, `ulsim`
- 5G: `nr_dlsim`, `nr_ulsim`, `nr_dlschsim`, `nr_ulschsim`, `ldpctest`,
  `nr_pbchsim`, `nr_prachsim`, `nr_pucchsim`, `polartest`, `smallblocktest`,
  `nr_psbchsim` (sidelink)

These tests are run in this pipeline:
[RAN-PhySim-Cluster](https://jenkins-oai.eurecom.fr/job/RAN-PhySim-Cluster/).
