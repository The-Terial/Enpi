#!/usr/bin/env python3
"""Reproduce the accumulated-energy comparison reported in Table 8."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd


DEFAULT_INPUT = Path("data/raw/Metrel_y_Comparacion_V2.xlsx")
DEFAULT_OUTPUT = Path("results/tables/table8_accumulated_energy.csv")

EXPECTED = {
    "000": (577, 3, 0.123, 0.000),
    "001": (1202, 1, 21.045, 20.996),
    "002": (1191, 2, 12.010, 11.002),
    "003": (900, 1, 456.971, 451.996),
    "004": (1201, 1, 6.044, 5.997),
    "005": (1201, 1, 111.816, 108.993),
    "006": (1201, 1, 14.582, 14.000),
    "007": (1181, 3, 639.345, 630.012),
    "008": (900, 1, 0.000, 0.000),
    "009": (1042, 11, 367.947, 361.023),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--skip-paper-check",
        action="store_true",
        help="Write results without checking them against the manuscript table.",
    )
    return parser.parse_args()


def load_matched_data(path: Path) -> pd.DataFrame:
    raw = pd.read_excel(path, sheet_name="Calculos", header=1)
    required = {"Escenario", "Hora [UTC]", "Ep1+(Med) [mWh]", "Energia Enpi"}
    missing = sorted(required.difference(raw.columns))
    if missing:
        raise ValueError(f"Missing columns in sheet 'Calculos': {missing}")

    data = pd.DataFrame(
        {
            "scenario": pd.to_numeric(raw["Escenario"], errors="coerce"),
            "timestamp": pd.to_datetime(raw["Hora [UTC]"], errors="coerce"),
            "metrel_mWh": pd.to_numeric(raw["Ep1+(Med) [mWh]"], errors="coerce"),
            "enpi_kWh": pd.to_numeric(raw["Energia Enpi"], errors="coerce"),
        }
    ).dropna()
    data["scenario"] = data["scenario"].astype(int)
    return data.sort_values(["scenario", "timestamp"], kind="stable").reset_index(drop=True)


def calculate(data: pd.DataFrame) -> pd.DataFrame:
    rows: list[dict[str, object]] = []
    for scenario, group in data.groupby("scenario", sort=True):
        group = group.sort_values("timestamp", kind="stable").copy()
        group["segment"] = group["timestamp"].diff().dt.total_seconds().ne(1).cumsum()

        metrel_wh = group["metrel_mWh"].sum() / 1000.0
        enpi_wh = 0.0
        for _, segment in group.groupby("segment", sort=True):
            # EnPi stores cumulative energy in kWh. Register differences are
            # calculated only inside uninterrupted, timestamp-matched segments.
            enpi_wh += (segment["enpi_kWh"].iloc[-1] - segment["enpi_kWh"].iloc[0]) * 1000.0

        # The manuscript does not report a percentage when no load is present
        # or when the interval energy is below the 1 Wh EnPi register resolution.
        difference = (
            np.nan
            if metrel_wh < 1.0
            else 100.0 * (enpi_wh - metrel_wh) / metrel_wh
        )
        rows.append(
            {
                "scenario": f"{scenario:03d}",
                "matched_observations": len(group),
                "contiguous_segments": int(group["segment"].nunique()),
                "metrel_energy_Wh": metrel_wh,
                "enpi_change_Wh": enpi_wh,
                "difference_pct": difference,
            }
        )
    return pd.DataFrame(rows)


def check_against_paper(result: pd.DataFrame) -> None:
    if set(result["scenario"]) != set(EXPECTED):
        raise RuntimeError("Scenario set does not match Table 8.")
    for row in result.itertuples(index=False):
        n, segments, metrel, enpi = EXPECTED[row.scenario]
        if row.matched_observations != n or row.contiguous_segments != segments:
            raise RuntimeError(f"Count check failed for scenario {row.scenario}.")
        if not np.isclose(row.metrel_energy_Wh, metrel, atol=0.0005):
            raise RuntimeError(f"Metrel energy check failed for scenario {row.scenario}.")
        if not np.isclose(row.enpi_change_Wh, enpi, atol=0.0005):
            raise RuntimeError(f"EnPi energy check failed for scenario {row.scenario}.")


def main() -> None:
    args = parse_args()
    result = calculate(load_matched_data(args.input))
    if not args.skip_paper_check:
        check_against_paper(result)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result.to_csv(args.output, index=False, float_format="%.6f")
    print(result.to_string(index=False, float_format=lambda value: f"{value:.6f}"))
    print(f"\nWrote: {args.output}")


if __name__ == "__main__":
    main()
