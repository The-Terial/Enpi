# Active-Power Bootstrap Reproducibility

## Purpose

This analysis quantifies uncertainty around the pooled active-power agreement
metrics reported for the loaded scenarios. It uses the 9,119 timestamp-matched
Metrel/EnPi observations from scenarios `001`-`007` and `009`. Idle scenario
`000` and zero-current scenario `008` are outside this pooled loaded analysis.

## Why observations are segmented

The retained observations contain 21 contiguous one-second segments. A new
segment starts whenever adjacent timestamps are not exactly one second apart.
The bootstrap never draws a block across those boundaries, so the 210 seconds
without server-side EnPi records are not treated as observed continuity.

## Method

- Segment-preserving circular moving-block bootstrap.
- 2,000 replicates for each block length.
- Block lengths: 15, 30, 60, and 120 observations.
- Primary analysis: block length 60.
- Random seed: `20261013 + block_length`.
- Confidence intervals: empirical 2.5th and 97.5th percentiles.
- Metrics: MAE, RMSE, signed bias (`EnPi - Metrel`), and WMAPE.

Within each replicate, every contiguous segment is resampled independently by
drawing circular blocks until the original segment length is reached. The
resampled segments are then pooled, preserving the number of observations in
each segment and scenario.

## Exact command

Run from the repository root:

```bash
python3 scripts/bootstrap_active_power.py
```

The script checks that the input reproduces the manuscript's 9,119 observations
and point estimates before generating any inferential output. Use
`--skip-paper-check` only for explicitly exploratory analyses on modified data.

## Outputs

```text
results/bootstrap/active_power_analysis_pairs.csv
results/bootstrap/active_power_segments.csv
results/bootstrap/active_power_bootstrap_replicates.csv
results/bootstrap/active_power_bootstrap_summary.csv
results/bootstrap/active_power_bootstrap_manifest.json
```

The manifest includes the input workbook SHA-256 digest, parameters, package
versions, seed rule, and observed point estimates.
