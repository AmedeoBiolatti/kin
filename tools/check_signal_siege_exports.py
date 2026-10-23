#!/usr/bin/env python3
"""Compare trusted local Signal Siege ZIPs after relocation (requires Pillow).

Runs default/stress arenas and the upgrade screen in all four locales. GPU mode
requires a real display and GPU backend; it must not silently use SDL software.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile
from PIL import Image

CASES = [
    ("arena", 120, ["--locale=en"], 300, False),
    ("stress", 120, ["--stress", "--locale=en"], 1200, False),
    ("grid-en", 30, ["--power-grid", "--locale=en"], 300, True),
    ("grid-fr", 30, ["--power-grid", "--locale=fr"], 300, True),
    ("grid-ja", 30, ["--power-grid", "--locale=ja"], 300, True),
    ("grid-ar", 30, ["--power-grid", "--locale=ar"], 300, True),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", nargs=2, action="append", required=True, metavar=("LABEL", "ZIP"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gpu", action="store_true")
    args = parser.parse_args()
    if len(args.package) < 2 or len({label for label, _ in args.package}) != len(args.package):
        parser.error("provide at least two packages with unique labels")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, SDL_VIDEO_DRIVER="x11" if args.gpu else "dummy", SDL_AUDIO_DRIVER="dummy",
                       KIN_RENDER_BACKEND="gpu" if args.gpu else "sdl", KIN_LOG_FORMAT="text")
    rows = []
    reference = {}
    for index, (label, filename) in enumerate(args.package):
        with tempfile.TemporaryDirectory(prefix="kin siege export ") as temp:
            destination = Path(temp) / "relocated game with spaces"
            with zipfile.ZipFile(Path(filename).resolve()) as package:
                package.extractall(destination)
            candidates = list(destination.glob("*/SHA256SUMS"))
            if len(candidates) != 1:
                raise RuntimeError("Expected one packaged manifest")
            root = candidates[0].parent
            count = 0
            for line in candidates[0].read_text().splitlines():
                expected, relative = line.split("  ", 1)
                if hashlib.sha256((root / relative).read_bytes()).hexdigest() != expected:
                    raise RuntimeError(f"Checksum mismatch: {relative}")
                count += 1
            for locale in ("en", "fr", "ja", "ar"):
                if not (root / "assets/signal_siege" / f"{locale}.kinlang").is_file():
                    raise RuntimeError(f"Missing localization asset: {locale}")
            binary = root / ("signal_siege.exe" if os.name == "nt" else "signal_siege")
            binary.chmod(binary.stat().st_mode | 0o100)
            for case, frames, options, enemies, grid in CASES:
                capture = output / f"package-{index}-{case}.png"
                command = [str(binary), "--headless", "--render", f"--frames={frames}", "--seed=7",
                           "--report=-", "--screenshot", str(capture), "--capture-frame", str(frames),
                           "--fail-on-missing-text", *options]
                result = subprocess.run(command, cwd=temp, env=environment, capture_output=True,
                                        text=True, check=True, timeout=300)
                report = json.loads(result.stdout)
                if report["status"] != "ok" or report["localization"]["missing_text"]:
                    raise RuntimeError(f"Run/localization failure: {report}")
                expected_locale = next(value.split("=", 1)[1] for value in options if value.startswith("--locale="))
                if report["localization"]["locale"] != expected_locale:
                    raise RuntimeError(f"Requested locale was not selected: {report['localization']}")
                expected_backend = "SDL_GPU" if args.gpu else "software"
                if f"renderer created backend={expected_backend}" not in result.stderr:
                    raise RuntimeError(f"Expected {expected_backend}, got:\n{result.stderr}")
                state = report["scenes"][0]["state"]
                if state["enemies"] != enemies or state["power_grid"] != grid:
                    raise RuntimeError(f"Unexpected scenario: {state}")
                with Image.open(capture) as image:
                    pixels = image.convert("RGBA").tobytes()
                    dimensions = image.size
                row = {"package": label, "case": case, "frames": frames, "renderer": expected_backend,
                       "checksums_verified": count, "state": state, "locale": report["localization"]["locale"],
                       "image_size": dimensions, "pixel_sha256": hashlib.sha256(pixels).hexdigest()}
                if case in reference:
                    baseline = reference[case]
                    if (row["state"], row["locale"], row["image_size"], row["pixel_sha256"]) != (
                            baseline["state"], baseline["locale"], baseline["image_size"], baseline["pixel_sha256"]):
                        raise RuntimeError(f"Export differs from reference: {label}/{case}")
                else:
                    reference[case] = row
                rows.append(row)
                (output / f"package-{index}-{case}.json").write_text(json.dumps(report, indent=2) + "\n")
                print(f"{label}/{case}: {expected_backend}; state and pixels match", flush=True)
    (output / "checks.json").write_text(json.dumps({"schema": "kin.signal-siege-export/1", "gpu": args.gpu,
        "note": "Trusted local ZIPs, manifest verification, unrelated cwd, fixed seed, silent tool runs without saved-game writes; exact shared state and decoded pixel comparison. System fonts are host prerequisites. GPU software drivers establish functionality, not hardware performance.",
        "runs": rows}, indent=2) + "\n")


if __name__ == "__main__":
    main()
