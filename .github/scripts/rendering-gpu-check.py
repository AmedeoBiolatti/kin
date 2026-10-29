#!/usr/bin/env python3
"""Require the rendering integration tests to execute, optionally with Vulkan validation."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET


TESTS = (
    "kin-gpu-memory",
    "kin-vision3d-graph",
    "kin-outline3d-graph",
    "kin-surface3d-graph",
    "kin-ascii3d-graph",
    "kin-slowlight3d-graph",
    "kin-render3d-attachment-graph",
    "kin-render3d-attachment-graph-foreign",
    "kin-render3d-surface-stages",
    "kin-render3d-surface-stages-foreign",
    "kin-render3d-postprocess",
    "kin-render3d-postprocess-foreign",
    "kin-render3d-material-inputs",
    "kin-render3d-material-inputs-foreign",
    "kin-render3d-material-inputs-lifetime",
    "kin-render3d-local-graph",
    "kin-render3d-geometry",
    "kin-render3d-geometry-foreign",
    "kin-render3d-preparation-lifetime",
    "kin-render3d-bounds",
    "kin-render3d-fidelity",
)
VALIDATION_ERROR = re.compile(
    r"Validation Error|SYNC-HAZARD|VK_ERROR_VALIDATION_FAILED_EXT", re.IGNORECASE
)
LAYER_INSERTED = re.compile(r'Insert instance layer "VK_LAYER_KHRONOS_validation"')


def check_report(report, expected, validation=False):
    """CTest can return success for skipped tests; require every selected case."""
    cases = ET.fromstring(report).findall(".//testcase")
    errors = []
    names = [case.get("name") for case in cases]
    if sorted(names) != sorted(expected):
        errors.append(f"test inventory mismatch: expected {sorted(expected)}, got {sorted(names)}")
    observations = []
    for case in cases:
        name = case.get("name")
        output = "\n".join(case.itertext())
        skipped = case.find("skipped") is not None
        failed = case.find("failure") is not None or case.find("error") is not None
        active = bool(LAYER_INSERTED.search(output))
        count = len(VALIDATION_ERROR.findall(output))
        if skipped or failed or case.get("status", "run") != "run":
            errors.append(f"{name}: did not pass without skipping")
        if validation and not active:
            errors.append(f"{name}: no loader evidence that Khronos validation was inserted")
        if count:
            errors.append(f"{name}: {count} Vulkan validation error markers")
        observations.append({"name": name, "skipped": skipped, "failed": failed,
                             "validation_layer_observed": active, "validation_errors": count})
    return errors, observations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("plain", "core", "sync"), default="plain")
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = args.output.resolve() / "ctest.xml"
    log = args.output.resolve() / "ctest.log"
    report.unlink(missing_ok=True)  # A failed invocation must not reuse an older report.
    env = os.environ.copy()
    env.update(KIN_REQUIRE_GPU_TESTS="1", KIN_RENDER_BACKEND="gpu",
               SDL_VIDEO_DRIVER="x11", SDL_AUDIO_DRIVER="dummy")
    validation = args.mode != "plain"
    if validation:
        env.update(VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation", VK_LOADER_DEBUG="layer",
                   VK_VALIDATION_DEBUG_ACTION="VK_DBG_LAYER_ACTION_LOG_MSG",
                   VK_VALIDATION_REPORT_FLAGS="error:warn")
        env.pop("VK_VALIDATION_VALIDATE_SYNC", None)
        if args.mode == "sync":
            env["VK_VALIDATION_VALIDATE_SYNC"] = "1"
    command = ["ctest", "--test-dir", str(args.build.resolve()), "-C", args.config,
               "-R", "^(" + "|".join(re.escape(name) for name in TESTS) + ")$",
               "-j", "1", "--timeout", "300", "--no-tests=error", "--verbose",
               "--output-junit", str(report)]
    start = time.monotonic()
    with log.open("w") as stream:
        result = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT)
    errors = []
    observations = []
    if result.returncode:
        errors.append(f"CTest exited {result.returncode}")
    try:
        errors_from_report, observations = check_report(report.read_text(), TESTS, validation)
        errors.extend(errors_from_report)
    except (OSError, ET.ParseError) as error:
        errors.append(f"missing or invalid CTest report: {error}")
    # Include errors outside testcase output, too (for example loader startup errors).
    total_errors = len(VALIDATION_ERROR.findall(log.read_text(errors="replace")))
    if total_errors:
        errors.append(f"combined log: {total_errors} Vulkan validation error markers")
    summary = {"mode": args.mode, "command": command, "returncode": result.returncode,
               "elapsed_s": time.monotonic() - start, "errors": errors, "tests": observations}
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"Rendering {args.mode}: {len(observations)}/{len(TESTS)} cases, {len(errors)} errors; {log}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        print("\n".join(log.read_text(errors="replace").splitlines()[-80:]), file=sys.stderr)
    return int(bool(errors))


if __name__ == "__main__":
    sys.exit(main())
