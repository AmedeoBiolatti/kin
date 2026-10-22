#!/usr/bin/env python3
"""Compare reports emitted by Kin's <game>_package targets (standard library only)."""

import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", action="append", nargs=2, metavar=("LABEL", "PATH"), required=True)
    parser.add_argument("--output", type=Path, help="Also save exact bytes and deltas as JSON")
    args = parser.parse_args()
    rows = []
    identity = None
    for label, path in args.report:
        try:
            report = json.loads(Path(path).read_text(encoding="utf-8"))
            if report["schema"] != "kin.package-size/1":
                raise ValueError("unsupported size report schema")
            key = tuple(report[field] for field in ("target", "system", "architecture"))
            if identity is not None and key != identity:
                raise ValueError("compare the same game, OS and architecture")
            identity = key
            metrics = {name: report[name] for name in (
                "build_executable_bytes", "packaged_executable_bytes", "uncompressed_bytes", "archive_bytes"
            )}
            if any(type(value) is not int or value <= 0 for value in metrics.values()):
                raise ValueError("size metrics must be positive integer byte counts")
        except (OSError, ValueError, KeyError, TypeError) as error:
            parser.error(f"{path}: {error}")
        baseline = rows[0]["bytes"] if rows else metrics
        rows.append({
            "label": label, "report": str(Path(path)), "config": report["config"],
            "compiler": report["compiler"], "compiler_version": report["compiler_version"],
            "features": report["features"], "optimizations": report["optimizations"],
            "bytes": metrics, "categories": report["categories"],
            "delta_bytes": {name: value - baseline[name] for name, value in metrics.items()},
            "delta_percent": {name: 100 * (value / baseline[name] - 1) for name, value in metrics.items()},
        })
    print("| Build | Executable MiB | Installed MiB | ZIP MiB | ZIP change |")
    print("|---|---:|---:|---:|---:|")
    for row in rows:
        values = [row["bytes"][key] / (1024 * 1024) for key in (
            "packaged_executable_bytes", "uncompressed_bytes", "archive_bytes")]
        print(f"| {row['label']} | {values[0]:.3f} | {values[1]:.3f} | {values[2]:.3f} | "
              f"{row['delta_percent']['archive_bytes']:+.1f}% |")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({
            "schema": "kin.package-comparison/1", "target": identity[0],
            "system": identity[1], "architecture": identity[2], "builds": rows,
        }, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
