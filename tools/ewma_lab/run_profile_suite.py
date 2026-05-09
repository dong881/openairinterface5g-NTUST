#!/usr/bin/env python3
"""Run multiple TC profiles, then aggregate rankings across runs."""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd


@dataclass(frozen=True)
class Profile:
    delay_us: int
    jitter_us: int

    @property
    def tc_start_cmd(self) -> str:
        return f"sudo ~/tc_manager.sh start {self.delay_us}us {self.jitter_us}us"

    @property
    def tag(self) -> str:
        return f"d{self.delay_us}_j{self.jitter_us}"


def run_cmd(command: str) -> None:
    print(f"$ {command}", flush=True)
    subprocess.run(command, shell=True, text=True, check=True)


def list_runs(out_dir: Path) -> set[Path]:
    if not out_dir.exists():
        return set()
    return {p for p in out_dir.iterdir() if p.is_dir() and p.name.startswith("ewma_")}


def run_one_profile(args: argparse.Namespace, profile: Profile) -> Path:
    before = list_runs(args.out_dir)
    cmd = (
        f"{shlex.quote(sys.executable)} {shlex.quote(str(args.runner))} "
        f"--preset focused --out-dir {shlex.quote(str(args.out_dir))} "
        f"--warmup-sec {args.warmup_sec} --measure-sec {args.measure_sec} --cooldown-sec {args.cooldown_sec} "
        f"--skip-build --pnf-build-dir {shlex.quote(args.pnf_build_dir)} "
        f"--tc-start-cmd {shlex.quote(profile.tc_start_cmd)} "
        f"--tc-stop-cmd {shlex.quote(args.tc_stop_cmd)}"
    )
    run_cmd(cmd)
    after = list_runs(args.out_dir)
    created = sorted(after - before)
    if not created:
        raise RuntimeError(f"Could not detect created run directory for profile {profile.tag}")
    return created[-1]


def rank_from_csv(csv_path: Path, score_field: str) -> tuple[int, pd.DataFrame]:
    df = pd.read_csv(csv_path)
    ranked = df.sort_values(score_field, ascending=True).reset_index(drop=True)
    ranked["rank"] = np.arange(1, len(ranked) + 1)
    target_rank = int(ranked.loc[(ranked["alpha"] == 8) & (ranked["beta"] == 4), "rank"].iloc[0])
    return target_rank, ranked


