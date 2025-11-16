# Copilot Instructions

## Repository Big Picture
- `openair1/`, `openair2/`, `openair3/` host PHY/L2/L3 stacks; `executables/` wraps them into softmodems (eNB/gNB/UE) and CU components.
- `nfapi/` contains the (n)FAPI bridge implementing SCF-225 and SCF-222 specifications; timing-sensitive work (e.g., `nfapi/oai_integration` and `nfapi/open-nFAPI`) must keep slot accounting consistent with MAC code under `openair2/NR_MAC`.[1][2]
- Shared build utilities live under `cmake_targets/`; CI Orchestration and Dockerized test harnesses are in `ci-scripts/` plus `docker/`.
- Run-time configs and test vectors are spread across `targets/RT/CONF`, `ci-scripts/xml_files/`, and `tests/` (NR CU-UP, RF simulator, etc.); reuse these instead of hand-coding JSON/YAML.


## Build & Dependency Workflow
- Always source `oaienv` before invoking tooling so paths (e.g., `OPENAIR_DIR`) are exported.
- Preferred build entrypoint is `cmake_targets/build_oai` which wraps cmake/ninja and logs actual commands under `cmake_targets/log`: 
  ```bash
  cd cmake_targets
  sudo ./build_oai --gNB --ninja -t oran_fhlib_5g --cmake-opt -Dxran_LOCATION=/home/oai72_su/oai_mp_f_ming/phy/fhi_lib/lib
  ```
- One-off cmake builds belong in throwaway `build/` folders (see `doc/BUILD.md` for target list). Never modify generated files inside `cmake_targets/ran_build/build`.

## Coding Conventions
- Follow `.clang-format` (two spaces, 132-char lines, braces per `doc/code-style-contrib.md`). Run `pre-commit-clang` or `clang-format -i` before posting patches.
- Use `AssertFatal`/`DevAssert` only for invariants; real error paths must return status codes logged via `NFAPI_TRACE`/`LOG_*` macros from `openair2/UTIL`.
- Shared utilities belong under `common/` or `openair2/UTILS`; avoid duplicating helpers already present (search first).


## Feature-Specific Tips

### nFAPI Delay Management & Slot Timing[2][1]
Delay management ensures P7 slot procedures occur in a timely fashion per SCF-225 section 2.1.3.4 and SCF-222 section 2.2.1:

- **Receive Timing Windows**: Each PHY instance maintains per-message-type receive windows characterized by two parameters (both in microseconds):
  - **Timing Window**: Size of the buffering interval
  - **<msg> Timing Offset**: Offset prior to the targeted slot (where `<msg>` ∈ {`DL_TTI`, `UL_TTI`, `UL_DCI`, `TX_Data`})
  
- **Window Anchoring**: Receive windows are anchored to slots; slot anchoring is specific to each PDU's numerology:
  - PRACH PDUs for long preambles anchor to 15 kHz numerology
  - SSB PDUs may target highest PDSCH numerology if lower than SSB numerology
  - PRACH PDUs may target highest PUSCH numerology if lower than PRACH numerology

- **Message Arrival Handling**:
  - **Inside window**: Processed for transmission/reception at targeted slot
  - **Outside window**: Marked as 'too early' or 'too late' and triggers Timing Info report to VNF
  - Late arrivals result in slot loss (DL slot lost if `DL_TTI`/`TX_Data` late; UL slot lost if `UL_TTI`/`UL_DCI` late)

- **Two Delay Management Modes**:
  - **With Timestamps** (section 2.2.1.1): Uses DL/UL Node Sync messages for latency probing (t1/t2/t3 timestamps) and Timing Info messages for arrival reports
  - **Without Timestamps** (section 2.2.1.2): Uses TIMING.indication (event-driven or periodic) instead of Node Sync; L2 adjusts transmission window based on feedback

- **Implementation Requirements**:
  - Timing parameters exchanged through `PNF_PARAM` procedure
  - Configuration knobs surfaced via `nfapi_vnf_p7_config_t` and delay management TLVs (Table 3-21 in SCF-225)
  - Update slot timing code synchronously across `nfapi/oai_integration`, `nfapi/open-nFAPI/vnf/src`, and `openair2/LAYER2/NR_MAC` schedulers
  - Use microsecond precision for all timing parameters; match SCF-225 Figure 2-11 window semantics

### nFAPI Architecture & State Machines
- **PNF Lifecycle**: PNF device moves through IDLE → CONFIGURED → RUNNING states via `PNF_PARAM`, `PNF_CONFIG`, `PNF_START` message exchanges
- **PHY Instantiation**: Not triggered by PNF initialization; requires DFE/RF instances initialized + DFE Profile selected + compatible PHY Profile selected (section 2.1.1)
- **Message Categories**:
  - **Dedicated nFAPI messages**: PNF management (`PNF_READY`, `PNF_PARAM`, `PNF_CONFIG`, `PNF_START/STOP`)
  - **Combined messages**: Wrap FAPI messages with nFAPI headers
  - **Transparent messages**: P5/P7 messages passed through with minimal nFAPI framing

### General Configuration & Testing
- External dependencies are managed via CPM inside `cmake_targets/CPM.cmake`; prefer adding CPM packages over vendoring code.
- Scheduler knobs (e.g., `sl_ahead`) and radio device selection must be surfaced through existing structs (`gNB_RrcConfigurationReq`); do not add free-form globals.


Keep instructions concise in future edits and update this file whenever workflows or entrypoints change.
