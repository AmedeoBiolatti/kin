"""Adapters translate versioned evaluation jobs; renderers keep their scene loaders."""

from __future__ import annotations
import copy
import csv
import json
import math
from pathlib import Path
import re
import sys
from .common import (
    EvalError,
    Unsupported,
    execute,
    identifier,
    number,
    read_json,
    rows,
    write_json,
    write_rows,
)
from .model import ROOT, timeline


def safe_name(name):
    from .common import fingerprint

    text = re.sub(r"[^a-zA-Z0-9_.-]", "_", name)[:72]
    return identifier(
        (text if text and text not in (".", "..") else "view")
        + ("-" + fingerprint(name)[:6] if text != name else "")
    )


def run_process(job, out, argv, *, env=None, cwd=None):
    return execute(
        list(map(str, argv)),
        out,
        cwd=cwd,
        env=dict(KIN_RENDER_BACKEND="gpu", **(env or {})),
        timeout=job["profile"].get("timeout", 1800),
    )


def build_args(job):
    return job["build"].get("args", []) + job["scene"].get("args", [])


def checked_args(job, managed):
    extra = build_args(job)
    if any(arg.split("=", 1)[0] in managed for arg in extra):
        raise EvalError("adapter-managed flags cannot be overridden")
    return extra


def capture_budget(job, count):
    pixels = count * math.prod(job["profile"]["size"])
    if pixels > job["profile"].get("max_score_pixels", 128000000):
        raise EvalError(
            "capture exceeds scoring pixel budget; select a shorter explicit segment"
        )


def image_sequence(sid, files, *, mode="still", fps=60, transform=None):
    return {
        "id": sid,
        "mode": mode,
        "fps": fps,
        "transform": transform or {"kind": "srgb"},
        "frames": files,
    }


def select_records(path, count, view=None):
    if not Path(path).is_file():
        raise Unsupported(
            "binary has no per-frame evaluation telemetry; rebuild with KIN_EVAL_FRAME_LOG support"
        )
    values = rows(path)
    if view is not None:
        values = [r for r in values if r["view"] == view]
    if len(values) < count:
        raise EvalError(f"missing measured frames: {len(values)} of {count}")
    return values[-count:]


