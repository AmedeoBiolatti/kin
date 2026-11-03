#!/usr/bin/env python3
"""Alternate native Yard CPU-cache controls and check work/pixel parity."""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def summarize(rows):
    result = {}
    for key in rows[0]:
        values = sorted(float(row[key]) for row in rows)
        result[key] = {"p50": statistics.median(values), "p95": values[int(.95 * len(values))]}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--recipe", type=Path)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--frames", type=int, default=1440)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--cpus", default="0-15")
    parser.add_argument("--mode", choices=["direct", "local-gather"], default="direct")
    parser.add_argument("--skip-captures", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.repeats <= 20 or not 1 <= args.frames <= 6000 or not 0 <= args.workers <= 4:
        parser.error("repeats/frames/workers outside the bounded benchmark profile")
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    repo = Path(__file__).resolve().parents[1]
    recipe = args.recipe.resolve() if args.recipe else root / "medium.yrd"
    if not args.recipe:
        subprocess.run(["python3", str(repo / "examples/industrial_yard/generate.py"), str(recipe)],
                       check=True, stdout=subprocess.DEVNULL)
    env = dict(os.environ, KIN_RENDER_BACKEND="gpu", KIN_REQUIRE_GPU_TESTS="1")
    runs = []

    def run(enabled, repeat, frames, offset=0, capture=False):
        label = f"{'capture' if capture else 'run'}-{repeat}-{'on' if enabled else 'off'}"
        command = [str(binary), "--recipe", str(recipe), "--metrics", str(root / (label + ".csv")),
                   "--report=" + str(root / (label + ".json")), "--profile-cpu", "--measured", str(frames),
                   "--workers", str(args.workers), "--retain-scene-instances",
                   str(int(enabled or args.mode == "local-gather")), "--cache-local-shadow-gather",
                   str(int(enabled and args.mode == "local-gather")),
                   "--route-offset", str(offset)]
        if args.cpus:
            command = ["taskset", "-c", args.cpus, *command]
        if capture:
            command += ["--eval-capture", str(root / label)]
        start = time.monotonic()
        with (root / (label + ".log")).open("w") as log:
            subprocess.run(command, env=env, check=True, stdout=log, stderr=subprocess.STDOUT)
        rows = list(csv.DictReader((root / (label + ".csv")).open()))
        if len(rows) != frames:
            raise RuntimeError(f"{label}: expected {frames} measured rows, got {len(rows)}")
        if enabled and not capture and not any(int(row["direct_retained_instances"]) for row in rows):
            raise RuntimeError(f"{label}: direct cache never admitted the scene")
        if enabled and args.mode == "local-gather" and not capture and not any(
                int(row["local_gather_cache_hits"]) for row in rows):
            raise RuntimeError(f"{label}: local gather cache never admitted the scene")
        if any(int(row["device_waits"]) for row in rows):
            raise RuntimeError(f"{label}: unexpected device-idle wait")
        result = {"enabled": enabled, "repeat": repeat, "label": label, "capture": capture,
                  "command": command, "elapsed_s": time.monotonic() - start, "columns": summarize(rows)}
        runs.append(result)
        (root / "runs.json").write_text(json.dumps(runs, indent=2) + "\n")
        cpu = result["columns"]["render_main_cpu_ms"]["p50"]
        frame = result["columns"]["frame_interval_ms"]["p50"]
        print(f"{label}: frame {frame:.3f} ms, render thread CPU {cpu:.3f} ms", flush=True)
        return rows, label

    checks = []
    equal_columns = ["route_frame", "draws", "instances", "triangles", "culled", "lights", "spots",
                     "local_shadow_maps", "local_shadow_draws", "local_shadow_triangles", "local_shadow_casters",
                     "skinned_copies", "skin_matrices", "moving_x_sum", "moving_y_sum", "moving_z_sum"]
    if args.mode == "local-gather":
        equal_columns += ["direct_retained_instances", "direct_upload_bytes", "direct_bounds_rebuilt",
                          "bounds_instance_tests", "retained_gpu_bytes", "retained_upload_bytes",
                          "instance_upload_bytes", "feature_bytes", "local_shadow_bytes"]
    for repeat in range(args.repeats):
        pair = {}
        for enabled in ([False, True] if repeat % 2 == 0 else [True, False]):
            pair[enabled], _ = run(enabled, repeat, args.frames)
        for key in equal_columns:
            if any(a[key] != b[key] for a, b in zip(pair[False], pair[True])):
                raise RuntimeError(f"repeat {repeat}: work mismatch in {key}")
        checks.append({"repeat": repeat, "work_equal": True})
    if not args.skip_captures:
        from PIL import Image
        for offset in [0, 540, 960]:
            images = []
            for enabled in [False, True]:
                _, label = run(enabled, offset, 1, offset=offset, capture=True)
                with Image.open(root / label / "frame_0.png") as image:
                    images.append(image.convert("RGBA").tobytes())
            if images[0] != images[1]:
                raise RuntimeError(f"offset {offset}: decoded RGBA pixels differ")
            checks.append({"offset": offset, "pixels_equal": True})
    summary = {}
    for enabled in [False, True]:
        selected = [run for run in runs if run["enabled"] == enabled and not run["capture"]]
        summary["on" if enabled else "off"] = {
            column: {quantile: statistics.median(run["columns"][column][quantile] for run in selected)
                     for quantile in ["p50", "p95"]} for column in selected[0]["columns"]}
    identity = {"mode": args.mode, "binary": str(binary),
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "recipe_sha256": hashlib.sha256(recipe.read_bytes()).hexdigest(),
                "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
                "dirty": subprocess.check_output(["git", "status", "--short"], cwd=repo, text=True).splitlines(),
                "environment": {key: env[key] for key in ["KIN_RENDER_BACKEND", "KIN_REQUIRE_GPU_TESTS",
                                "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "__NV_PRIME_RENDER_OFFLOAD",
                                "__GLX_VENDOR_LIBRARY_NAME"] if key in env}}
    (root / "summary.json").write_text(json.dumps({"summary": summary, "checks": checks,
                                                   "provenance": identity}, indent=2) + "\n")
    print("All paired work and image checks passed.", flush=True)


if __name__ == "__main__":
    main()
