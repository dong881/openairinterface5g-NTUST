#!/usr/bin/env python3
"""Parse EWMA lab logs and generate CSV plus non-empty validation plots."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path
from typing import Iterable

import numpy as np


SUMMARY_RE = re.compile(
    r"\[P7_EWMA_SUMMARY\],"
    r"alpha=(?P<alpha>\d+),beta=(?P<beta>\d+),total=(?P<total>\d+),"
    r"up=(?P<up>\d+),down=(?P<down>\d+),stable=(?P<stable>\d+),"
    r"adjust=(?P<adjust>\d+),osc=(?P<osc>\d+),"
    r"min_s=(?P<min_s>-?\d+),max_s=(?P<max_s>-?\d+),"
    r"min_late=(?P<min_late>-?\d+),max_late=(?P<max_late>-?\d+),"
    r"avg_abs_late=(?P<avg_abs_late>[-+]?\d+(?:\.\d+)?),"
    r"stable_ratio=(?P<stable_ratio>[-+]?\d+(?:\.\d+)?),"
    r"late_risk=(?P<late_risk>\d+),late_reacted=(?P<late_reacted>\d+),"
    r"avg_late_to_up_delay=(?P<avg_late_to_up_delay>[-+]?\d+(?:\.\d+)?),"
    r"max_late_to_up_delay=(?P<max_late_to_up_delay>\d+),"
    r"score=(?P<score>[-+]?\d+(?:\.\d+)?)"
)

PNF_LATE_CSV_RE = re.compile(
    r"\[P7_PNF_LATE_CSV\],time=(?P<time>\d+),type=(?P<type>[^,]+),"
    r"sfn=(?P<sfn>\d+),slot=(?P<slot>\d+),late_us=(?P<late_us>\d+)"
)
PNF_TOO_LATE_RE = re.compile(r"TOO LATE by (?P<late_us>\d+) us")
RUN_NAME_RE = re.compile(r"a(?P<alpha>\d+)_b(?P<beta>\d+)")


INT_FIELDS = {
    "alpha",
    "beta",
    "total",
    "up",
    "down",
    "stable",
    "adjust",
    "osc",
    "min_s",
    "max_s",
    "min_late",
    "max_late",
    "late_risk",
    "late_reacted",
    "max_late_to_up_delay",
}
FLOAT_FIELDS = {"avg_abs_late", "stable_ratio", "avg_late_to_up_delay", "score"}


def parse_summary_logs(paths: Iterable[Path]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for path in paths:
        with path.open("r", encoding="utf-8", errors="ignore") as stream:
            for line in stream:
                match = SUMMARY_RE.search(line)
                if not match:
                    continue
                row: dict[str, object] = match.groupdict()
                for field in INT_FIELDS:
                    row[field] = int(row[field])
                for field in FLOAT_FIELDS:
                    row[field] = float(row[field])
                row["source"] = str(path)
                rows.append(row)
    return rows


def latest_summary_per_run(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    latest: dict[tuple[str, int, int], dict[str, object]] = {}
    for row in rows:
        key = (str(row["source"]), int(row["alpha"]), int(row["beta"]))
        latest[key] = row
    return list(latest.values())


def infer_alpha_beta(path: Path) -> tuple[int | None, int | None]:
    match = RUN_NAME_RE.search(path.name)
    if match:
        return int(match.group("alpha")), int(match.group("beta"))
    match = RUN_NAME_RE.search(str(path.parent))
    if match:
        return int(match.group("alpha")), int(match.group("beta"))
    return None, None


def parse_pnf_late_logs(paths: Iterable[Path]) -> dict[tuple[int, int], dict[str, float]]:
    stats: dict[tuple[int, int], dict[str, float]] = {}
    for path in paths:
        alpha, beta = infer_alpha_beta(path)
        if alpha is None or beta is None:
            continue
        key = (alpha, beta)
        item = stats.setdefault(key, {"pnf_too_late_count": 0, "pnf_too_late_max_us": 0, "pnf_too_late_sum_us": 0})
        with path.open("r", encoding="utf-8", errors="ignore") as stream:
            for line in stream:
                match = PNF_LATE_CSV_RE.search(line) or PNF_TOO_LATE_RE.search(line)
                if not match:
                    continue
                late_us = int(match.group("late_us"))
                item["pnf_too_late_count"] += 1
                item["pnf_too_late_sum_us"] += late_us
                item["pnf_too_late_max_us"] = max(item["pnf_too_late_max_us"], late_us)
    for item in stats.values():
        count = item["pnf_too_late_count"]
        item["pnf_too_late_avg_us"] = item["pnf_too_late_sum_us"] / count if count else 0.0
    return stats


def attach_pnf_stats(rows: list[dict[str, object]], pnf_stats: dict[tuple[int, int], dict[str, float]]) -> None:
    for row in rows:
        key = (int(row["alpha"]), int(row["beta"]))
        item = pnf_stats.get(key, {})
        row["pnf_too_late_count"] = int(item.get("pnf_too_late_count", 0))
        row["pnf_too_late_max_us"] = int(item.get("pnf_too_late_max_us", 0))
        row["pnf_too_late_avg_us"] = float(item.get("pnf_too_late_avg_us", 0.0))
        row["score_with_pnf"] = (
            float(row["score"])
            + 1000.0 * int(row["pnf_too_late_count"])
            + 2.0 * float(row["pnf_too_late_avg_us"])
        )


def _minmax_norm(values: list[float]) -> list[float]:
    arr = np.array(values, dtype=float)
    mn = float(arr.min())
    mx = float(arr.max())
    if mx <= mn:
        return [0.0 for _ in values]
    return [float((v - mn) / (mx - mn)) for v in arr]


def attach_score_models(rows: list[dict[str, object]]) -> None:
    if not rows:
        return
    score = [float(r["score"]) for r in rows]
    late_count = [float(r["pnf_too_late_count"]) for r in rows]
    late_avg = [float(r["pnf_too_late_avg_us"]) for r in rows]
    adjust = [float(r["adjust"]) for r in rows]
    osc = [float(r["osc"]) for r in rows]
    avg_abs_late = [float(r["avg_abs_late"]) for r in rows]
    late_delay = [float(r["avg_late_to_up_delay"]) for r in rows]

    n_score = _minmax_norm(score)
    n_late_count = _minmax_norm(late_count)
    n_late_avg = _minmax_norm(late_avg)
    n_adjust = _minmax_norm(adjust)
    n_osc = _minmax_norm(osc)
    n_avg_abs_late = _minmax_norm(avg_abs_late)
    n_late_delay = _minmax_norm(late_delay)

    for idx, row in enumerate(rows):
        row["score_all_ones_raw"] = (
            float(row["score"]) + float(row["pnf_too_late_count"]) + float(row["pnf_too_late_avg_us"])
        )
        row["score_log_pnf"] = (
            float(row["score"])
            + 200.0 * float(np.log1p(float(row["pnf_too_late_count"])))
            + 0.5 * float(row["pnf_too_late_avg_us"])
        )
        row["score_norm_equal_3"] = n_score[idx] + n_late_count[idx] + n_late_avg[idx]

        severe_late = int(row["pnf_too_late_count"]) >= 100
        moderate_late = (int(row["pnf_too_late_count"]) > 0) and (not severe_late)
        safety_tier = 2 if severe_late else (1 if moderate_late else 0)
        row["safety_tier"] = safety_tier
        row["performance_score"] = (
            n_adjust[idx] + n_osc[idx] + n_avg_abs_late[idx] + n_late_delay[idx]
        )
        row["score_two_stage_hierarchical"] = 100.0 * safety_tier + float(row["performance_score"])


def write_csv(rows: list[dict[str, object]], output: Path) -> None:
    fields = [
        "alpha",
        "beta",
        "total",
        "up",
        "down",
        "stable",
        "adjust",
        "osc",
        "min_s",
        "max_s",
        "min_late",
        "max_late",
        "avg_abs_late",
        "stable_ratio",
        "late_risk",
        "late_reacted",
        "avg_late_to_up_delay",
        "max_late_to_up_delay",
        "pnf_too_late_count",
        "pnf_too_late_max_us",
        "pnf_too_late_avg_us",
        "score",
        "score_with_pnf",
        "score_all_ones_raw",
        "score_log_pnf",
        "score_norm_equal_3",
        "safety_tier",
        "performance_score",
        "score_two_stage_hierarchical",
        "source",
    ]
    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def plot_heatmap(rows: list[dict[str, object]], field: str, output: Path, title: str, cbar_label: str) -> None:
    import matplotlib.pyplot as plt
    import numpy as np

    alphas = sorted({int(row["alpha"]) for row in rows})
    betas = sorted({int(row["beta"]) for row in rows})
    data = np.full((len(betas), len(alphas)), np.nan)
    by_key = {(int(row["alpha"]), int(row["beta"])): row for row in rows}
    for y, beta in enumerate(betas):
        for x, alpha in enumerate(alphas):
            row = by_key.get((alpha, beta))
            if row:
                data[y, x] = float(row[field])

    plt.figure(figsize=(8, 6))
    image = plt.imshow(data, aspect="auto", origin="lower")
    plt.colorbar(image, label=cbar_label)
    plt.xticks(range(len(alphas)), alphas)
    plt.yticks(range(len(betas)), betas)
    plt.xlabel("Alpha denominator")
    plt.ylabel("Beta denominator")
    plt.title(title)
    if not np.isnan(data).all() and (8 in alphas and 4 in betas):
        plt.scatter([alphas.index(8)], [betas.index(4)], marker="x", s=100, c="white", linewidths=2)
    plt.tight_layout()
    plt.savefig(output, dpi=150)
    plt.close()


def plot_top_bars(rows: list[dict[str, object]], field: str, output: Path, title: str, ylabel: str) -> None:
    import matplotlib.pyplot as plt

    top = sorted(rows, key=lambda row: float(row["score_with_pnf"]))[:15]
    labels = [f"a=1/{row['alpha']}, b=1/{row['beta']}" for row in top]
    values = [float(row[field]) for row in top]
    plt.figure(figsize=(12, 5))
    colors = ["tab:orange" if row["alpha"] == 8 and row["beta"] == 4 else "tab:blue" for row in top]
    plt.bar(labels, values, color=colors)
    plt.xticks(rotation=45, ha="right")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.tight_layout()
    plt.savefig(output, dpi=150)
    plt.close()


def validate_png(path: Path) -> dict[str, object]:
    import matplotlib.image as mpimg

    image = mpimg.imread(path)
    return {
        "file": str(path),
        "exists": path.exists(),
        "bytes": path.stat().st_size if path.exists() else 0,
        "shape": list(image.shape),
        "pixel_std": float(image.std()),
        "non_blank": path.exists() and path.stat().st_size > 1000 and float(image.std()) > 0.0001,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vnf-log", action="append", default=[], help="VNF log file. Can be passed multiple times.")
    parser.add_argument("--pnf-log", action="append", default=[], help="PNF log file. Can be passed multiple times.")
    parser.add_argument("--log-dir", type=Path, help="Directory containing per-run logs.")
    parser.add_argument("--out-dir", type=Path, default=Path("ewma_results"))
    parser.add_argument(
        "--score-model",
        choices=[
            "legacy",
            "all_ones_raw",
            "log_pnf",
            "norm_equal_3",
            "two_stage_hierarchical",
        ],
        default="legacy",
        help="Scoring model to rank final alpha/beta sets.",
    )
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    vnf_logs = [Path(path) for path in args.vnf_log]
    pnf_logs = [Path(path) for path in args.pnf_log]
    if args.log_dir:
        vnf_logs.extend(sorted(args.log_dir.rglob("*VNF*.log")))
        pnf_logs.extend(sorted(args.log_dir.rglob("*PNF*.log")))

    rows = latest_summary_per_run(parse_summary_logs(vnf_logs))
    if not rows:
        raise RuntimeError("No P7_EWMA_SUMMARY rows found in VNF logs")

    attach_pnf_stats(rows, parse_pnf_late_logs(pnf_logs))
    attach_score_models(rows)

    score_field_by_model = {
        "legacy": "score_with_pnf",
        "all_ones_raw": "score_all_ones_raw",
        "log_pnf": "score_log_pnf",
        "norm_equal_3": "score_norm_equal_3",
        "two_stage_hierarchical": "score_two_stage_hierarchical",
    }
    active_score_field = score_field_by_model[args.score_model]
    rows = sorted(rows, key=lambda row: float(row[active_score_field]))

    csv_path = args.out_dir / "ewma_sweep_summary.csv"
    write_csv(rows, csv_path)

    plot_paths = [
        args.out_dir / "ewma_score_heatmap.png",
        args.out_dir / "ewma_score_with_pnf_heatmap.png",
        args.out_dir / "ewma_adjustment_count.png",
        args.out_dir / "ewma_oscillation_count.png",
    ]
    plot_heatmap(rows, "score", plot_paths[0], "EWMA Parameter Sweep Score", "Score, lower is better")
    plot_heatmap(
        rows,
        active_score_field,
        plot_paths[1],
        f"EWMA Score Heatmap ({args.score_model})",
        "Score, lower is better",
    )
    plot_top_bars(rows, "adjust", plot_paths[2], "Top EWMA Parameter Sets - Adjustment Count", "Adjustment count")
    plot_top_bars(rows, "osc", plot_paths[3], "Top EWMA Parameter Sets - Oscillation Count", "Oscillation count")

    validation = {
        "csv": {"file": str(csv_path), "rows": len(rows), "bytes": csv_path.stat().st_size},
        "plots": [validate_png(path) for path in plot_paths],
        "top10": rows[:10],
        "score_model": args.score_model,
        "active_score_field": active_score_field,
    }
    target_rank = next((idx + 1 for idx, row in enumerate(rows) if row["alpha"] == 8 and row["beta"] == 4), None)
    validation["target_alpha_8_beta_4_rank"] = target_rank
    validation_path = args.out_dir / "ewma_sweep_validation.json"
    validation_path.write_text(json.dumps(validation, indent=2), encoding="utf-8")

    print(f"Wrote {csv_path}")
    print(f"Wrote validation {validation_path}")
    print("Top 10 parameter sets:")
    for idx, row in enumerate(rows[:10], start=1):
        print(
            f"{idx:2d}. alpha=1/{row['alpha']} beta=1/{row['beta']} "
            f"{active_score_field}={float(row[active_score_field]):.2f} score={row['score']:.2f} "
            f"pnf_late={row['pnf_too_late_count']}"
        )
    if target_rank:
        print(f"Target alpha=1/8 beta=1/4 rank: {target_rank}")
    else:
        print("Target alpha=1/8 beta=1/4 not found")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
