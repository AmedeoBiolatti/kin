#!/usr/bin/env python3
"""Summarize Chromium frame/presentation events captured by webgl2_benchmark.mjs."""
import argparse
from collections import Counter
import gzip
import json
from pathlib import Path
import statistics


def distribution(values):
    if not values:
        return None
    values = sorted(values)
    return {"samples": len(values), "median": statistics.median(values),
            "p95": values[int((len(values) - 1) * .95)], "max": values[-1], "sum": sum(values)}


def analyze(trace, frames=None):
    read = gzip.open(trace, "rt") if trace.suffix == ".gz" else trace.open()
    with read:
        events = json.load(read)["traceEvents"]
    threads = {(event["pid"], event["tid"]): event.get("args", {}).get("name")
               for event in events if event["name"] == "thread_name"}
    stages = {}
    for name in ["FireAnimationFrame", "SkiaOutputSurfaceImplOnGpu::SwapBuffers", "Display::DrawAndSwap"]:
        samples = [event for event in events if event["name"] == name and event.get("ph") == "X"]
        stages[name] = {"wall_ms": distribution([event.get("dur", 0) / 1000 for event in samples]),
                        "thread_cpu_ms": distribution([event["tdur"] / 1000 for event in samples if "tdur" in event]),
                        "threads": sorted({threads.get((event["pid"], event["tid"]), "unknown") for event in samples})}
    reasons = Counter()
    sequences = []
    for event in events:
        if event["name"] == "Graphics.Pipeline":
            reason = event.get("args", {}).get("chrome_graphics_pipeline", {}).get("frame_skipped_reason")
            if reason:
                reasons[reason] += 1
        if event["name"] == "FrameSequenceTrackerV3" and event.get("ph") == "b":
            args = event.get("args", {})
            sequences.append({"name": args.get("name"), "data": args.get("args", {}).get("data")})
    result = {"trace": str(trace), "events": len(events), "stages": stages,
              "frame_skipped_reasons": dict(reasons), "frame_sequences": sequences,
              "scope": "Tracing adds overhead. Stage wall time includes CPU/driver waits; this is not a GPU timer query."}
    if frames:
        rows = [row for row in json.loads(frames.read_text())["frames"] if row["step"] > 0 and row["phase"] in [1, 2]]
        result["callbacks"] = {key: distribution([row[key] for row in rows if key in row])
                               for key in ["callbackMs", "intervalMs", "rafDelayMs"]}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--frames", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(analyze(args.trace, args.frames), indent=2) + "\n")


if __name__ == "__main__":
    main()
