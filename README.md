# EnPi: Reproducibility Repository

This repository accompanies the manuscript:

**Experimental Characterization of a Low-Cost IoT Electrical Monitoring System for Traceable Machine-Level Energy Performance Indicators**

It contains the final manuscript and supplement, the validated EnPi--Metrel comparison workbook, processed datasets, executable analysis scripts, and the outputs used to support the reported experimental characterization.

## Repository Structure

```text
data/
  raw/                 Validated source workbook
  processed/           Derived workbooks retained for traceability
docs/                   Method and data documentation
manuscript/             Current article, supplement, and referenced figures
results/
  bootstrap/            Moving-block bootstrap outputs
  figures/              Reproduced analysis figures
  tables/               Reproduced numerical tables
scripts/
  bootstrap_active_power.py
  reproduce_table8_energy.py
  legacy/               Earlier scripts retained only for provenance
```

## Environment

Python 3.10 or later is recommended.

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
```

## Reproduce the Main Analyses

Run the commands from the repository root:

```bash
python3 scripts/reproduce_table8_energy.py
python3 scripts/bootstrap_active_power.py
```

The first command reproduces the accumulated-energy comparison in Table 8 and writes:

```text
results/tables/table8_accumulated_energy.csv
```

The second command reproduces the segment-preserving circular moving-block bootstrap for active power using 2,000 replicates and block lengths of 15, 30, 60, and 120 observations. It writes the paired analysis data, segment table, bootstrap replicates, summary, and machine-readable manifest to `results/bootstrap/`.

Both scripts contain consistency checks against the values reported in the manuscript. They terminate with an error if the validated input no longer reproduces the published sample sizes or point estimates.

## Validated Input

The authoritative analysis input is:

```text
data/raw/Metrel_y_Comparacion_V2.xlsx
```

The `Calculos` worksheet contains 10,596 timestamp-matched observations across scenarios 000--009. Active-power bootstrap inference uses 9,119 paired observations from nonzero-load scenarios 001--007 and 009.

The continuity audit identified 210 missing one-second EnPi records relative to the reference timeline: 20 in scenario 000, 10 in 002, 20 in 007, and 160 in 009. Missing observations are not interpolated.

## Table 8 Method

Table 8 uses only timestamp-matched EnPi--Metrel observations. For each scenario, observations are sorted by timestamp and divided into contiguous one-second segments. Metrel interval energy is summed over retained observations. EnPi cumulative-register changes are calculated independently inside each segment and then summed, preventing the cumulative counter from spanning unmatched intervals.

| Scenario | Matched observations | Segments | Metrel (Wh) | EnPi (Wh) | Difference (%) |
|---:|---:|---:|---:|---:|---:|
| 000 | 577 | 3 | 0.123 | 0.000 | N/A, below resolution |
| 001 | 1202 | 1 | 21.045 | 20.996 | -0.235 |
| 002 | 1191 | 2 | 12.010 | 11.002 | -8.397 |
| 003 | 900 | 1 | 456.971 | 451.996 | -1.089 |
| 004 | 1201 | 1 | 6.044 | 5.997 | -0.772 |
| 005 | 1201 | 1 | 111.816 | 108.993 | -2.525 |
| 006 | 1201 | 1 | 14.582 | 14.000 | -3.991 |
| 007 | 1181 | 3 | 639.345 | 630.012 | -1.460 |
| 008 | 900 | 1 | 0.000 | 0.000 | N/A, no load |
| 009 | 1042 | 11 | 367.947 | 361.023 | -1.882 |

## Manuscript Files

- `manuscript/main_paper.tex` and `manuscript/main_paper.pdf`
- `manuscript/suplementary.tex` and `manuscript/suplementary.pdf`

The `Definitions/` directory and all figures referenced by these TeX sources are included, allowing the two documents to be compiled from the `manuscript/` directory.

## Scope

The repository supports the bounded single-phase experimental characterization, active-power bootstrap analysis, accumulated-energy comparison, and operational EnPI demonstration described in the manuscript. It does not establish billing-grade accuracy, regulatory conformity, electrical-safety certification, industrial cybersecurity certification, or general performance beyond the tested conditions.

## Data Availability

The public repository location referenced by the manuscript is:

https://github.com/The-Terial/Enpi

## Integrity

`SHA256SUMS.txt` records the SHA-256 digest of every distributed file. It can be checked on macOS with:

```bash
shasum -a 256 -c SHA256SUMS.txt
```

No software license has been added automatically. Select and add the intended repository license before granting reuse permissions beyond the manuscript's data-availability purpose.
