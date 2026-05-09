# EWMA Alpha/Beta Scoring Rationale

## Problem

The previous score:

- `score_with_pnf = score + 1000 * pnf_too_late_count + 2 * pnf_too_late_avg_us`

can become dominated by `pnf_too_late_count` when some parameter sets generate thousands of late events.
That makes the heatmap mostly a "late-count map" and hides controller-quality differences.

## What We Need

A ranking that is:

- **Safety-aware** (catastrophic late behavior must be penalized)
- **Interpretable** (easy to explain in a report)
- **Not single-term dominated** (still shows adjustment/oscillation/reaction quality)

## Candidate Scoring Models

1. `legacy`
   - `score_with_pnf`
2. `all_ones_raw`
   - `score + pnf_too_late_count + pnf_too_late_avg_us`
3. `log_pnf`
   - `score + 200*log1p(pnf_too_late_count) + 0.5*pnf_too_late_avg_us`
4. `norm_equal_3`
   - min-max normalize `score`, `pnf_too_late_count`, `pnf_too_late_avg_us`, then equal-weight sum
5. `two_stage_hierarchical` (recommended)
   - Stage A (safety tier):
     - Tier 0: `pnf_too_late_count == 0`
     - Tier 1: `1 <= pnf_too_late_count < 100`
     - Tier 2: `pnf_too_late_count >= 100`
   - Stage B (within same tier): equal-weight normalized
     - `adjust`, `osc`, `avg_abs_late`, `avg_late_to_up_delay`

## Why Two-Stage Is Better

- It enforces operational reality: catastrophic late behavior cannot rank above safe behavior.
- It preserves detail among non-catastrophic candidates using controller-quality metrics.
- It avoids "all color is one penalty term" in the heatmap.

## Interpretation For This Run

Using `/home/hpe/ewma_sweep_runs/ewma_20260508_002431/analysis/ewma_sweep_summary.csv`:

- `alpha=1/8, beta=1/4` has:
  - zero PNF too-late count
  - low adjustment/oscillation
  - stable and safe behavior
- `alpha=1/16, beta=1/2` appears good in raw control score but has catastrophic PNF too-late count (thousands), so it should not be considered a production-safe winner.

This is exactly why raw or poorly balanced weighting can be misleading.

## Practical Recommendation

For report and future reproducibility:

- Default ranking model: `two_stage_hierarchical`
- Keep `legacy` and `all_ones_raw` as comparison baselines
- Always publish:
  - selected model
  - top-3 by selected model
  - top-3 by legacy model
  - PNF too-late distribution

This gives a transparent and defensible explanation of alpha/beta selection.
