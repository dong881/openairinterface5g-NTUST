---
name: OpenAirInterface RAN Development Assistant
description: Expert guidance for OpenAirInterface 5G RAN development, focusing on nFAPI P7 delay management, PHY/L2/L3 integration, and CI/CD workflows.

---

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
  ./build_oai -w USRP --gNB --nrUE --ninja
  ```
- Use `./build_oai -I --install-optional-packages -w <radio>` to install prerequisites; UHD-from-source toggled via `BUILD_UHD_FROM_SOURCE`/`UHD_VERSION` env vars (`doc/BUILD.md`).
- One-off cmake builds belong in throwaway `build/` folders (see `doc/BUILD.md` for target list). Never modify generated files inside `cmake_targets/ran_build/build`.


## Coding Conventions
- Follow `.clang-format` (two spaces, 132-char lines, braces per `doc/code-style-contrib.md`). Run `pre-commit-clang` or `clang-format -i` before posting patches.
- Use `AssertFatal`/`DevAssert` only for invariants; real error paths must return status codes logged via `NFAPI_TRACE`/`LOG_*` macros from `openair2/UTIL`.
- Shared utilities belong under `common/` or `openair2/UTILS`; avoid duplicating helpers already present (search first).


## Feature-Specific Tips

### nFAPI P7 Delay Management & Slot Timing[3]

#### Core Architecture
The nFAPI P7 interface connects VNF (L2/L3 Scheduler) and PHY instances on a slot-by-slot basis over network transport, requiring explicit delay management to ensure time-critical messages (DL_TTI, UL_TTI, TxData, UL_DCI) arrive before their target slots start. Unlike shared-memory FAPI with SLOT.indication, nFAPI P7 must account for fronthaul latency and non-deterministic message delivery order.[3]

#### Timing Window Management[3]
Each PHY instance maintains per-message-type receive windows defined by two parameters (both in microseconds):[3]
- **Timing Offset** (TLVs 0x0106–0x0109): Microseconds before slot start when window opens
- **Window Size** (TLV 0x011E): Duration in microseconds; receives windows close when `window_start - window_size` is reached[3]

**Window Logic**:[3]
```c
window_start = slot_start_time - timing_offset;
window_end = window_start - timing_window;
if (msg_arrival_time >= window_end && msg_arrival_time <= window_start) {
    process_for_slot();           // on-time
} else if (msg_arrival_time > window_start) {
    record_early_offset();        // early arrival
} else {
    record_late_offset();         // late arrival; slot lost
    mark_slot_lost();
}
```

#### Slot Anchoring & Multiple Numerologies[3]
When multiple numerologies coexist, window anchoring is numerology-specific:[3]
- **PRACH (long preambles)**: Anchored to 15 kHz numerology window
- **SSB**: If SSB numerology > PDSCH numerology, use PDSCH window
- **PUSCH**: If PRACH numerology > PUSCH numerology, use PUSCH window

#### Message Arrival Handling & Slot Loss[3]
- **Inside window**: Message processed for transmission/reception at targeted slot
- **Outside window (early/late)**: Triggers **Timing Info** report to VNF with detailed statistics (arrival offset, jitter per RFC 3550)[3]
- **Late arrivals**: Marked as slot loss (DL slot lost if `DL_TTI`/`TX_Data` late; UL slot lost if `UL_TTI`/`UL_DCI` late)[3]

#### Two Delay Management Modes[3]

**Mode 1: Timestamp-based (with Node Sync)**[3]
- Uses DL/UL Node Sync messages (VNF→PHY and PHY→VNF) for round-trip latency probing with timestamps (t1 transmit, t2 PHY receive, t3 PHY transmit)[3]
- Roundtrip latency: RTT = t3 - t2[3]
- PHY collects arrival statistics and generates periodic/event-driven Timing Info messages reporting jitter, latest delay, and earliest arrival[3]
- VNF adjusts timing offsets dynamically based on feedback (high jitter → widen window; frequent late → increase offset)[3]

**Mode 2: Event-driven (without Timestamps)**[3]
- No transmit timestamps; relies on TIMING.indication messages (periodic or event-driven per TLVs 0x011F/0x0120)[3]
- L2 adjusts transmission window based on reported jitter and delay feedback (lower real-time precision, suitable for legacy/FAPI-only deployments)[3]

#### Configuration Parameters & TLVs[3]
| TLV Tag | Parameter              | Range      | Units | Purpose                      |
|---------|------------------------|------------|-------|------------------------------|
| 0x0106  | DL_TTI Timing Offset   | 0–65535    | µs    | Window start time for DL_TTI |
| 0x0107  | UL_TTI Timing Offset   | 0–65535    | µs    | Window start time for UL_TTI |
| 0x0108  | UL_DCI Timing Offset   | 0–65535    | µs    | Window start time for UL_DCI |
| 0x0109  | TxData Timing Offset   | 0–65535    | µs    | Window start time for TxData |
| 0x011E  | Timing Window          | 0–30000    | µs    | Window duration (all message types) |
| 0x011F  | Info Mode              | 0–2        | enum  | Periodic/aperiodic Timing Info reporting |
| 0x0120  | Info Period            | 1–255      | slots | Timing Info reporting interval |

#### Jitter Calculation[3]
RFC 3550-based with message-specific deltas:
```
delta = (arrival_time_n - arrival_time_{n-1}) - (transmit_ts_n - transmit_ts_{n-1})
jitter = jitter + (|delta| - jitter) / 16
```

#### Implementation Requirements[3]
- **Initialization**: Exchange `PNF_PARAM` → `PNF_CONFIG` → `PNF_START` messages; use initial timing offsets based on network latency estimates or Node Sync probes[3]
- **Synchronization**: Update slot timing code synchronously across `nfapi/oai_integration` (P7 message handling), `nfapi/open-nFAPI/vnf/src` (VNF-side scheduling), and `openair2/LAYER2/NR_MAC` (MAC scheduler)[3]
- **Microsecond Precision**: All timing parameters use microseconds; validate timing window compliance per SCF-225 Figure 2-11[3]
- **Error Recovery**: On multiple late arrivals, VNF sends Node Sync to recalibrate; PHY marks slots lost and logs via `NFAPI_TRACE` macros[3]

### nFAPI Architecture & State Machines[1]
- **PNF Lifecycle**: PNF device transitions IDLE → CONFIGURED → RUNNING via `PNF_PARAM`, `PNF_CONFIG`, `PNF_START` message exchanges[1]
- **PHY Instantiation**: Not triggered by PNF initialization; requires DFE/RF instances initialized + DFE Profile selected + compatible PHY Profile selected[1]
- **Message Categories**:[1]
  - **Dedicated nFAPI messages**: PNF management (`PNF_READY`, `PNF_PARAM`, `PNF_CONFIG`, `PNF_START`/`STOP`)
  - **Combined messages**: Wrap FAPI messages with nFAPI P7 headers
  - **Transparent messages**: P5/P7 messages passed through with minimal nFAPI framing

### Test & Debug Habits
- End-to-end validation runs through `ci-scripts/run_locally.sh <scenario.xml>` which bootstraps CN/gNB/UE containers and emits logs into `cmake_targets/log/<scenario>.d/` plus HTML reports.
- Simulator binaries (`nr_dlsim`, `nr_ulsim`, etc.) are produced via `./build_oai --phy_simulators` and live in `cmake_targets/ran_build/build`; keep deterministic seeds when editing PHY algorithms.
- Docker workflows rely on `docker/README.md` and CI YAML templates under `ci-scripts/yaml_files/`; align new scenarios with these rather than ad-hoc scripts.
- For nFAPI P7 delay management testing: validate timing window compliance (on-time/early/late arrivals), jitter accumulation over long runs, multi-numerology slot anchoring, and Node Sync round-trip latency under simulated network conditions (packet loss, reordering).[3]

### General Configuration & Testing
- External dependencies are managed via CPM inside `cmake_targets/CPM.cmake`; prefer adding CPM packages over vendoring code.
- Scheduler knobs (e.g., `sl_ahead`) and radio device selection must be surfaced through existing structs (`nfapi_vnf_p7_config_t`, `gNB_RrcConfigurationReq`); do not add free-form globals.
- Sample delay management configurations:[3]
  - **Low-latency networks**: offset = 300µs, window = 100µs (e.g., local testbed)
  - **Medium-latency networks**: offset = 500µs, window = 150µs (e.g., fronthaul over IP)
  - **High-latency networks**: offset = 800µs, window = 200µs (e.g., split DU-RU over WAN)


Keep instructions concise in future edits and update this file whenever workflows or entrypoints change.

[1](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_7bc30214-9957-4cbd-aa20-e7dc476161fd/1e0b512b-9dce-4e3e-9cfa-e71f8fbb2538/SCF225_5G_nFAPI_specification-JULY-22.pdf)
[2](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_7bc30214-9957-4cbd-aa20-e7dc476161fd/128a8eee-4bf4-4c97-872e-34e2ce518748/SCF222_5G_FAPI_PHY_API_specification-DelayManagment.pdf)
[3](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/27146824/8fabc5b1-c69d-4085-95f1-f9ac8f3543b4/NFAPI_P7_Delay_Management_Dev_Manual_EN.md)