#!/usr/bin/env python3
"""Reproduce the active-power moving-block bootstrap reported in the paper.

The implementation resamples paired EnPi/Metrel observations within contiguous
time segments. This preserves short-range serial dependence without bridging
telemetry gaps.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import pandas as pd


DEFAULT_INPUT = Path("data/raw/Metrel_y_Comparacion_V2.xlsx")
DEFAULT_OUTPUT = Path("results/bootstrap")
LOADED_SCENARIOS = (1, 2, 3, 4, 5, 6, 7, 9)
EXPECTED_N = 9_119
EXPECTED_METRICS = {
    "mae_W": 11.40770577914245,
    "rmse_W": 23.184323104931565,
    "bias_W": -11.033755565303213,
    "wmape_pct": 1.7730710096443942,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--replicates", type=int, default=2_000)
    parser.add_argument("--block-lengths", type=int, nargs="+", default=[15, 30, 60, 120])
    parser.add_argument("--primary-block", type=int, default=60)
    parser.add_argument("--seed-base", type=int, default=20_261_013)
    parser.add_argument(
        "--skip-paper-check",
        action="store_true",
        help="Allow execution with data that do not reproduce the published sample and point estimates.",
    )
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_paired_power(path: Path) -> pd.DataFrame:
    raw = pd.read_excel(path, sheet_name="Calculos", header=1)
    required = {"Escenario", "Hora [UTC]", "P1+(Med) [W]", "Potengia Enpi"}
    missing = sorted(required.difference(raw.columns))
    if missing:
        raise ValueError(f"Missing required columns in sheet 'Calculos': {missing}")

    data = pd.DataFrame(
        {
            "scenario": pd.to_numeric(raw["Escenario"], errors="coerce"),
            "timestamp": pd.to_datetime(raw["Hora [UTC]"], errors="coerce"),
            "metrel_power_W": pd.to_numeric(raw["P1+(Med) [W]"], errors="coerce"),
            # The workbook stores EnPi active power in kW.
            "enpi_power_W": pd.to_numeric(raw["Potengia Enpi"], errors="coerce") * 1000.0,
        }
    )
    data = data[data["scenario"].isin(LOADED_SCENARIOS)].dropna().copy()
    data["scenario"] = data["scenario"].astype(int)
    data = data.sort_values(["scenario", "timestamp"], kind="stable").reset_index(drop=True)
    data["error_W"] = data["enpi_power_W"] - data["metrel_power_W"]
    return data


def point_metrics(data: pd.DataFrame | np.ndarray) -> dict[str, float]:
    if isinstance(data, pd.DataFrame):
        reference = data["metrel_power_W"].to_numpy(float)
        observed = data["enpi_power_W"].to_numpy(float)
    else:
        reference = data[:, 0]
        observed = data[:, 1]
    error = observed - reference
    return {
        "mae_W": float(np.mean(np.abs(error))),
        "rmse_W": float(np.sqrt(np.mean(error**2))),
        "bias_W": float(np.mean(error)),
        "wmape_pct": float(100.0 * np.sum(np.abs(error)) / np.sum(np.abs(reference))),
    }


def split_contiguous_segments(data: pd.DataFrame) -> tuple[list[np.ndarray], pd.DataFrame]:
    segments: list[np.ndarray] = []
    metadata: list[dict[str, object]] = []
    segment_id = 0
    for scenario, group in data.groupby("scenario", sort=True):
        group = group.sort_values("timestamp", kind="stable").copy()
        # A new segment begins whenever timestamps are not exactly one second apart.
        local_ids = group["timestamp"].diff().dt.total_seconds().ne(1).cumsum()
        for _, segment in group.groupby(local_ids, sort=True):
            segment_id += 1
            array = segment[["metrel_power_W", "enpi_power_W"]].to_numpy(float)
            segments.append(array)
            metadata.append(
                {
                    "segment_id": segment_id,
                    "scenario": f"{scenario:03d}",
                    "start_utc": segment["timestamp"].iloc[0].isoformat(sep=" "),
                    "end_utc": segment["timestamp"].iloc[-1].isoformat(sep=" "),
                    "n_observations": len(segment),
                }
            )
    return segments, pd.DataFrame(metadata)


def circular_block_sample(segment: np.ndarray, block_length: int, rng: np.random.Generator) -> np.ndarray:
    n = len(segment)
    blocks: list[np.ndarray] = []
    sampled = 0
    offsets = np.arange(block_length)
    while sampled < n:
        start = int(rng.integers(0, n))
        block = segment[(start + offsets) % n]
        blocks.append(block)
        sampled += len(block)
    return np.concatenate(blocks, axis=0)[:n]


def bootstrap(
    segments: list[np.ndarray], block_length: int, replicates: int, seed: int
) -> pd.DataFrame:
    rng = np.random.default_rng(seed)
    rows = []
    for replicate in range(1, replicates + 1):
        sample = np.concatenate(
            [circular_block_sample(segment, block_length, rng) for segment in segments], axis=0
        )
        rows.append(
            {
                "block_length": block_length,
                "seed": seed,
                "replicate": replicate,
                **point_metrics(sample),
            }
        )
    return pd.DataFrame(rows)


def check_against_paper(data: pd.DataFrame, metrics: dict[str, float]) -> None:
    if len(data) != EXPECTED_N:
        raise RuntimeError(f"Expected {EXPECTED_N} loaded pairs, found {len(data)}.")
    for name, expected in EXPECTED_METRICS.items():
        if not np.isclose(metrics[name], expected, rtol=0, atol=1e-10):
            raise RuntimeError(f"Published {name} check failed: expected {expected}, found {metrics[name]}.")


def main() -> None:
    args = parse_args()
    if args.replicates < 1:
        raise ValueError("--replicates must be positive")
    if any(length < 1 for length in args.block_lengths):
        raise ValueError("All block lengths must be positive")
    if args.primary_block not in args.block_lengths:
        raise ValueError("--primary-block must also appear in --block-lengths")

    input_path = args.input.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    data = load_paired_power(input_path)
    observed = point_metrics(data)
    if not args.skip_paper_check:
        check_against_paper(data, observed)

    segments, segment_table = split_contiguous_segments(data)
    replicate_tables = []
    summary_rows = []
    metric_names = tuple(EXPECTED_METRICS)

    for block_length in args.block_lengths:
        seed = args.seed_base + block_length
        replicates = bootstrap(segments, block_length, args.replicates, seed)
        replicate_tables.append(replicates)
        row: dict[str, float | int | bool] = {
            "block_length": block_length,
            "primary_analysis": block_length == args.primary_block,
            "replicates": args.replicates,
            "seed": seed,
            "n_observations": len(data),
            "n_segments": len(segments),
        }
        for metric in metric_names:
            lower, upper = np.percentile(replicates[metric], [2.5, 97.5])
            row[f"observed_{metric}"] = observed[metric]
            row[f"ci95_lower_{metric}"] = float(lower)
            row[f"ci95_upper_{metric}"] = float(upper)
        summary_rows.append(row)

    all_replicates = pd.concat(replicate_tables, ignore_index=True)
    summary = pd.DataFrame(summary_rows)
    data.to_csv(output_dir / "active_power_analysis_pairs.csv", index=False)
    segment_table.to_csv(output_dir / "active_power_segments.csv", index=False)
    all_replicates.to_csv(output_dir / "active_power_bootstrap_replicates.csv", index=False)
    summary.to_csv(output_dir / "active_power_bootstrap_summary.csv", index=False)

    manifest = {
        "method": "segment-preserving circular moving-block bootstrap",
        "input_file": args.input.as_posix(),
        "input_sha256": sha256(input_path),
        "input_sheet": "Calculos",
        "loaded_scenarios": [f"{value:03d}" for value in LOADED_SCENARIOS],
        "n_observations": len(data),
        "n_segments": len(segments),
        "replicates_per_block_length": args.replicates,
        "block_lengths": args.block_lengths,
        "primary_block_length": args.primary_block,
        "seed_rule": "seed_base + block_length",
        "seed_base": args.seed_base,
        "confidence_interval": "2.5th and 97.5th percentiles",
        "observed_metrics": observed,
        "paper_consistency_check": not args.skip_paper_check,
        "software": {"python": "3.10+", "numpy": np.__version__, "pandas": pd.__version__},
    }
    (output_dir / "active_power_bootstrap_manifest.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8"
    )

    print(summary.to_string(index=False))
    print(f"\nWrote reproducibility outputs to: {output_dir}")


if __name__ == "__main__":
    main()
