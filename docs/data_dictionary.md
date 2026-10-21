# Data Dictionary

## Authoritative Workbook

`data/raw/Metrel_y_Comparacion_V2.xlsx` is the validated source used by the executable analyses.

### `Calculos`

Contains 10,596 timestamp-matched EnPi--Metrel observations across scenarios 000--009.

- `Escenario`: test-scenario identifier.
- `Hora [UTC]`: paired timestamp at one-second resolution.
- `U1(Med) [V]`: Metrel mean voltage, V.
- `I1(Med) [mA]`: Metrel current channel; scenario-specific units are documented in the source workbook.
- `f(ProAct) [Hz]`: Metrel frequency, Hz.
- `Ep1+(Med) [mWh]`: Metrel interval active energy, mWh.
- `P1+(Med) [W]`: Metrel active power, W.
- `PF Metrel`: selected Metrel power-factor value.
- `Voltaje Enpi`: EnPi voltage, V.
- `Corriente Enpi`: EnPi current, A.
- `Potengia Enpi`: EnPi active power, kW. The misspelling is retained because it is the source column name.
- `Energia Enpi`: EnPi cumulative active-energy register, kWh.
- `Frecuencia Enpi`: EnPi frequency, Hz.
- `Factor de Potencia Enpi`: EnPi power factor.

The remaining columns contain workbook calculations supporting absolute error, relative error, signed error, squared error, and aggregate metrics.

### `Enpi`

Server-side EnPi export containing reading identifiers, meter code, timestamps, electrical variables, the original payload, and creation time.

### `000`--`009`

Reference-instrument observations for each controlled load scenario.

### Other Worksheets

`Resumen`, `Resumen Calculo`, and `Limites` retain author summaries and calculation context. They are not the direct input used by the two primary Python scripts.

## Processed Workbooks

- `data/processed/Lecturas_con_Metrel_Mi2883_metricas.xlsx`
- `data/processed/mediciones_con_indicadores.xlsx`

These files preserve intermediate calculations and are included for traceability. The authoritative scripted results are generated from the raw validated workbook.