def reference(job, out):
    scene, p = job["scene"], job["profile"]
    doc = read_json(scene["source"])
    doc["name"] = scene["id"]
    views = doc.get("cameras", [])
    if scene.get("views"):
        views = [v for v in views if v["name"] in scene["views"]]
    if not views:
        raise EvalError("reference scene has no selected cameras")
    doc["cameras"] = copy.deepcopy(views)
    way = timeline(
        scene, p["frames"] if job["kind"] == "bench" and scene.get("timeline") else None
    )
    if job.get("stress_frames"):
        way = {"fps": 60, "capture": [], "frames": job["stress_frames"]}
    for v in doc["cameras"]:
        v["name"] = safe_name(v.get("name", "view"))
        if way:
            v["eval_frames"] = way["frames"]
            v["eval_capture"] = way["capture"]
    if way and doc.get("animated") and way["fps"] != 60:
        raise Unsupported(
            "reference procedural simulation uses 60 Hz; choose 60 Hz or an application adapter"
        )
    source = out / "scene.json"
    write_json(source, doc)
    width, height = p["size"]
    argv = [
        job["binary"],
        str(source),
        "--out",
        str(out),
        "--cache",
        scene.get("asset_root", str(Path.home() / ".cache/kin/reference")),
        "--warmup",
        str(p["warmup"]),
        "--frames",
        str(p["frames"] if job["kind"] == "bench" else 1),
        "--repeat",
        "1",
        "--width",
        str(width),
        "--height",
        str(height),
    ]
    forbidden = {
        "--out",
        "--cache",
        "--frames",
        "--warmup",
        "--repeat",
        "--width",
        "--height",
        "--eval-bench",
    }
    extra = checked_args(job, forbidden)
    if job["kind"] != "bench":
        capture_budget(job, len(way["capture"]) if way else 1)
    if job["kind"] == "bench":
        if {
            "--readback",
            "--flicker",
            "--supersample",
            "--gpu-pass-markers",
        }.intersection(extra):
            raise EvalError(
                "capture/synchronizing diagnostics cannot run in a throughput job"
            )
        argv += ["--eval-bench"]
    run_process(
        job,
        out,
        argv + extra,
        env={
            "KIN_EVAL_FRAME_LOG": str(out / "raw_frames.jsonl"),
            "KIN_EVAL_CAPTURE_LOG": str(out / "captures.jsonl"),
        },
    )
    info = read_json(out / "results.json")
    if info["size"] != p["size"] or info["repeat"] != 1:
        raise EvalError("effective resolution/repetition mismatch")
    result = {
        "schema": "kin.eval-result/1",
        "status": "ok",
        "adapter": "reference",
        "size": p["size"],
        "sequences": [],
        "effective": info,
        "fps": way["fps"] if way else 60,
        "simulation": "reference 60 Hz frame replay",
        "unmapped_truth_flags": sorted(
            set(extra)
            & {
                "--spots",
                "--point-lights",
                "--rain",
                "--snow",
                "--wind",
                "--no-weather",
                "--no-particles",
                "--particles",
                "--sky",
                "--no-sky",
                "--fog",
                "--no-fog",
                "--sky-light",
                "--sky-light-every",
                "--time",
                "--crowd-move",
                "--moon-shadow",
                "--moon-cascades",
            }
        ),
    }
    if job["kind"] == "bench":
        records = []
        for v in doc["cameras"]:
            records.extend(
                select_records(out / "raw_frames.jsonl", p["frames"], v["name"])
            )
        write_rows(out / "frames.jsonl", records)
        result["frames_file"] = "frames.jsonl"
    else:
        if way:
            captures = rows(out / "captures.jsonl")
            for v in doc["cameras"]:
                frames = [r for r in captures if r["view"] == v["name"]]
                if [r["frame"] for r in frames] != way["capture"]:
                    raise EvalError("incomplete capture sequence")
                result["sequences"].append(
                    image_sequence(
                        v["name"],
                        frames,
                        mode="moving" if len(frames) > 1 else "still",
                        fps=way["fps"],
                    )
                )
        else:
            for c in info["scenes"][0]["cameras"]:
                result["sequences"].append(
                    image_sequence(
                        c["name"],
                        [
                            {
                                "frame": 0,
                                "image": c["png"],
                                "eye": c["eye"],
                                "target": c["target"],
                                "fov": c["fov"],
                            }
                        ],
                    )
                )
    return result


