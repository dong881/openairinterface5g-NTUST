#!/usr/bin/env python3
"""Run an automated EWMA alpha/beta sweep with VNF local and PNF over SSH."""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


FOCUSED_GRID = [(4, 2), (4, 4), (4, 8), (8, 2), (8, 4), (8, 8), (16, 2), (16, 4), (16, 8)]
FULL_DENOMS = [1, 2, 4, 8, 16, 32, 64]


def run_cmd(
    command: str,
    *,
    check: bool = True,
    dry_run: bool = False,
    display_command: str | None = None,
) -> subprocess.CompletedProcess[str]:
    print(f"$ {display_command or command}", flush=True)
    if dry_run:
        return subprocess.CompletedProcess(command, 0, "", "")
    return subprocess.run(command, shell=True, text=True, check=check)


def ssh(
    host: str,
    command: str,
    *,
    check: bool = True,
    dry_run: bool = False,
    display_command: str | None = None,
) -> subprocess.CompletedProcess[str]:
    return run_cmd(
        f"ssh {shlex.quote(host)} {shlex.quote(command)}",
        check=check,
        dry_run=dry_run,
        display_command=display_command or f"ssh {host} {command}",
    )


def remote_sudo_prefix(args: argparse.Namespace) -> tuple[str, str]:
    sudo_password = args.pnf_sudo_password or os.environ.get("EWMA_PNF_SUDO_PASSWORD")
    if sudo_password:
        return (f"echo {shlex.quote(sudo_password)} | sudo -S -p ''", "sudo -S -p '' [REDACTED]")
    return ("sudo", "sudo")


def quote_remote_path(path: str) -> str:
    if path.startswith("~/"):
        return "~/" + shlex.quote(path[2:])
    return shlex.quote(path)


def build_grid(args: argparse.Namespace) -> list[tuple[int, int]]:
    if args.grid:
        grid: list[tuple[int, int]] = []
        for item in args.grid.split(","):
            alpha, beta = item.split(":", 1)
            grid.append((int(alpha), int(beta)))
        return grid
    if args.preset == "full":
        return [(alpha, beta) for alpha in FULL_DENOMS for beta in FULL_DENOMS]
    return FOCUSED_GRID


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def stop_vnf(args: argparse.Namespace) -> None:
    run_cmd(
        "screen -S VNF_SESSION -X stuff $'\\003\\n' 2>/dev/null || true; "
        "sleep 1; screen -S VNF_SESSION -X quit 2>/dev/null || true",
        check=False,
        dry_run=args.dry_run,
    )


def stop_pnf(args: argparse.Namespace) -> None:
    ssh(
        args.pnf_host,
        "screen -S PNF_SESSION -X stuff $'\\003\\n' 2>/dev/null || true; "
        "sleep 1; screen -S PNF_SESSION -X quit 2>/dev/null || true",
        check=False,
        dry_run=args.dry_run,
    )


def stop_tc(args: argparse.Namespace) -> None:
    run_cmd(args.tc_stop_cmd, check=False, dry_run=args.dry_run)


def start_tc(args: argparse.Namespace) -> None:
    run_cmd(args.tc_start_cmd, check=True, dry_run=args.dry_run)


def build_nr_softmodem(args: argparse.Namespace) -> None:
    if args.skip_build:
        return
    run_cmd(f"cd {shlex.quote(str(args.build_dir))} && sudo ninja nr-softmodem", dry_run=args.dry_run)
    if not args.skip_pnf_build:
        remote_sudo, remote_display = remote_sudo_prefix(args)
        ssh(
            args.pnf_host,
            f"cd {quote_remote_path(args.pnf_build_dir)} && {remote_sudo} ninja nr-softmodem",
            display_command=f"ssh {args.pnf_host} 'cd {quote_remote_path(args.pnf_build_dir)} && {remote_display} ninja nr-softmodem'",
            dry_run=args.dry_run,
        )


