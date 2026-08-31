# Data Dictionary

## `data/raw/metrel/Respaldo_Experimentacion_Enpi.xlsx`

Primary experimental backup workbook available locally for the Metrel/EnPI comparison.

Expected sheet:

- `Todos`: pooled paired readings by scenario. This sheet uses a two-row header layout; the second row contains the variable names used by the reproduction script.

Key columns used by `scripts/reproduce_metrel_metrics.py`:

- `Escenario`: validation scenario identifier. Values are normalized to three digits.
- `U1(Med) [V]`: Metrel voltage reference, in volts.
- `I1(Med) [mA]`: original Metrel current reference column.
- `I corregida`: Metrel current reference converted to amperes; used by the reproduction script when available.
- `f(ProAct) [Hz]`: Metrel frequency reference, in hertz.
- `PF1cap+(ProAct) []`: Metrel power factor reference.
- `voltaje Enpi`: EnPI voltage measurement.
- `corriente Enpi`: EnPI current measurement, in amperes.
- `frecuencia Enpi`: EnPI frequency measurement.
- `factor_potencia Enpi`: EnPI power factor measurement.
- `potencia_kw Enpi`: EnPI active power estimate, in kilowatts.

Derived columns created by the reproduction script:

- `metrel_current_A`: Metrel current converted to amperes according to the scenario rule above.
- `metrel_power_kW_derived`: active power derived as voltage x current x power factor / 1000.

## `data/raw/iot/lectura_iot.csv`

Raw IoT export available locally. It is included as evidence for traceability of the EnPI platform readings. Its exact role in the submitted Tables 9-11 should be verified against the final analysis workflow.

## `data/raw/metrel/mediciones_corregido.xlsx`

Earlier corrected workbook preserved as provenance. The final reproduction script does not use this file by default.

## `data/processed/*.xlsx`

Processed workbooks generated during prior analysis. They are preserved to document the transformation history and to help reviewers compare the submitted values with intermediate calculations.
