# Reproducibility Package: EnPI IoT Validation

This repository accompanies the manuscript:

**A SaaS IoT Architecture for Traceable Energy Performance Indicators and Distributed Machine-Level Monitoring**

It provides the available raw data, processed workbooks, analysis scripts, figures, and manuscript files used to support the metrological validation and SME traceability case reported in the paper.

## Repository Contents

```text
data/
  raw/
    metrel/          Raw, corrected, and final backup Metrel/EnPI workbooks available locally
    iot/             IoT readings exported from the EnPI system
  processed/         Processed spreadsheets used for indicator and error calculations

scripts/
  reproduce_metrel_metrics.py
  *_original.py      Original local scripts preserved for provenance

results/
  tables/            Reproduced CSV tables and exported calculation workbooks
  figures/           Figures derived from the available validation outputs

manuscript/
  Version_A1.tex
  Version_A1_corrigida_claims_reproducibility.tex
  *.pdf
  figures/

docs/
  data_dictionary.md
  reproducibility_notes.md
  security_note.md
```

## Quick Reproduction

From the repository root:

```bash
python3 -m pip install -r requirements.txt
python3 scripts/reproduce_metrel_metrics.py
```

The script reads:

```text
data/raw/metrel/Respaldo_Experimentacion_Enpi.xlsx
```

and writes:

```text
results/tables/metrel_enpi_metrics_by_scenario.csv
results/tables/table9_reproduced_scenario_level_errors.csv
results/tables/table10_reproduced_global_agreement.csv
results/tables/table11_reproduced_active_power_derived.csv
results/tables/reproduction_qc.json
```

## Important Reproducibility Note

The final experimental backup workbook contains **10,596 data rows** in sheet `Todos` when read using the second row as the variable header. The reproduction script uses that file as the default input.

## Data Availability Statement For Manuscript

Suggested wording:

> The raw and processed data supporting the metrological validation and SME traceability case, together with the analysis scripts required to reproduce the reported error metrics, are available at: https://github.com/The-Terial/Enpi

## Scientific Boundaries

This package supports the component-level validation and case-level traceability analysis described in the manuscript. It does not, by itself, establish full industrial-scale platform validation, cybersecurity certification, high-availability SaaS benchmarking, or long-term multi-site deployment robustness.

## Contact

For questions about the dataset, exclusions, or exact manuscript version, contact the corresponding author listed in the manuscript.
