#!/usr/bin/env bash
set -euo pipefail

# One-click entrypoint for EWMA experiment reproduction.
# Defaults are aligned with observed operator history:
# - VNF split-direct config
# - PNF split-direct config on host "super"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="${SCRIPT_DIR}/run_ewma_sweep.py"

PRESET="${PRESET:-focused}"                  # focused | full
GRID="${GRID:-}"                             # optional: "8:4,4:4"
OUT_DIR="${OUT_DIR:-$HOME/ewma_sweep_runs}"

WARMUP_SEC="${WARMUP_SEC:-15}"
MEASURE_SEC="${MEASURE_SEC:-45}"
COOLDOWN_SEC="${COOLDOWN_SEC:-2}"

PNF_HOST="${PNF_HOST:-super}"
PNF_BUILD_DIR="${PNF_BUILD_DIR:-~/oai_mp_f_ming/openairinterface5g/cmake_targets/ran_build/build}"
REMOTE_LOG_DIR="${REMOTE_LOG_DIR:-~/gNB-logs/ewma_sweep}"

TC_START_CMD="${TC_START_CMD:-sudo ~/tc_manager.sh start 1400us 100us}"
TC_STOP_CMD="${TC_STOP_CMD:-sudo ~/tc_manager.sh stop}"

BUILD_DIR="${BUILD_DIR:-/home/hpe/openairinterface5g/cmake_targets/ran_build/build}"
VNF_CONF="${VNF_CONF:-../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-vnf-split-direct.sa.band78.273prb.nfapi-bmw.conf}"
PNF_CONF="${PNF_CONF:-../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-pnf-split-direct.sa.band78.fhi72.nfapi.4x4-pegatron.conf}"

SKIP_BUILD_FLAG=""
if [[ "${SKIP_BUILD:-1}" == "1" ]]; then
  SKIP_BUILD_FLAG="--skip-build"
fi

GRID_FLAG=""
if [[ -n "${GRID}" ]]; then
  GRID_FLAG="--grid ${GRID}"
fi

echo "[ONECLICK] Starting EWMA experiment"
echo "[ONECLICK] preset=${PRESET} warmup=${WARMUP_SEC}s measure=${MEASURE_SEC}s cooldown=${COOLDOWN_SEC}s"
echo "[ONECLICK] vnf_conf=${VNF_CONF}"
echo "[ONECLICK] pnf_conf=${PNF_CONF}"
echo "[ONECLICK] tc_start=${TC_START_CMD}"

python3 "${RUNNER}" \
  --preset "${PRESET}" \
  ${GRID_FLAG} \
  --out-dir "${OUT_DIR}" \
  --build-dir "${BUILD_DIR}" \
  ${SKIP_BUILD_FLAG} \
  --pnf-host "${PNF_HOST}" \
  --pnf-build-dir "${PNF_BUILD_DIR}" \
  --remote-log-dir "${REMOTE_LOG_DIR}" \
  --vnf-conf "${VNF_CONF}" \
  --pnf-conf "${PNF_CONF}" \
  --warmup-sec "${WARMUP_SEC}" \
  --measure-sec "${MEASURE_SEC}" \
  --cooldown-sec "${COOLDOWN_SEC}" \
  --tc-start-cmd "${TC_START_CMD}" \
  --tc-stop-cmd "${TC_STOP_CMD}"

echo "[ONECLICK] Completed."
