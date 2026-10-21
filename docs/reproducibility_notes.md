# Reproducibility Notes

## Verified Analysis State

- The validated workbook contains 10,596 timestamp-matched observations.
- The loaded active-power bootstrap analysis contains 9,119 observations in 21 contiguous segments.
- The continuity audit identifies 210 absent one-second EnPi records relative to the reference timeline.
- Table 8 is reproduced from timestamp-matched records only.
- Missing records are not interpolated.
- EnPi cumulative-energy differences are calculated independently inside each contiguous segment.
- The primary moving-block bootstrap uses 2,000 replicates and a block length of 60 observations; 15, 30, and 120 observations are sensitivity analyses.

## Primary Scripts

```text
scripts/reproduce_table8_energy.py
scripts/bootstrap_active_power.py
```

Both scripts use relative paths, write machine-readable outputs, and check their results against the values reported in the manuscript.

## Legacy Scripts

The files under `scripts/legacy/` are retained only to document earlier stages of the analysis. Some refer to source files or local paths that are not part of this clean public package. They are not the reproduction interface for the current manuscript.

## Evidence Boundary

The scripts reproduce the numerical analyses represented in this repository. They do not independently verify electrical-safety certification, regulatory conformity, billing-grade metrology, cybersecurity, or performance outside the tested single-phase conditions.
