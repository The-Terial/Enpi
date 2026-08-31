# Reproducibility Notes

## Scope

This package was assembled to support the IEEE Access data/code availability statement for the manuscript. It focuses on the metrological comparison between EnPI and the Metrel MI 2883 reference instrument, plus supporting material for the SME traceability case.

## Current Verification Status

- The repository includes raw and processed workbooks available locally.
- The repository includes original calculation scripts for provenance.
- The repository includes a portable reproduction script with relative paths.
- The script successfully generates CSV summaries for scenario-level and global error metrics.
- The final experimental backup workbook yields 10,596 data rows in sheet `Todos` when read with the second row as header.
- This resolves the previous mismatch with the manuscript-reported 10,596 paired samples.

## Open Verification Item

Before submission, the PI should verify whether the manuscript tables use exactly the same pairwise missing-value handling implemented in `scripts/reproduce_metrel_metrics.py`.

## Exclusion Criteria

No additional exclusion criteria were inferred beyond dropping missing/non-numeric paired values per variable in `scripts/reproduce_metrel_metrics.py`.

If the manuscript used additional exclusions, thresholds, filtering windows, synchronization rules, or scenario-specific cleaning decisions, add them here and encode them in the script.

INFORMATION REQUIRED FROM PI:

- Final exclusion/synchronization criteria, if different from pairwise non-missing numeric comparison.
- Confirmation that `I corregida` is the intended Metrel current reference in amperes.

## Original Scripts

The files named `*_original.py` are preserved for provenance. Some use local absolute paths from the PI computer and are not intended as the primary reproduction interface. The portable script is:

```text
scripts/reproduce_metrel_metrics.py
```