def start_vnf(args: argparse.Namespace, alpha: int, beta: int, run_dir: Path) -> Path:
    log_path = run_dir / f"nfapi-VNF-ewma-a{alpha}_b{beta}.log"
    env = {
        "SLOT_AHEAD": str(args.slot_ahead),
        "DYNAMIC_TIMING": str(args.dynamic_timing),
        "TIMING_INFO_MODE": str(args.timing_info_mode),
        "TIMING_INFO_PERIOD": str(args.timing_info_period),
        "TIMING_WINDOW": str(args.timing_window),
        "MAX_S_AHEAD": str(args.max_s_ahead),
        "EWMA_ALPHA": str(alpha),
        "EWMA_BETA": str(beta),
        "EWMA_SUMMARY_PERIOD": str(args.ewma_summary_period),
        "EWMA_CSV_EVERY": str(args.ewma_csv_every),
        "EWMA_ONLY_CONTROL": "1",
        "RAW_WORST_LATE_CONTROL": "0",
        "NFAPI_TRACE_LEVEL": "info",
    }
    env_words = " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items())
    command = (
        "screen -dmS VNF_SESSION bash -lc "
        + shlex.quote(
            f"cd {shlex.quote(str(args.build_dir))} && "
            f"sudo -E env {env_words} numactl --cpunodebind=1 --membind=1 "
            f"taskset -c {shlex.quote(args.vnf_taskset)} ./nr-softmodem "
            f"-O {shlex.quote(args.vnf_conf)} --nfapi VNF 2>&1 | tee {shlex.quote(str(log_path))}"
        )
    )
    run_cmd(command, dry_run=args.dry_run)
    return log_path


def start_pnf(args: argparse.Namespace, alpha: int, beta: int, run_dir: Path) -> Path:
    local_log = run_dir / f"nfapi-PNF-ewma-a{alpha}_b{beta}.log"
    remote_log = f"{args.remote_log_dir}/nfapi-PNF-ewma-a{alpha}_b{beta}.log"
    ssh(args.pnf_host, f"mkdir -p {quote_remote_path(args.remote_log_dir)} && rm -f {quote_remote_path(remote_log)}", dry_run=args.dry_run)
    remote_sudo, remote_display = remote_sudo_prefix(args)
    pnf_command = (
        f"cd {quote_remote_path(args.pnf_build_dir)} && "
        f"{remote_sudo} NFAPI_TRACE_LEVEL=info ./nr-softmodem -O {shlex.quote(args.pnf_conf)} "
        f"--nfapi PNF {args.pnf_extra_args} 2>&1 | tee {quote_remote_path(remote_log)}"
    )
    ssh(
        args.pnf_host,
        "screen -dmS PNF_SESSION bash -lc " + shlex.quote(pnf_command),
        display_command=(
            f"ssh {args.pnf_host} 'screen -dmS PNF_SESSION bash -lc [REDACTED remote sudo command]'") ,
        dry_run=args.dry_run,
    )
    local_log.write_text(f"PNF log will be fetched from {args.pnf_host}:{remote_log}\n", encoding="utf-8")
    return local_log


def fetch_pnf_log(args: argparse.Namespace, alpha: int, beta: int, local_log: Path) -> None:
    remote_log = f"{args.remote_log_dir}/nfapi-PNF-ewma-a{alpha}_b{beta}.log"
    run_cmd(
        f"scp {shlex.quote(args.pnf_host)}:{quote_remote_path(remote_log)} {shlex.quote(str(local_log))}",
        check=False,
        dry_run=args.dry_run,
    )


def wait_for_logs(vnf_log: Path, pnf_log: Path, args: argparse.Namespace) -> None:
    if args.dry_run:
        return
    deadline = time.time() + args.start_timeout
    while time.time() < deadline:
        if vnf_log.exists() and pnf_log.exists():
            return
        time.sleep(1)
    print("WARNING: startup logs were not both visible before timeout; continuing measurement", file=sys.stderr)


