#!/usr/bin/env python3
"""Compare matched Summit traversals on one GPU, using an isolated X11 display."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

REPO = Path(__file__).resolve().parents[2]


def percentile(values, p):
    ordered = sorted(values)
    index = (len(ordered) - 1) * p
    lo = int(index)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (index - lo)


def distribution(values):
    return {"samples": len(values), "p50": statistics.median(values),
            "p95": percentile(values, .95), "p99": percentile(values, .99), "max": max(values)} if values else None


def descendant(pid, owner):
    for _ in range(32):
        if pid == owner:
            return True
        if pid <= 1:
            return False
        try:
            status = Path(f"/proc/{pid}/status").read_text()
            pid = int(next(line.split()[1] for line in status.splitlines() if line.startswith("PPid:")))
        except (OSError, StopIteration):
            return False
    return False


def gpu_state(owner=0, known=None):
    pmon = subprocess.check_output(["nvidia-smi", "pmon", "-c", "1"], text=True, timeout=5)
    conflicts = []
    for line in pmon.splitlines():
        fields = line.split()
        if line.startswith("#") or len(fields) < 4 or not fields[1].isdigit():
            continue
        pid = int(fields[1])
        ours = (known is not None and pid in known) or descendant(pid, owner)
        if ours and known is not None:
            known.add(pid)
        if fields[-1] != "gnome-shell" and not ours:
            conflicts.append({"pid": pid, "name": fields[-1]})
    return {"wall_time": time.time(), "conflicts": conflicts, "pmon": pmon}


def quiet():
    deadline = time.monotonic() + 180
    clean = 0
    while time.monotonic() < deadline:
        state = gpu_state()
        clean = 0 if state["conflicts"] else clean + 1
        if clean >= 3:
            return
        if state["conflicts"]:
            print("Waiting for GPU:", state["conflicts"], flush=True)
        time.sleep(2)
    raise RuntimeError("A competing GPU workload is still active; comparison not started")


def run(command, output, env, allow_contention=False):
    if not allow_contention:
        quiet()
    with (output / "process.log").open("w") as log, (output / "gpu-environment.jsonl").open("w") as monitor:
        process = subprocess.Popen(command, cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT)
        start = time.monotonic()
        conflicts = []
        known = {process.pid}
        try:
            while process.poll() is None:
                if time.monotonic() - start > 280:
                    raise RuntimeError("Traversal timed out")
                state = gpu_state(process.pid, known)
                conflicts.extend(state["conflicts"])
                monitor.write(json.dumps(state) + "\n")
                monitor.flush()
                time.sleep(1)
            if process.returncode:
                raise RuntimeError(f"Benchmark exited {process.returncode}: {output / 'process.log'}")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    if conflicts and not allow_contention:
        raise RuntimeError(f"Run has competing GPU work; retain it for diagnosis, then rerun: {conflicts}")
    return sorted({item["name"] for item in conflicts})


def analyze(path, complete=True):
    metadata = json.loads((path / "benchmark.csv.json").read_text())
    assert metadata["quality"] == "core" and metadata["scenery"] == "compact" and metadata["samples"] == 1
    assert not metadata["falls"] and not metadata["deaths"]
    if complete:
        assert metadata["completed"] and metadata["sheltered"], "The full sheltered route must complete"
    rows = [row for row in csv.DictReader((path / "benchmark.csv").open())
            if row["measured"] == "1" and row["phase"] in ["1", "2"]]
    assert len(rows) > (1000 if complete else 20)
    assert all(int(row["shadow_cascades"]) == 2 and int(row["skinned_copies"]) == 1 for row in rows)
    assert all(int(row["skin_matrices"]) == 20 for row in rows)
    stats = {key: distribution([float(row[key]) for row in rows])
             for key in ["frame_interval_ms", "scene_build_ms", "cpu_submit_ms", "ui_ms", "draws", "triangles", "instances"]}
    stats["cpu_render_path_ms"] = distribution([sum(float(row[key]) for key in ["scene_build_ms", "cpu_submit_ms", "ui_ms"])
                                                 for row in rows])
    for key in ["retained_gpu_bytes", "instance_upload_bytes", "retained_upload_bytes", "prepared_draws", "prepared_reused"]:
        if key in rows[0]:
            stats[key] = distribution([float(row[key]) for row in rows])
    if "retained_upload_bytes" in rows[0]:
        stats["total_instance_upload_bytes"] = distribution([
            float(row["instance_upload_bytes"]) + float(row["retained_upload_bytes"]) for row in rows])
    stats["gpu_fence_frame_ms"] = distribution([float(row["gpu_frame_ms"]) for row in rows if float(row["gpu_frame_ms"]) > 0])
    result = {"metadata": metadata, "statistics": stats}
    if (path / "frames.json").exists():
        frames = json.loads((path / "frames.json").read_text())
        assert frames["disjoints"] == 0, "Disjoint GPU timer results cannot be compared"
        result["gpu_query_ms"] = distribution([r["ms"] for r in frames["gpu"] if r.get("step", 0) > 0])
        result["callback_ms"] = distribution([r["callbackMs"] for r in frames["frames"] if r["step"] > 0 and r["phase"] in [1, 2]])
    (path / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def summarize(root, runs):
    manifest = json.loads((root / "manifest.json").read_text())
    browser_mode = manifest.get("browser_mode", "windowed")
    browser_window = manifest.get("browser_window", "browser")
    report = {"scope": "Same NVIDIA GPU, matched core quality and scripted sheltered route; see browser_mode for presentation",
              "browser_mode": browser_mode,
              "browser_window": browser_window if browser_mode == "windowed" else None,
              "cpu_scope": "Scene preparation + 3D submission + UI spans; excludes simulation updates and final present/flush",
              "pacing_scope": "Game-loop start-to-start intervals; native uses requested cap and browser uses normal rAF; not maximum FPS",
              "excluded_contended_runs": [r["report"] for r in runs if r["contending_gpu_processes"]],
              "results": {}}
    if browser_mode == "offscreen":
        report["presentation_note"] = "Offscreen browser rAF has no window presentation; use this to diagnose rendering capacity, not onscreen/itch.io FPS."
    for target in ["native", "web"]:
        clean = [r for r in runs if r["target"] == target and not r["gpu_diagnostic"]
                 and not r["contending_gpu_processes"]]
        if not clean:
            continue
        rows = []
        for run in clean:
            path = root / run["report"]
            rows.extend(row for row in csv.DictReader((path.parent / "benchmark.csv").open())
                        if row["measured"] == "1" and row["phase"] in ["1", "2"])
        intervals = [float(row["frame_interval_ms"]) for row in rows]
        spans = [sum(float(row[key]) for key in ["scene_build_ms", "cpu_submit_ms", "ui_ms"]) for row in rows]
        report["results"][target] = {"runs": len(clean), "frames": len(rows),
                                     "game_loop_fps": 1000 / statistics.mean(intervals),
                                     "frame_interval_ms": distribution(intervals),
                                     "cpu_render_spans_ms": distribution(spans),
                                     "frames_over_25_ms": sum(t > 25 for t in intervals),
                                     "route_steps": [r["metadata"]["steps"] for r in clean],
                                     "route_hashes_within_build": [r["metadata"]["final_hash_hex"] for r in clean]}
    (root / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--web-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=2)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--browser-mode", choices=["windowed", "offscreen"], default="windowed",
                        help="Offscreen isolates browser rendering capacity from window presentation")
    parser.add_argument("--browser-window", choices=["app", "browser"], default="app",
                        help="Match native window/viewport size, or reproduce the larger browser-window diagnostic")
    parser.add_argument("--allow-contention", action="store_true",
                        help="Keep explicitly labeled diagnostic results while other GPU applications run")
    args = parser.parse_args()
    root = args.output.resolve()
    assert not root.exists() or not any(root.iterdir()), "Choose a fresh output directory"
    assert args.pairs > 0 and args.fps > 0 and args.width > 0 and args.height > 0
    root.mkdir(parents=True, exist_ok=True)
    native, web = args.native.resolve(), args.web_root.resolve()
    env = dict(os.environ, SDL_VIDEO_DRIVER="x11", SDL_AUDIO_DRIVER="dummy", KIN_RENDER_BACKEND="gpu",
               VK_DRIVER_FILES="/usr/share/vulkan/icd.d/nvidia_icd.json",
               KIN_UI_FONT_REGULAR=str(REPO / "examples/content/lang/fonts/latin.ttf"),
               KIN_UI_FONT_BOLD=str(REPO / "examples/content/lang/fonts/latin.ttf"))
    env.pop("KIN_LOG_FRAME_STATS", None)
    manifest = {"gpu": subprocess.check_output(["nvidia-smi", "--query-gpu=name,driver_version", "--format=csv"], text=True),
                "chrome": subprocess.check_output(["google-chrome", "--version"], text=True).strip(),
                "render_size": [args.width, args.height], "native_fps_cap": args.fps,
                "browser_mode": args.browser_mode,
                "browser_window": args.browser_window if args.browser_mode == "windowed" else None,
                "browser_pacing": "Default requestAnimationFrame; offscreen mode has no window presentation",
                "native_sha256": hashlib.sha256(native.read_bytes()).hexdigest(),
                "wasm_sha256": hashlib.sha256((web / "summit_web.wasm").read_bytes()).hexdigest(),
                "environment": {key: value for key, value in env.items() if key.startswith(("KIN_", "SDL_", "VK_"))},
                "gpu_timing": "Separate diagnostic runs; native fence completion and WebGL timer queries have different semantics"}
    manifest["allow_contention"] = args.allow_contention
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    runs = []
    order = [(target, index + 1, False) for index in range(args.pairs)
             for target in (["native", "web"] if index % 2 == 0 else ["web", "native"])]
    order += [("native", "gpu", True), ("web", "gpu", True)]
    for target, index, gpu in order:
        path = root / f"{target}-{index}"
        path.mkdir()
        if target == "native":
            command = [str(native), "--benchmark", str(path / "benchmark.csv"), "--script", "shelter",
                       "--quality", "core", "--scenery", "compact", "--look", "morning", "--freeze-time",
                       "--seed", "1", "--mute", "--no-settings", "--fps", str(args.fps),
                       "--width", str(args.width), "--height", str(args.height), "--benchmark-warmup", "2"]
            if gpu:
                command += ["--gpu-timing", "--max-steps", "900"]
        else:
            command = ["node", "tests/webgl2_benchmark.mjs", "--root", str(web), "--output", str(path),
                       "--width", str(args.width), "--height", str(args.height)]
            if args.browser_mode == "offscreen":
                command += ["--headless"]
            else:
                command += ["--window", args.browser_window]
            if gpu:
                command += ["--gpu-timing", "--steps", "900"]
        (path / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        print("Starting", path.name, flush=True)
        conflicts = run(command, path, env, args.allow_contention)
        result = analyze(path, complete=not gpu)
        assert (result["metadata"]["width"], result["metadata"]["height"]) == (args.width, args.height)
        runs.append({"target": target, "run": index, "gpu_diagnostic": gpu,
                     "contending_gpu_processes": conflicts, "report": path.name + "/summary.json", **result})
        (root / "comparison.json").write_text(json.dumps({"manifest": manifest, "runs": runs}, indent=2) + "\n")
        summarize(root, runs)
        print(path.name, json.dumps(result["statistics"]["frame_interval_ms"]), flush=True)


if __name__ == "__main__":
    main()
