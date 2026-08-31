# Repository Audit

## Included

| Category | Included evidence |
|---|---|
| Raw Metrel/EnPI workbooks | `data/raw/metrel/Respaldo_Experimentacion_Enpi.xlsx`, `data/raw/metrel/mediciones.xlsx`, `data/raw/metrel/mediciones_corregido.xlsx`, `data/raw/metrel/Lecturas_con_Metrel_Mi2883_trabajo.xlsx` |
| Raw IoT export | `data/raw/iot/lectura_iot.csv` |
| Processed workbooks | `data/processed/*.xlsx` |
| Reproducible analysis script | `scripts/reproduce_metrel_metrics.py` |
| Original analysis scripts | `scripts/*_original.py` |
| Reproduced tables | `results/tables/*.csv`, `results/tables/reproduction_qc.json` |
| Figures | `results/figures/*.png`, `manuscript/figures/*.png` |
| Manuscript files | `manuscript/Version_A1.tex`, corrected TeX, and reviewed PDF (36) |
| Integrity manifest | `MANIFEST.csv` with SHA-256 hashes |

## Excluded Due To GitHub File Size

- `mediciones_corregido_con_calculos.pdf` was not kept in the final GitHub-ready folder because the local copy is approximately 220 MB, exceeding GitHub's standard 100 MB per-file limit. The corresponding calculation workbook is included as `data/processed/mediciones_corregido_con_calculos.xlsx`.

## Verification Performed

- The portable script `scripts/reproduce_metrel_metrics.py` was executed successfully from the repository root.
- The script generated reproduced metric tables under `results/tables/` from `Respaldo_Experimentacion_Enpi.xlsx`.
- The package was scanned for common credential patterns.
- The full PHP/MySQL platform source was not copied because the local configuration contains real database credentials.

## Reviewer-Facing Limitations

1. The exact final exclusion/synchronization rule must be confirmed by the PI if it differs from pairwise non-missing numeric comparison.
2. The package supports validation and traceability reproduction; it is not a deployment-ready release of the SaaS platform.

## Recommended Before Public Release

1. Decide the repository license for scripts and data.
2. Rotate the database credentials found in the local platform project before publishing any web-application code.
3. Keep the GitHub repository public before submission if the manuscript cites the URL as data/code availability.