def yard(job, out):
    scene, p = job["scene"], job["profile"]
    if p["warmup"] > 300 or p["frames"] > 6000:
        raise Unsupported("Yard supports warmup <=300 and measured frames <=6000")
    source = scene.get("source")
    if not source:
        source = str(out / "scene.yrd")
        run_process(
            job,
            out,
            [
                sys.executable,
                ROOT / "examples/industrial_yard/generate.py",
                source,
                "--tier",
                scene.get("tier", "small"),
            ],
        )
    capture = scene.get("capture", {})
    count = number(capture.get("count", 1), "capture count", 1, 6000, True)
    start = number(capture.get("from", 0), "capture from", 0, 5999, True)
    if job["kind"] != "bench":
        capture_budget(job, count)
        if start + count > 6000:
            raise Unsupported("Yard capture exceeds 6000 measured frames")
    extra = checked_args(
        job,
        {
            "--recipe",
            "--metrics",
            "--warmup",
            "--measured",
            "--frames",
            "--size",
            "--report",
            "--eval-capture",
            "--eval-capture-from",
            "--eval-capture-count",
            "--preview",
            "--visible",
            "--profile",
        },
    )
    total = p["frames"] if job["kind"] == "bench" else start + count
    w, h = p["size"]
    argv = [
        job["binary"],
        "--recipe",
        source,
        "--metrics",
        str(out / "yard.csv"),
        "--warmup",
        str(p["warmup"]),
        "--measured",
        str(total),
        "--size",
        f"{w}x{h}",
        "--report",
        str(out / "app_report.json"),
    ]
    if job["kind"] != "bench":
        argv += [
            "--eval-capture",
            str(out / "images"),
            "--eval-capture-from",
            str(start),
            "--eval-capture-count",
            str(count),
        ]
    run_process(
        job,
        out,
        argv + extra,
        env={"KIN_EVAL_FRAME_LOG": str(out / "raw_frames.jsonl")},
    )
    result = {
        "schema": "kin.eval-result/1",
        "status": "ok",
        "adapter": "yard",
        "size": p["size"],
        "sequences": [],
        "fps": 60,
        "simulation": "YRD1 route/pose replay, one route frame per rendered frame",
        "effective": read_json(out / "app_report.json"),
    }
    if job["kind"] == "bench":
        data = select_records(out / "raw_frames.jsonl", p["frames"])
        csv_rows = list(csv.DictReader((out / "yard.csv").open()))
        if len(csv_rows) != p["frames"]:
            raise EvalError("Yard workload counter/frame count mismatch")
        for frame, counts in zip(data, csv_rows):
            if frame["frame"] != int(counts["frame"]):
                raise EvalError("Yard workload/telemetry frame IDs differ")
            frame["workload"] = {k: float(v) for k, v in counts.items()}
        write_rows(out / "frames.jsonl", data)
        result["frames_file"] = "frames.jsonl"
    else:
        result["sequences"] = [
            image_sequence(
                "route",
                [
                    {"frame": f, "image": f"images/frame_{f}.png"}
                    for f in range(start, start + count)
                ],
                mode="moving" if count > 1 else "still",
            )
        ]
    return result


def bistro_views(scene):
    source = read_json(scene["source"])
    views = source.get("views", [])
    if scene.get("views"):
        views = [v for v in views if v["name"] in scene["views"]]
    if not views:
        raise EvalError("Bistro has no selected views")
    return views


def path_camera(path, frame):
    t = max(0, frame - path["hold"]) / path["fps"]
    keys = path["keys"]
    if t <= keys[0]["t"]:
        return keys[0]["camera"]
    if t >= keys[-1]["t"]:
        return keys[-1]["camera"]
    for a, b in zip(keys, keys[1:]):
        if a["t"] <= t < b["t"]:
            s = (t - a["t"]) / (b["t"] - a["t"])
            return [x + (y - x) * s for x, y in zip(a["camera"], b["camera"])]
    raise EvalError("invalid Bistro path")