def aggregate(run_infos: list[dict[str, object]], out_file: Path) -> None:
    rows: list[dict[str, object]] = []
    for info in run_infos:
        profile = str(info["profile"])
        run_dir = Path(str(info["run_dir"]))
        csv = run_dir / "analysis_legacy" / "ewma_sweep_summary.csv"
        df = pd.read_csv(csv)
        for _, r in df.iterrows():
            rows.append(
                {
                    "profile": profile,
                    "run_dir": str(run_dir),
                    "alpha": int(r["alpha"]),
                    "beta": int(r["beta"]),
                    "score_with_pnf": float(r["score_with_pnf"]),
                    "score": float(r["score"]),
                    "pnf_too_late_count": int(r["pnf_too_late_count"]),
                    "pnf_too_late_max_us": int(r["pnf_too_late_max_us"]),
                    "adjust": int(r["adjust"]),
                    "osc": int(r["osc"]),
                    "avg_abs_late": float(r["avg_abs_late"]),
                    "avg_late_to_up_delay": float(r["avg_late_to_up_delay"]),
                }
            )
    all_df = pd.DataFrame(rows)
    # Per-profile ranking using legacy score
    per_profile_rank = []
    for profile, g in all_df.groupby("profile"):
        gr = g.sort_values("score_with_pnf").reset_index(drop=True)
        gr["rank"] = np.arange(1, len(gr) + 1)
        per_profile_rank.append(gr)
    rank_df = pd.concat(per_profile_rank, ignore_index=True)

    summary = (
        rank_df.groupby(["alpha", "beta"], as_index=False)
        .agg(
            mean_rank=("rank", "mean"),
            max_rank=("rank", "max"),
            min_rank=("rank", "min"),
            mean_late_count=("pnf_too_late_count", "mean"),
            max_late_count=("pnf_too_late_count", "max"),
            mean_adjust=("adjust", "mean"),
            mean_osc=("osc", "mean"),
            mean_score_with_pnf=("score_with_pnf", "mean"),
            profiles=("profile", "nunique"),
        )
        .sort_values(["mean_rank", "max_rank", "mean_late_count", "mean_osc", "mean_adjust"])
        .reset_index(drop=True)
    )
    summary.to_csv(out_file, index=False)

    # Region: robust set where rank <= 4 in every profile and no catastrophic late spikes
    robust = summary[(summary["max_rank"] <= 4) & (summary["max_late_count"] <= 100)]
    robust_out = out_file.with_name("profile_suite_robust_region.csv")
    robust.to_csv(robust_out, index=False)

    report_path = out_file.with_name("profile_suite_report.md")
    lines = []
    lines.append("# EWMA Profile Suite Report")
    lines.append("")
    lines.append("## Goal")
    lines.append("- Find scenarios where `a=1/8,b=1/4` reaches rank 1.")
    lines.append("- Determine overall robust best setting and acceptable parameter region.")
    lines.append("")
    lines.append("## Profiles Executed")
    for info in run_infos:
        lines.append(f"- `{info['profile']}` => `{info['run_dir']}`")
    lines.append("")
    lines.append("## `a=1/8,b=1/4` Per-Profile Rank")
    for info in run_infos:
        lines.append(f"- `{info['profile']}`: rank {info['target_rank_8_4']}")
    lines.append("")
    lines.append("## Overall Ranking (Mean Rank)")
    for _, r in summary.head(8).iterrows():
        lines.append(
            f"- a=1/{int(r['alpha'])}, b=1/{int(r['beta'])}: "
            f"mean_rank={r['mean_rank']:.2f}, max_rank={int(r['max_rank'])}, "
            f"mean_late_count={r['mean_late_count']:.1f}, mean_osc={r['mean_osc']:.1f}, mean_adjust={r['mean_adjust']:.1f}"
        )
    lines.append("")
    lines.append("## Robust Acceptable Region")
    if robust.empty:
        lines.append("- No pair satisfies strict robust criteria `(max_rank<=4 and max_late_count<=100)`.")
    else:
        lines.append("- Pairs satisfying `(max_rank<=4 and max_late_count<=100)`:")
        for _, r in robust.iterrows():
            lines.append(
                f"  - a=1/{int(r['alpha'])}, b=1/{int(r['beta'])} "
                f"(mean_rank={r['mean_rank']:.2f}, max_rank={int(r['max_rank'])}, max_late_count={int(r['max_late_count'])})"
            )
    lines.append("")
    lines.append("## Reasonability Notes")
    lines.append("- Ranking stability across multiple TC profiles is prioritized over single-run wins.")
    lines.append("- Catastrophic late-count outliers are filtered in robust-region definition.")
    lines.append("- If `a=1/8,b=1/4` is rank-1 in some profiles and robust top-tier overall, that is explainable as a balanced tuning point.")
    report_path.write_text("\n".join(lines), encoding="utf-8")

    print(f"Wrote {out_file}")
    print(f"Wrote {robust_out}")
    print(f"Wrote {report_path}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, default=Path("/home/hpe/openairinterface5g/tools/ewma_lab/run_ewma_sweep.py"))
    parser.add_argument("--out-dir", type=Path, default=Path("/home/hpe/ewma_sweep_runs/profile_suite"))
    parser.add_argument("--pnf-build-dir", default="~/oai_mp_f_ming/openairinterface5g/cmake_targets/ran_build/build")
    parser.add_argument("--tc-stop-cmd", default="sudo ~/tc_manager.sh stop")
    parser.add_argument("--warmup-sec", type=int, default=10)
    parser.add_argument("--measure-sec", type=int, default=30)
    parser.add_argument("--cooldown-sec", type=int, default=2)
    parser.add_argument(
        "--profiles",
        default="1400:100,1600:200,1800:300,2200:500",
        help="Comma-separated delay:jitter pairs in microseconds.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    profiles = []
    for item in args.profiles.split(","):
        d, j = item.split(":")
        profiles.append(Profile(delay_us=int(d), jitter_us=int(j)))

    run_infos: list[dict[str, object]] = []
    analyzer = Path("/home/hpe/openairinterface5g/tools/ewma_lab/analyze_ewma_sweep.py")
    score_models = ["legacy", "all_ones_raw", "log_pnf", "norm_equal_3", "two_stage_hierarchical"]

    for profile in profiles:
        print(f"\n=== Running profile {profile.tag} ===", flush=True)
        run_dir = run_one_profile(args, profile)
        info: dict[str, object] = {"profile": profile.tag, "run_dir": str(run_dir)}
        for model in score_models:
            out = run_dir / f"analysis_{model}"
            cmd = (
                f"{shlex.quote(sys.executable)} {shlex.quote(str(analyzer))} "
                f"--log-dir {shlex.quote(str(run_dir))} --out-dir {shlex.quote(str(out))} "
                f"--score-model {model}"
            )
            run_cmd(cmd)
        target_rank, _ = rank_from_csv(run_dir / "analysis_legacy" / "ewma_sweep_summary.csv", "score_with_pnf")
        info["target_rank_8_4"] = target_rank
        run_infos.append(info)

    summary_json = args.out_dir / "profile_suite_runs.json"
    summary_json.write_text(json.dumps(run_infos, indent=2), encoding="utf-8")
    aggregate(run_infos, args.out_dir / "profile_suite_summary.csv")
    print(f"Wrote {summary_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
