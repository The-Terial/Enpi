from __future__ import annotations

from pathlib import Path
import math

import pandas as pd


ROOT = Path(__file__).resolve().parents[1]
INPUT = ROOT / "data" / "raw" / "metrel" / "Respaldo_Experimentacion_Enpi.xlsx"
OUTPUT_DIR = ROOT / "results" / "tables"

# Fallback rule from the original local scripts. The preferred source is the
# workbook column "I corregida", when available.
CURRENT_IN_AMPERE_SCENARIOS = {"003", "007"}


def as_num(series: pd.Series) -> pd.Series:
    return pd.to_numeric(series.replace({"---": None, "": None}), errors="coerce")


def metrics(reference: pd.Series, enpi: pd.Series) -> dict[str, float | int | None]:
    clean = pd.DataFrame({"reference": as_num(reference), "enpi": as_num(enpi)}).dropna()
    if clean.empty:
        return {"n": 0, "mae": None, "rmse": None, "mape_pct": None, "bias": None, "r": None, "r2": None}
    err = clean["enpi"] - clean["reference"]
    abs_err = err.abs()
    sq_err = err**2
    nonzero = clean["reference"] != 0
    ape = (err[nonzero] / clean.loc[nonzero, "reference"]).abs() * 100
    r = clean["reference"].corr(clean["enpi"]) if len(clean) > 1 else None
    return {
        "n": int(len(clean)),
        "mae": float(abs_err.mean()),
        "rmse": float(math.sqrt(sq_err.mean())),
        "mape_pct": float(ape.mean()) if not ape.empty else None,
        "bias": float(err.mean()),
        "r": float(r) if r is not None and not math.isnan(r) else None,
        "r2": float(r * r) if r is not None and not math.isnan(r) else None,
    }


def metrel_current_ampere(df: pd.DataFrame) -> pd.Series:
    if "I corregida" in df.columns:
        return as_num(df["I corregida"])
    current = as_num(df["I1(Med) [mA]"])
    scenario = df["Escenario"].astype(str).str.zfill(3)
    scale = scenario.isin(CURRENT_IN_AMPERE_SCENARIOS)
    return current.where(scale, current / 1000)


def read_validation_workbook(path: Path) -> pd.DataFrame:
    preview = pd.read_excel(path, sheet_name="Todos", header=None, nrows=3)
    header_row = 1 if preview.astype(str).eq("Escenario").any(axis=1).iloc[1] else 0
    df = pd.read_excel(path, sheet_name="Todos", header=header_row)
    df = df.dropna(how="all")
    return df


def build_tables() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    df = read_validation_workbook(INPUT)
    df["Escenario"] = df["Escenario"].astype(str).str.zfill(3)
    df["metrel_current_A"] = metrel_current_ampere(df)
    df["metrel_power_kW_derived"] = (
        as_num(df["U1(Med) [V]"]) * df["metrel_current_A"] * as_num(df["PF1cap+(ProAct) []"]) / 1000
    )

    variables = [
        ("Voltage", "V", df["U1(Med) [V]"], df["voltaje Enpi"]),
        ("Current", "A", df["metrel_current_A"], df["corriente Enpi"]),
        ("Frequency", "Hz", df["f(ProAct) [Hz]"], df["frecuencia Enpi"]),
        ("Power factor", "", df["PF1cap+(ProAct) []"], df["factor_potencia Enpi"]),
        ("Active power derived", "kW", df["metrel_power_kW_derived"], df["potencia_kw Enpi"]),
    ]

    rows = []
    for scenario, g in df.groupby("Escenario", sort=True):
        for name, unit, ref, enpi in variables:
            idx = g.index
            m = metrics(ref.loc[idx], enpi.loc[idx])
            rows.append({"scenario": scenario, "variable": name, "unit": unit, **m})
    for name, unit, ref, enpi in variables:
        rows.append({"scenario": "ALL", "variable": name, "unit": unit, **metrics(ref, enpi)})

    summary = pd.DataFrame(rows)
    summary.to_csv(OUTPUT_DIR / "metrel_enpi_metrics_by_scenario.csv", index=False)

    table9_vars = ["Voltage", "Current", "Frequency", "Power factor"]
    table9 = summary[summary["variable"].isin(table9_vars) & (summary["scenario"] != "ALL")].copy()
    table9_pivot = table9.pivot(index="scenario", columns="variable", values=["n", "mae", "mape_pct"])
    table9_pivot.to_csv(OUTPUT_DIR / "table9_reproduced_scenario_level_errors.csv")

    table10 = summary[summary["scenario"].eq("ALL") & summary["variable"].isin(table9_vars)].copy()
    table10.to_csv(OUTPUT_DIR / "table10_reproduced_global_agreement.csv", index=False)

    table11 = summary[summary["variable"].eq("Active power derived") & summary["scenario"].ne("ALL")].copy()
    table11.to_csv(OUTPUT_DIR / "table11_reproduced_active_power_derived.csv", index=False)

    qc = {
        "input_file": str(INPUT.relative_to(ROOT)),
        "rows_in_Todos": int(len(df)),
        "scenarios": sorted(df["Escenario"].dropna().unique().tolist()),
        "note": (
            "This script reproduces metrics from the available final experimental backup workbook. "
            "Pairwise non-numeric or missing values are excluded per variable before calculating metrics."
        ),
    }
    pd.Series(qc).to_json(OUTPUT_DIR / "reproduction_qc.json", indent=2)


if __name__ == "__main__":
    build_tables()