def bistro(job, out):
    scene, p = job["scene"], job["profile"]
    w, h = p["size"]
    content = Path(scene["asset_root"])
    level = read_json(content / "level/level.json")
    result = {
        "schema": "kin.eval-result/1",
        "status": "ok",
        "adapter": "bistro",
        "size": p["size"],
        "sequences": [],
        "fps": 60,
    }
    result["effective"] = {}
    extra = checked_args(
        job,
        {
            "--headless",
            "--frames",
            "--content",
            "--time",
            "--size",
            "--quality",
            "--exposure",
            "--report",
            "--camera",
            "--path",
            "--capture",
            "--capture-from",
            "--capture-count",
            "--fog",
            "--bloom",
            "--no-gi",
            "--show-ui",
        },
    )
    all_records = []
    for view in bistro_views(scene):
        sid = safe_name(view["name"])
        folder = out / sid
        folder.mkdir()
        iso = float(view.get("iso", level["scenarios"][view["time"]]["exposure_mult"]))
        if "path" in view:
            path = view["path"]
            last = path["hold"] + round(path["keys"][-1]["t"] * path["fps"])
            ids = list(range(last - path["capture"] + 1, last + 1))
            fps = path["fps"]
            write_json(folder / "path.json", path)
            camera_arg = [f"--path={folder/'path.json'}"]
        else:
            first = scene.get("settle_frames", 600)
            ids = list(
                range(first, first + int(scene.get("capture", {}).get("count", 1)))
            )
            fps = 60
            camera_arg = ["--camera=" + ",".join(map(str, view["camera"]))]
        if job["kind"] != "bench":
            capture_budget(job, len(ids))
        frames = p["warmup"] + p["frames"] if job["kind"] == "bench" else ids[-1] + 2
        argv = [
            job["binary"],
            "--headless",
            f"--frames={frames}",
            "--mute",
            "--hide-ui",
            "--gi",
            f"--content={content}",
            f"--time={view['time']}",
            f"--size={w}x{h}",
            "--no-fog",
            "--no-bloom",
            f"--quality={scene.get('quality',1)}",
            f"--exposure={iso/3072007*scene.get('tonemap_match',.91)}",
            f"--report={folder/'app_report.json'}",
            *camera_arg,
        ]
        if job["kind"] != "bench":
            argv += [
                f"--capture={folder/'frame_%04d.ppm'}",
                f"--capture-from={ids[0]}",
                f"--capture-count={len(ids)}",
            ]
        run_process(
            job,
            folder,
            argv + extra,
            cwd=scene.get("project"),
            env={"KIN_EVAL_FRAME_LOG": str(folder / "raw_frames.jsonl")},
        )
        report = read_json(folder / "app_report.json")
        if report.get("status") != "ok" or report.get("frames") != frames:
            raise EvalError("Bistro effective frame count/status differs from request")
        state = report["scenes"][0]["state"]
        if [state.get("view_width"), state.get("view_height")] != p["size"]:
            raise EvalError("Bistro effective view dimensions differ from request")
        result["effective"][sid] = report
        if job["kind"] == "bench":
            records = select_records(folder / "raw_frames.jsonl", p["frames"])
            for row in records:
                row["view"] = sid
            all_records.extend(records)
        else:
            images = []
            for f in ids:
                images.append(
                    {
                        "frame": f,
                        "image": f"{sid}/frame_{f:04d}.ppm",
                        "camera": (
                            path_camera(view["path"], f)
                            if "path" in view
                            else view["camera"]
                        ),
                    }
                )
            result["sequences"].append(
                image_sequence(
                    sid,
                    images,
                    mode=(
                        "moving"
                        if "path" in view and len(ids) > 1
                        else "static" if len(ids) > 1 else "still"
                    ),
                    fps=fps,
                    transform={
                        "kind": "bistro_port",
                        "saturation": scene.get("saturation", 1.17),
                    },
                )
            )
            result["sequences"][-1].update(iso=iso, time=view["time"])
    if all_records:
        write_rows(out / "frames.jsonl", all_records)
        result["frames_file"] = "frames.jsonl"
    return result


def command(job, out):
    templates = job["scene"].get("commands", {})
    if job["kind"] not in templates:
        raise Unsupported(f"command adapter has no {job['kind']} provider")
    job_file = out / "job.json"
    write_json(job_file, job)
    values = {
        "job": str(job_file),
        "out": str(out),
        "binary": job.get("binary") or "",
        "source": job["scene"].get("source", ""),
    }
    argv = []
    for arg in templates[job["kind"]]:
        for name, value in values.items():
            arg = arg.replace("{" + name + "}", value)
        argv.append(arg)
    run_process(job, out, argv, cwd=job["scene"].get("project"))
    result = read_json(out / "result.json")
    if result.get("schema") != "kin.eval-result/1":
        raise EvalError("external adapter must write kin.eval-result/1")
    return result


def capture_or_bench(job, out):
    return {"reference": reference, "yard": yard, "bistro": bistro, "command": command}[
        job["scene"]["adapter"]
    ](job, out)


