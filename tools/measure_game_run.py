#!/usr/bin/env python3
"""Measure rendered game runs without compiled profile capture (standard library only)."""
import argparse
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", nargs=2, action="append", required=True, metavar=("LABEL", "EXECUTABLE"))
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.frames < 2 or args.repetitions < 1:
        parser.error("frames must be >= 2 and repetitions >= 1")
    report = {
        "schema": "kin.process-timing/1",
        "platform": platform.platform(),
        "frames": args.frames,
        "repetitions": args.repetitions,
        "environment": {key: os.environ.get(key) for key in
                        ("SDL_VIDEO_DRIVER", "SDL_AUDIO_DRIVER", "KIN_RENDER_BACKEND", "VK_ICD_FILENAMES", "KIN_SKIP_PRESENT", "KIN_LOG_FRAME_STATS")},
        "note": "One-frame timing includes process startup, rendering and shutdown. Frame cost is an estimate, not a percentile.",
        "games": [],
    }
    for label, executable in args.game:
        binary = str(Path(executable).resolve(strict=True))
        samples = {1: [], args.frames: []}
        for _ in range(args.repetitions):
            for frames in samples:
                command = [binary, "--headless", "--render", f"--frames={frames}", "--seed=7"]
                start = time.perf_counter_ns()
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=300)
                samples[frames].append((time.perf_counter_ns() - start) / 1_000_000)
        first = statistics.median(samples[1])
        total = statistics.median(samples[args.frames])
        result = {"label": label, "executable": binary, "samples_ms": samples,
                  "one_frame_process_ms": first, "many_frame_process_ms": total,
                  "estimated_frame_ms": (total - first) / (args.frames - 1)}
        report["games"].append(result)
        print(f"{label}: one-frame process {first:.3f} ms; estimated frame {result['estimated_frame_ms']:.3f} ms", flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
