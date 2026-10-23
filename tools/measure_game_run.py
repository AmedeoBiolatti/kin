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

try:
    import resource
except ImportError:  # Windows has no getrusage; retain wall-time measurements.
    resource = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", nargs=2, action="append", required=True, metavar=("LABEL", "EXECUTABLE"))
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--argument", action="append", default=[], help="Extra game flag (use --argument=--stress)")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--cpu", type=int, help="Pin this benchmark and its children to one allowed CPU (Linux)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.frames < 2 or args.repetitions < 1:
        parser.error("frames must be >= 2 and repetitions >= 1")
    if args.cpu is not None:
        if not hasattr(os, "sched_getaffinity") or args.cpu not in os.sched_getaffinity(0):
            parser.error("--cpu requires an allowed CPU on a system supporting affinity")
        os.sched_setaffinity(0, {args.cpu})
    report = {
        "schema": "kin.process-timing/1",
        "platform": platform.platform(),
        "frames": args.frames,
        "seed": args.seed,
        "arguments": args.argument,
        "repetitions": args.repetitions,
        "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
        "environment": {key: os.environ.get(key) for key in
                        ("SDL_VIDEO_DRIVER", "SDL_AUDIO_DRIVER", "KIN_RENDER_BACKEND", "VK_ICD_FILENAMES", "KIN_SKIP_PRESENT", "KIN_LOG_FRAME_STATS")},
        "note": "One-frame timing includes process startup, rendering and shutdown. Frame cost is an estimate, not a percentile. POSIX CPU time sums child user/system time and excludes time waiting to be scheduled.",
        "games": [],
    }
    for label, executable in args.game:
        binary = str(Path(executable).resolve(strict=True))
        samples = {1: [], args.frames: []}
        cpu_samples = {1: [], args.frames: []}
        for _ in range(args.repetitions):
            for frames in samples:
                command = [binary, "--headless", "--render", f"--frames={frames}", f"--seed={args.seed}", *args.argument]
                start = time.perf_counter_ns()
                before = resource.getrusage(resource.RUSAGE_CHILDREN) if resource else None
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=300)
                samples[frames].append((time.perf_counter_ns() - start) / 1_000_000)
                if resource:
                    after = resource.getrusage(resource.RUSAGE_CHILDREN)
                    cpu_samples[frames].append(1000 * (after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime))
        first = statistics.median(samples[1])
        total = statistics.median(samples[args.frames])
        result = {"label": label, "executable": binary, "samples_ms": samples,
                  "one_frame_process_ms": first, "many_frame_process_ms": total,
                  "estimated_frame_ms": (total - first) / (args.frames - 1)}
        if resource:
            cpu_first = statistics.median(cpu_samples[1])
            cpu_total = statistics.median(cpu_samples[args.frames])
            result.update(samples_cpu_ms=cpu_samples, one_frame_process_cpu_ms=cpu_first,
                          many_frame_process_cpu_ms=cpu_total,
                          estimated_frame_cpu_ms=(cpu_total - cpu_first) / (args.frames - 1))
        report["games"].append(result)
        print(f"{label}: one-frame process {first:.3f} ms; estimated frame {result['estimated_frame_ms']:.3f} ms", flush=True)
        if resource:
            print(f"{label}: estimated frame CPU {result['estimated_frame_cpu_ms']:.3f} ms", flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