def truth(job, out):
    scene = job["scene"]
    provider = scene.get("reference", {}).get("provider", scene["adapter"])
    if provider in ("baseline", "none"):
        raise Unsupported(
            scene.get("reference", {}).get(
                "reason", "no matched Cycles export declared"
            )
        )
    if provider == "command":
        return command(job, out)
    capture = job["capture"]
    if capture.get("unmapped_truth_flags"):
        raise Unsupported(
            "scene-changing CLI overrides need a matched truth provider: "
            + ", ".join(capture["unmapped_truth_flags"])
        )
    script = scene.get("script") or str(ROOT / "tools/reference/blender_truth.py")
    blender = job["tools"].get("blender")
    if not blender:
        raise Unsupported("Cycles truth requires tools.blender")
    result = {
        "schema": "kin.eval-result/1",
        "status": "ok",
        "adapter": provider,
        "size": job["profile"]["size"],
        "seeds": {},
        "reference_role": "cycles",
    }
    for seed in job["profile"]["seeds"]:
        folder = out / f"seed-{seed}"
        folder.mkdir()
        sequences = []
        if provider == "reference":
            doc = read_json(scene["source"])
            if not doc.get("truth"):
                raise Unsupported("scene declares no Cycles truth mapping")
            if doc.get("animated") or doc.get("procedural") not in (None, ""):
                raise Unsupported(
                    "procedural/animated reference export needs a reviewed scene provider"
                )
            doc["name"] = scene["id"]
            write_json(folder / "scene.json", doc)
            cameras = []
            for seq in capture["sequences"]:
                for frame in seq["frames"]:
                    cameras.append(
                        dict(
                            name=f"{seq['id']}__{frame['frame']}",
                            eye=frame["eye"],
                            target=frame["target"],
                            fov=frame["fov"],
                        )
                    )
                sequences.append(
                    image_sequence(
                        seq["id"],
                        [
                            dict(
                                f,
                                image=f"seed-{seed}/{scene['id']}/{seq['id']}__{f['frame']}.png",
                            )
                            for f in seq["frames"]
                        ],
                        mode=seq["mode"],
                        fps=seq["fps"],
                    )
                )
            metadata = {
                "size": job["profile"]["size"],
                "scenes": [{"name": scene["id"], "cameras": cameras}],
            }
            write_json(folder / "cameras.json", metadata)
            run_process(
                job,
                folder,
                [
                    blender,
                    "-b",
                    "--factory-startup",
                    "--python",
                    script,
                    "--",
                    str(folder / "scene.json"),
                    "--kin",
                    str(folder / "cameras.json"),
                    "--cache",
                    scene["asset_root"],
                    "--out",
                    str(folder),
                    "--samples",
                    str(job["profile"]["samples"]),
                    "--seed",
                    str(seed),
                ],
            )
        elif provider == "bistro":
            if not scene.get("script"):
                raise Unsupported(
                    "Bistro requires its reviewed cycles_truth.py provider"
                )
            for seq in capture["sequences"]:
                if seq["time"] == "night":
                    raise Unsupported("Bistro night lamps are not mapped to Cycles")
                sub = folder / seq["id"]
                sub.mkdir()
                listing = [
                    {
                        "camera": f["camera"],
                        "seed": seed,
                        "out": str(sub / f"frame_{f['frame']}.exr"),
                    }
                    for f in seq["frames"]
                ]
                write_json(sub / "frames.json", listing)
                w, h = job["profile"]["size"]
                run_process(
                    job,
                    sub,
                    [
                        blender,
                        "-b",
                        "--factory-startup",
                        "--python",
                        script,
                        "--",
                        "--content",
                        scene["asset_root"],
                        "--time",
                        seq["time"],
                        "--size",
                        f"{w}x{h}",
                        "--samples",
                        str(job["profile"]["samples"]),
                        "--frames",
                        str(sub / "frames.json"),
                    ],
                )
                sequences.append(
                    image_sequence(
                        seq["id"],
                        [
                            dict(
                                f,
                                image=f"seed-{seed}/{seq['id']}/frame_{f['frame']}.exr",
                            )
                            for f in seq["frames"]
                        ],
                        mode=seq["mode"],
                        fps=seq["fps"],
                        transform={
                            "kind": "bistro_cycles",
                            "iso": seq["iso"],
                            "saturation": scene.get("saturation", 1.17),
                        },
                    )
                )
        else:
            raise Unsupported(f"no truth provider: {provider}")
        result["seeds"][str(seed)] = sequences
    return result