def run_analyzer(args: argparse.Namespace, output_dir: Path) -> None:
    analyzer = Path(__file__).with_name("analyze_ewma_sweep.py")
    run_cmd(
        f"{shlex.quote(sys.executable)} {shlex.quote(str(analyzer))} "
        f"--log-dir {shlex.quote(str(output_dir))} --out-dir {shlex.quote(str(output_dir / 'analysis'))}",
        dry_run=args.dry_run,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", choices=["focused", "full"], default="focused")
    parser.add_argument("--grid", help="Comma-separated alpha:beta list, for example 8:4,4:4")
    parser.add_argument("--build-dir", type=Path, default=Path("/home/hpe/openairinterface5g/cmake_targets/ran_build/build"))
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-pnf-build", action="store_true")
    parser.add_argument("--out-dir", type=Path, default=Path.home() / "ewma_sweep_runs")
    parser.add_argument("--vnf-conf", default="../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-vnf-split-direct.sa.band78.273prb.nfapi-bmw.conf")
    parser.add_argument("--vnf-taskset", default="8-15,40-47")
    parser.add_argument("--pnf-host", default="super")
    parser.add_argument("--pnf-build-dir", default="~/oai_mp_f_ming/openairinterface5g/cmake_targets/ran_build/build")
    parser.add_argument(
        "--pnf-conf",
        default="../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb-pnf-split-direct.sa.band78.fhi72.nfapi.4x4-pegatron.conf",
    )
    parser.add_argument("--pnf-extra-args", default="--thread-pool 8,9,10,11,13,14,15,1")
    parser.add_argument("--pnf-sudo-password", default="", help="Optional remote sudo password for super host (or set EWMA_PNF_SUDO_PASSWORD)")
    parser.add_argument("--remote-log-dir", default="~/gNB-logs/ewma_sweep")
    parser.add_argument("--slot-ahead", type=int, default=8)
    parser.add_argument("--max-s-ahead", type=int, default=14)
    parser.add_argument("--dynamic-timing", type=int, default=1)
    parser.add_argument("--timing-info-mode", type=int, default=1)
    parser.add_argument("--timing-info-period", type=int, default=20)
    parser.add_argument("--timing-window", type=int, default=2000)
    parser.add_argument("--ewma-summary-period", type=int, default=16)
    parser.add_argument("--ewma-csv-every", type=int, default=1)
    parser.add_argument("--warmup-sec", type=int, default=30)
    parser.add_argument("--measure-sec", type=int, default=90)
    parser.add_argument("--cooldown-sec", type=int, default=5)
    parser.add_argument("--start-timeout", type=int, default=30)
    parser.add_argument("--tc-start-cmd", default="sudo ~/tc_manager.sh start 1400us 100us")
    parser.add_argument("--tc-stop-cmd", default="sudo ~/tc_manager.sh stop")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    grid = build_grid(args)
    run_id = datetime.now().strftime("ewma_%Y%m%d_%H%M%S")
    output_dir = args.out_dir / run_id
    ensure_dir(output_dir)

    manifest = {
        "run_id": run_id,
        "grid": grid,
        "args": {key: str(value) for key, value in vars(args).items()},
    }
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    build_nr_softmodem(args)

    try:
        for index, (alpha, beta) in enumerate(grid, start=1):
            run_dir = output_dir / f"{index:02d}_a{alpha}_b{beta}"
            ensure_dir(run_dir)
            print(f"\n=== EWMA sweep {index}/{len(grid)}: alpha=1/{alpha}, beta=1/{beta} ===", flush=True)

            stop_tc(args)
            stop_vnf(args)
            stop_pnf(args)

            vnf_log = start_vnf(args, alpha, beta, run_dir)
            pnf_log = start_pnf(args, alpha, beta, run_dir)
            wait_for_logs(vnf_log, pnf_log, args)

            print(f"Warm-up {args.warmup_sec}s without TC impairment", flush=True)
            if not args.dry_run:
                time.sleep(args.warmup_sec)

            start_tc(args)
            print(f"Measurement {args.measure_sec}s with TC impairment", flush=True)
            if not args.dry_run:
                time.sleep(args.measure_sec)

            stop_tc(args)
            if not args.dry_run:
                time.sleep(args.cooldown_sec)
            stop_vnf(args)
            stop_pnf(args)
            fetch_pnf_log(args, alpha, beta, pnf_log)
    finally:
        stop_tc(args)
        stop_vnf(args)
        stop_pnf(args)

    run_analyzer(args, output_dir)
    print(f"\nEWMA sweep output: {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
