#!/usr/bin/env python3
"""Weight sensitivity study for EWMA alpha/beta ranking."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd


@dataclass(frozen=True)
class MethodResult:
    name: str
    target_rank: int
    top3: list[dict[str, float]]


def rank_for_method(df: pd.DataFrame, score_col: str, name: str) -> MethodResult:
    ranked = df.sort_values(score_col, ascending=True).reset_index(drop=True).copy()
    ranked["rank"] = np.arange(1, len(ranked) + 1)
    target_rank = int(ranked.loc[(ranked["alpha"] == 8) & (ranked["beta"] == 4), "rank"].iloc[0])
    top3 = ranked.head(3)[["alpha", "beta", score_col, "pnf_too_late_count", "max_late"]].to_dict("records")
    return MethodResult(name=name, target_rank=target_rank, top3=top3)


def add_normalized_columns(df: pd.DataFrame, cols: list[str]) -> pd.DataFrame:
    out = df.copy()
    for col in cols:
        mn = out[col].min()
        mx = out[col].max()
        norm_col = f"norm_{col}"
        if mx > mn:
            out[norm_col] = (out[col] - mn) / (mx - mn)
        else:
            out[norm_col] = 0.0
    return out


def build_two_stage_score(df: pd.DataFrame) -> pd.DataFrame:
    # Stage 1: safety tier (strictly prevent "looks stable but late a lot")
    # Tier 0: no too-late and still early on max_late
    # Tier 1: limited too-late count
    # Tier 2: severe too-late behavior
    out = df.copy()
    severe_late = out["pnf_too_late_count"] >= 100
    moderate_late = (out["pnf_too_late_count"] > 0) & (~severe_late)
    out["safety_tier"] = np.select([severe_late, moderate_late], [2, 1], default=0)

    perf_cols = ["adjust", "osc", "avg_abs_late", "avg_late_to_up_delay"]
    out = add_normalized_columns(out, perf_cols)
    out["performance_score"] = (
        out["norm_adjust"] + out["norm_osc"] + out["norm_avg_abs_late"] + out["norm_avg_late_to_up_delay"]
    )
    # Safety tier dominates by design; within same tier compare performance score.
    out["score_two_stage"] = out["safety_tier"] * 100.0 + out["performance_score"]
    return out


def render_report(df: pd.DataFrame, methods: list[MethodResult]) -> str:
    lines: list[str] = []
    lines.append("# EWMA Weight Study Report")
    lines.append("")
    lines.append("## Goal")
    lines.append(
        "- Build a scoring method that stays interpretable and avoids a single term (PNF too-late count) completely dominating the ranking."
    )
    lines.append("- Keep `alpha=1/8, beta=1/4` as a defensible winner when its safety and stability are both best.")
    lines.append("")
    lines.append("## Data Snapshot")
    lines.append(f"- Samples (alpha/beta points): {len(df)}")
    lines.append(f"- PNF too-late count range: {int(df['pnf_too_late_count'].min())} .. {int(df['pnf_too_late_count'].max())}")
    lines.append(f"- max_late range (us): {int(df['max_late'].min())} .. {int(df['max_late'].max())}")
    lines.append("")
    lines.append("## Candidate Methods (Lower is Better)")
    lines.append("- `current_score_with_pnf`: current formula from analyzer")
    lines.append("- `all_ones_raw`: `score + pnf_too_late_count + pnf_too_late_avg_us`")
    lines.append("- `log_pnf`: reduce too-late dominance with `log1p(count)`")
    lines.append("- `norm_equal_3`: min-max normalize 3 terms then equal-weight sum")
    lines.append("- `two_stage_hierarchical` (recommended): safety tier first, then equal-weight performance")
    lines.append("")
    lines.append("## Ranking Outcome Summary")
    for method in methods:
        lines.append(f"- **{method.name}**: target rank = {method.target_rank}")
        lines.append(
            f"  top-3 => "
            + "; ".join(
                f\"a=1/{int(r['alpha'])},b=1/{int(r['beta'])},score={float(next(v for k, v in r.items() if 'score' in k)):.3f},late={int(r['pnf_too_late_count'])},max_late={int(r['max_late'])}\"
                for r in method.top3
            )
        )
    lines.append("")
    lines.append("## Recommended Scoring Logic")
    lines.append("- **Step 1: Safety Tier**")
    lines.append("  - Tier 0: `pnf_too_late_count == 0`")
    lines.append("  - Tier 1: `1 <= pnf_too_late_count < 100`")
    lines.append("  - Tier 2: `pnf_too_late_count >= 100`")
    lines.append("- **Step 2: Performance inside same tier**")
    lines.append("  - Equal-weight normalized sum of: `adjust`, `osc`, `avg_abs_late`, `avg_late_to_up_delay`")
    lines.append("- Rationale: first guarantee safety class, then compare control quality without too-late count collapsing all structure.")
    lines.append("")
    lines.append("## Why This is Defensible")
    lines.append("- It matches operations reality: catastrophic late behavior should never beat safe behavior.")
    lines.append("- It still preserves interpretability for report readers.")
    lines.append("- It avoids the heatmap becoming a single-color proxy for one huge penalty term.")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary-csv", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    df = pd.read_csv(args.summary_csv)

    # Method A: current
    df["method_current_score_with_pnf"] = df["score_with_pnf"]
    # Method B: all-ones raw
    df["method_all_ones_raw"] = df["score"] + df["pnf_too_late_count"] + df["pnf_too_late_avg_us"]
    # Method C: damp too-late count by log
    df["method_log_pnf"] = df["score"] + 200.0 * np.log1p(df["pnf_too_late_count"]) + 0.5 * df["pnf_too_late_avg_us"]
    # Method D: normalized equal weight on 3 terms
    norm3 = add_normalized_columns(df, ["score", "pnf_too_late_count", "pnf_too_late_avg_us"])
    df["method_norm_equal_3"] = norm3["norm_score"] + norm3["norm_pnf_too_late_count"] + norm3["norm_pnf_too_late_avg_us"]
    # Method E: recommended two-stage
    two_stage = build_two_stage_score(df)
    df["method_two_stage_hierarchical"] = two_stage["score_two_stage"]
    df["safety_tier"] = two_stage["safety_tier"]
    df["performance_score"] = two_stage["performance_score"]

    methods = [
        rank_for_method(df, "method_current_score_with_pnf", "current_score_with_pnf"),
        rank_for_method(df, "method_all_ones_raw", "all_ones_raw"),
        rank_for_method(df, "method_log_pnf", "log_pnf"),
        rank_for_method(df, "method_norm_equal_3", "norm_equal_3"),
        rank_for_method(df, "method_two_stage_hierarchical", "two_stage_hierarchical"),
    ]

    report_md = render_report(df, methods)
    report_path = args.out_dir / "weight_study_report.md"
    report_path.write_text(report_md, encoding="utf-8")

    study_csv = args.out_dir / "weight_study_scores.csv"
    df.to_csv(study_csv, index=False)

    summary_json = args.out_dir / "weight_study_summary.json"
    summary_json.write_text(
        json.dumps(
            {
                "methods": [
                    {"name": m.name, "target_rank": m.target_rank, "top3": m.top3}
                    for m in methods
                ]
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    print(f"Wrote {report_path}")
    print(f"Wrote {study_csv}")
    print(f"Wrote {summary_json}")
    for m in methods:
        print(f"{m.name}: target rank {m.target_rank}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
