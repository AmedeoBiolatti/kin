from __future__ import annotations
import collections
import importlib.metadata
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import time
from . import adapters
from .common import (
    EvalError,
    Unsupported,
    fingerprint,
    gpu_lock,
    identifier,
    read_json,
    rows,
    sha256,
    within,
    write_json,
)
from .model import HERE, ROOT, input_files, timeline


def provenance(plan):
    code = {str(p.relative_to(ROOT)): sha256(p) for p in sorted(HERE.rglob("*.py"))}
    code["examples/industrial_yard/generate.py"] = sha256(
        ROOT / "examples/industrial_yard/generate.py"
    )
    code["tools/reference/blender_truth.py"] = sha256(
        ROOT / "tools/reference/blender_truth.py"
    )
    data = {}
    problems = {}
    for scene in plan["scenes"]:
        try:
            data[scene["id"]] = input_files(scene)
            if scene.get("script"):
                data[scene["id"]][scene["script"]] = sha256(scene["script"])
            if scene.get("timeline"):
                timeline(scene)
        except (OSError, EvalError) as e:
            problems[scene["id"]] = str(e)
    binaries = {}
    for label, build in plan["builds"].items():
        for key, path in build["executables"].items():
            if Path(path).is_file():
                binaries[f"{label}/{key}"] = {"path": path, "sha256": sha256(path)}
    build_problems = {}
    for label, build in plan["builds"].items():
        for scene in plan["scenes"]:
            adapter = scene["adapter"]
            if adapter != "command" and f"{label}/{adapter}" not in binaries:
                build_problems[f"{label}/{adapter}"] = "missing executable"
    display = {}
    if plan["display"].get("file"):
        display = {plan["display"]["file"]: sha256(plan["display"]["file"])}
    tool_files = {
        k: sha256(v) if Path(v).is_file() else "missing"
        for k, v in plan["tools"].items()
    }
    metrics_python = plan["tools"].get("metrics_python", os.sys.executable)
    try:
        cmd = [
            metrics_python,
            "-c",
            "import importlib.metadata as m,json; names=['numpy','pillow','cvvdp','flip-evaluator','torch','OpenEXR']; print(json.dumps({n:m.version(n) for n in names if any(d.metadata['Name'].lower()==n.lower() for d in m.distributions())},sort_keys=True))",
        ]
        packages = subprocess.run(
            cmd, capture_output=True, text=True, timeout=30
        ).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        packages = "unavailable"
    context = {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "environment": {
            k: os.environ[k]
            for k in (
                "DISPLAY",
                "VK_ICD_FILENAMES",
                "VK_DRIVER_FILES",
                "KIN_RENDER_BACKEND",
                "KIN_SKIP_PRESENT",
                "SDL_VIDEO_DRIVER",
                "SDL_AUDIODRIVER",
                "CUDA_VISIBLE_DEVICES",
            )
            if k in os.environ
        },
    }
    if shutil.which("nvidia-smi"):
        try:
            identity = subprocess.run(
                [
                    "nvidia-smi",
                    "--query-gpu=uuid,name,driver_version",
                    "--format=csv,noheader",
                ],
                capture_output=True,
                text=True,
                timeout=10,
            )
            context["nvidia_identity"] = identity.stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            context["nvidia_identity"] = "unavailable"
    for field in ("VK_ICD_FILENAMES", "VK_DRIVER_FILES"):
        for name in os.environ.get(field, "").split(os.pathsep):
            if name and Path(name).is_file():
                context[name] = sha256(name)
    return {
        "code": code,
        "content": data,
        "binaries": binaries,
        "build_unavailable": build_problems,
        "display": display,
        "tool_files": tool_files,
        "metric_packages": packages,
        "unavailable": problems,
        "render_context": context,
    }


def machine():
    result = {
        "platform": platform.platform(),
        "python": platform.python_version(),
        "environment": {
            k: os.environ[k]
            for k in (
                "DISPLAY",
                "VK_ICD_FILENAMES",
                "KIN_RENDER_BACKEND",
                "SDL_VIDEO_DRIVER",
            )
            if k in os.environ
        },
    }
    if shutil.which("nvidia-smi"):
        try:
            p = subprocess.run(
                [
                    "nvidia-smi",
                    "--query-gpu=name,driver_version,memory.total,memory.used,temperature.gpu,power.draw,clocks.sm",
                    "--format=csv,noheader,nounits",
                ],
                capture_output=True,
                text=True,
                timeout=10,
            )
            result["device_total_observation"] = p.stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            pass
    return result


def percentile(values, q):
    values = sorted(values)
    if not values:
        return None
    x = (len(values) - 1) * q
    lo = int(x)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (x - lo)


def summarize_frames(records):
    groups = collections.defaultdict(list)
    for r in records:
        groups[r["view"]].append(r)
    result = {}
    for view, data in groups.items():
        cpu = {}
        for name in ("interval_ms", "work_ms", "update_ms", "render_ms", "present_ms"):
            values = [r.get("cpu", {}).get(name) for r in data]
            values = [
                v
                for v in values
                if isinstance(v, (int, float)) and math.isfinite(v) and v >= 0
            ]
            cpu[name] = (
                {
                    "n": len(values),
                    "mean": statistics.fmean(values),
                    "p50": percentile(values, 0.5),
                    "p95": percentile(values, 0.95),
                    "p99": percentile(values, 0.99),
                }
                if values
                else None
            )
        gpu = {}
        for r in data:
            g = r.get("gpu", {})
            if g.get("sample_serial", 0) > 0 and g.get("latest_ms", 0) > 0:
                gpu[g["sample_serial"]] = g["latest_ms"]
        memory = {}
        for key in ("owned_bytes", "pooled_texture_bytes", "staging_bytes"):
            values = [r.get("memory", {}).get(key) for r in data]
            values = [v for v in values if v is not None]
            if values:
                memory[key] = {
                    "first": values[0],
                    "last": values[-1],
                    "peak": max(values),
                }
        alloc = [r.get("memory", {}).get("allocations") for r in data]
        result[view] = {
            "frames": len(data),
            "cpu": cpu,
            "memory": memory,
            "allocation_delta": (
                alloc[-1] - alloc[0]
                if alloc and None not in (alloc[0], alloc[-1])
                else None
            ),
            "gpu": {
                "method": "fence_completion_estimate",
                "source_frame": None,
                "percentiles_valid": False,
                "fresh_observations": len(gpu),
                "observation_median_ms": (
                    statistics.median(gpu.values()) if gpu else None
                ),
            },
        }
    return result


def validate_result(folder, result, profile):
    if result.get("schema") != "kin.eval-result/1" or result.get("status") not in (
        "ok",
        "unsupported",
    ):
        raise EvalError("adapter result schema/status invalid")
    if result["status"] == "unsupported":
        return
    if result.get("size") != profile["size"]:
        raise EvalError("adapter result dimensions differ from request")
    if result.get("frames_file"):
        records = rows(within(folder, result["frames_file"]))
        seen = set()
        if not records:
            raise EvalError("empty frame records")
        for r in records:
            if r.get("schema") != "kin.eval-frame/1":
                raise EvalError("expected kin.eval-frame/1")
            key = (r.get("view"), r.get("frame"))
            if (
                not isinstance(key[0], str)
                or not key[0]
                or isinstance(key[1], bool)
                or not isinstance(key[1], int)
                or key[1] < 0
                or key in seen
            ):
                raise EvalError("invalid/duplicate frame IDs")
            seen.add(key)
            if "interval_ms" not in r.get("cpu", {}):
                raise EvalError("frame record needs a CPU interval")
            for value in r.get("cpu", {}).values():
                if (
                    not isinstance(value, (int, float))
                    or not math.isfinite(value)
                    or value < 0
                ):
                    raise EvalError("invalid CPU timing")
        counts = collections.Counter(r["view"] for r in records)
        if any(n != profile["frames"] for n in counts.values()):
            raise EvalError("measured frame count differs from request")
    collections_ = [result.get("sequences", [])] + list(
        result.get("seeds", {}).values()
    )
    for sequences in collections_:
        ids = set()
        for seq in sequences:
            identifier(seq["id"])
            if seq["id"] in ids:
                raise EvalError("duplicate sequence ID")
            ids.add(seq["id"])
            indices = [f["frame"] for f in seq["frames"]]
            if (
                not indices
                or sorted(set(indices)) != indices
                or any(
                    isinstance(f, bool) or not isinstance(f, int) or f < 0
                    for f in indices
                )
            ):
                raise EvalError("frame indices must be increasing, unique integers")
            if seq.get("mode") not in ("still", "static", "moving"):
                raise EvalError("invalid sequence mode")
            if seq["mode"] == "still" and len(indices) != 1:
                raise EvalError("still sequence needs one frame")
            if seq["mode"] != "still" and (
                len(indices) < 2
                or any(b - a != 1 for a, b in zip(indices, indices[1:]))
            ):
                raise EvalError("clips require contiguous frames with a known cadence")
            if (
                isinstance(seq.get("fps"), bool)
                or not isinstance(seq.get("fps"), (float, int))
                or not math.isfinite(seq["fps"])
                or seq["fps"] <= 0
            ):
                raise EvalError("positive finite sequence FPS required")
            for f in seq["frames"]:
                image = within(folder, f["image"])
                if not image.is_file():
                    raise EvalError(f"missing capture frame: {image}")
                if image.suffix.lower() == ".exr":
                    with image.open("rb") as src:
                        if src.read(4) != b"v/1\x01" or image.stat().st_size < 32:
                            raise EvalError("invalid/truncated EXR header")
                else:
                    try:
                        from PIL import Image
                    except ImportError as e:
                        raise Unsupported("capture validation needs Pillow") from e
                    with Image.open(image) as im:
                        if list(im.size) != profile["size"]:
                            raise EvalError("captured image dimensions differ")
                        im.verify()


def inventory(folder):
    return {
        str(p.relative_to(folder)): sha256(p)
        for p in sorted(folder.rglob("*"))
        if p.is_file() and p.name != "complete.json"
    }


def cached(folder, key):
    try:
        marker = read_json(folder / "complete.json")
        if marker["key"] != key:
            return False
        return marker["artifacts"] == inventory(folder)
    except (OSError, ValueError, KeyError, EvalError):
        return False


def score_job(job, out):
    from .metrics import Perceptual, compare, transform

    try:
        from PIL import Image
        import numpy as np
    except ImportError as e:
        raise Unsupported("basic image scoring needs NumPy and Pillow") from e
    capture = job["capture"]
    reference = job["reference"]
    wanted = job["profile"]["metrics"]
    perceptual = Perceptual(job.get("display", {}), wanted)
    actual = capture["result"]["sequences"]
    truth = reference["result"]
    seeds = truth.get("seeds")
    if seeds:
        if len(seeds) < 2:
            raise EvalError("Cycles scoring requires two independent seeds")
        keys = list(seeds)
        refs = seeds[keys[0]]
        floor = seeds[keys[1]]
    else:
        refs = truth["sequences"]
        floor = None
    by_id = {s["id"]: s for s in refs}
    if set(by_id) != {s["id"] for s in actual}:
        raise EvalError("reference/test sequence sets differ")
    scores = []
    for test in actual:
        ref = by_id[test["id"]]
        ids = [f["frame"] for f in test["frames"]]
        if (
            [f["frame"] for f in ref["frames"]] != ids
            or ref["mode"] != test["mode"]
            or ref["fps"] != test["fps"]
        ):
            raise EvalError("reference/test frame IDs, mode or FPS differ")
        for a, b in zip(ref["frames"], test["frames"]):
            for key in ("eye", "target", "fov", "camera"):
                if key in a and key in b and a[key] != b[key]:
                    raise EvalError(f"reference camera mismatch: {key}")
        pixels = len(ids) * job["profile"]["size"][0] * job["profile"]["size"][1]
        if pixels > job["profile"].get("max_score_pixels", 128000000):
            raise EvalError(
                "clip exceeds scoring pixel budget; select a shorter explicit segment"
            )
        r = [
            transform(within(reference["root"], f["image"]), ref.get("transform", {}))
            for f in ref["frames"]
        ]
        t = [
            transform(within(capture["root"], f["image"]), test.get("transform", {}))
            for f in test["frames"]
        ]
        metrics, maps = compare(r, t, test["fps"], test["mode"], wanted, perceptual)
        metrics.update(
            sequence=test["id"],
            reference_role=truth.get(
                "reference_role", "cycles" if seeds else "baseline"
            ),
            frame_ids=ids,
        )
        target = out / test["id"]
        target.mkdir()
        previews = []
        for i, (a, b) in enumerate(zip(r, t)):
            stem = str(ids[i])
            Image.fromarray(a).save(target / f"{stem}_reference.png")
            Image.fromarray(b).save(target / f"{stem}_test.png")
            Image.fromarray(
                np.clip(np.abs(a.astype(float) - b.astype(float)) * 4, 0, 255).astype(
                    np.uint8
                )
            ).save(target / f"{stem}_diff4.png")
            if maps:
                e = maps[i]
                e = e[:, :, 0] if e.ndim == 3 else e
                Image.fromarray(np.round(np.clip(e, 0, 1) * 255).astype(np.uint8)).save(
                    target / f"{stem}_flip.png"
                )
            previews.append(
                {
                    "frame": ids[i],
                    "reference": f"{test['id']}/{stem}_reference.png",
                    "test": f"{test['id']}/{stem}_test.png",
                    "diff4": f"{test['id']}/{stem}_diff4.png",
                }
            )
        metrics["previews"] = previews
        if floor:
            other = next(s for s in floor if s["id"] == test["id"])
            if [f["frame"] for f in other["frames"]] != ids:
                raise EvalError("reference seed frame IDs differ")
            f = [
                transform(
                    within(reference["root"], x["image"]), other.get("transform", {})
                )
                for x in other["frames"]
            ]
            metrics["noise_floor"], _ = compare(
                r, f, test["fps"], test["mode"], wanted, perceptual
            )
        scores.append(metrics)
    versions = {}
    for pkg in ("numpy", "pillow", "cvvdp", "flip-evaluator", "torch", "OpenEXR"):
        try:
            versions[pkg] = importlib.metadata.version(pkg)
        except importlib.metadata.PackageNotFoundError:
            pass
    return {
        "schema": "kin.eval-result/1",
        "status": "ok",
        "size": job["profile"]["size"],
        "scores": scores,
        "metric_packages": versions,
        "display": job.get("display", {}),
    }


class Runner:
    def __init__(self, plan, out, resume=False, allow_unavailable=False):
        self.plan = plan
        self.out = Path(out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.allow_unavailable = allow_unavailable
        self.provenance = provenance(plan)
        self.key = fingerprint({"plan": plan, "provenance": self.provenance})
        manifest = self.out / "manifest.json"
        if manifest.exists():
            old = read_json(manifest)
            if not resume:
                raise EvalError(
                    "output already has a run; use --resume or a new directory"
                )
            if old["identity"] != self.key:
                raise EvalError(
                    "resume inputs/build/tools changed; use a new output directory"
                )
            self.run = old
        else:
            self.run = {
                "schema": "kin.eval-run/1",
                "identity": self.key,
                "plan": plan,
                "provenance": self.provenance,
                "machine": machine(),
                "jobs": [],
            }
            write_json(manifest, self.run)

    def perform(self, job):
        identity = fingerprint({"run": self.key, "job": job})
        label = f"{job['scene']['id']}-{job['kind']}-{job.get('variant','truth')}-{job.get('repeat',0)}-{identity[:12]}"
        folder = self.out / "jobs" / label
        folder.parent.mkdir(exist_ok=True)
        if cached(folder, identity):
            result = read_json(folder / "result.json")
            state = "cached"
        else:
            if folder.exists():
                attempts = self.out / "attempts"
                attempts.mkdir(exist_ok=True)
                folder.rename(attempts / f"{label}-{time.time_ns()}")
            folder.mkdir()
            write_json(folder / "job.json", job)
            started = time.time()
            try:
                if job["scene"]["id"] in self.provenance["unavailable"]:
                    raise Unsupported(
                        self.provenance["unavailable"][job["scene"]["id"]]
                    )
                if (
                    job["kind"] in ("bench", "capture")
                    and job["scene"]["adapter"] != "command"
                    and not job.get("binary")
                ):
                    raise Unsupported(
                        "build manifest has no executable for this scene adapter"
                    )
                if job.get("blocked_reason"):
                    raise Unsupported(job["blocked_reason"])
                if job.get("binary"):
                    observed = self.provenance["binaries"].get(
                        f"{job['variant']}/{job['scene']['adapter']}"
                    )
                    if not observed or not Path(job["binary"]).is_file():
                        raise Unsupported(
                            f"missing executable for {job['scene']['adapter']}"
                        )
                    if sha256(job["binary"]) != observed["sha256"]:
                        raise EvalError("binary changed during evaluation")
                with gpu_lock():
                    if job["kind"] in ("bench", "capture"):
                        result = adapters.capture_or_bench(job, folder)
                    elif job["kind"] == "truth":
                        from .cache import truth_identity

                        cache_key = truth_identity(job, self.provenance)
                        shared = (
                            Path(self.plan.get("cache", str(self.out / "cache")))
                            / "truth"
                            / cache_key
                        )
                        if cached(shared, cache_key):
                            shutil.copytree(shared, folder, dirs_exist_ok=True)
                            write_json(folder / "job.json", job)
                            result = read_json(folder / "result.json")
                            result["reference_cache_hit"] = True
                        else:
                            result = adapters.truth(job, folder)
                            validate_result(folder, result, job["profile"])
                            result["reference_cache_hit"] = False
                            result["reference_cache_key"] = cache_key
                            write_json(folder / "result.json", result)
                            shared.parent.mkdir(parents=True, exist_ok=True)
                            temporary = shared.with_name(
                                shared.name + f".partial-{os.getpid()}"
                            )
                            if temporary.exists():
                                shutil.rmtree(temporary)
                            shutil.copytree(folder, temporary)
                            write_json(
                                temporary / "complete.json",
                                {"key": cache_key, "artifacts": inventory(temporary)},
                            )
                            if shared.exists():
                                shutil.rmtree(shared)
                            temporary.rename(shared)
                    elif job["kind"] == "score":
                        python = self.plan["tools"].get("metrics_python")
                        if python:
                            from .common import execute

                            execute(
                                [
                                    python,
                                    "-I",
                                    str(HERE / "eval.py"),
                                    "_score",
                                    str(folder / "job.json"),
                                    str(folder),
                                ],
                                folder,
                            )
                            result = read_json(folder / "result.json")
                        else:
                            result = score_job(job, folder)
                    elif job["kind"] == "stress":
                        result = (
                            adapters.command(job, folder)
                            if job["scene"]["adapter"] == "command"
                            else self.stress(job, folder)
                        )
                    else:
                        raise EvalError("unknown job kind")
                if result.get("status") == "ok":
                    if job["kind"] == "bench" and not result.get("frames_file"):
                        raise EvalError("benchmark adapter emitted no frame records")
                    if job["kind"] == "capture" and not result.get("sequences"):
                        raise EvalError("capture adapter emitted no images")
                    if job["kind"] == "truth":
                        if not result.get("seeds") and not result.get("sequences"):
                            raise EvalError("truth adapter emitted no images")
                        if result.get("reference_role") == "cycles" and set(
                            result.get("seeds", {})
                        ) != set(map(str, job["profile"]["seeds"])):
                            raise EvalError(
                                "Cycles provider returned different seeds than requested"
                            )
                validate_result(
                    folder,
                    result,
                    dict(
                        job["profile"],
                        frames=result.get("stress", {}).get(
                            "measured_frames", job["profile"]["frames"]
                        ),
                    ),
                )
                if result.get("frames_file"):
                    result["performance"] = summarize_frames(
                        rows(folder / result["frames_file"])
                    )
                state = "completed"
            except Unsupported as e:
                result = {
                    "schema": "kin.eval-result/1",
                    "status": "unsupported",
                    "reason": str(e),
                }
                state = "unsupported"
            except (
                OSError,
                EvalError,
                ValueError,
                KeyError,
                TypeError,
                subprocess.SubprocessError,
            ) as e:
                result = {
                    "schema": "kin.eval-result/1",
                    "status": "failed",
                    "reason": str(e),
                }
                state = "failed"
            result["elapsed_s"] = time.time() - started
            write_json(folder / "result.json", result)
            if result["status"] != "failed":
                write_json(
                    folder / "complete.json",
                    {"key": identity, "artifacts": inventory(folder)},
                )
        entry = {
            "id": label,
            "kind": job["kind"],
            "scene": job["scene"]["id"],
            "variant": job.get("variant", "truth"),
            "repeat": job.get("repeat", 0),
            "folder": str(folder.relative_to(self.out)),
            "status": result["status"],
        }
        logical = ("kind", "scene", "variant", "repeat")
        self.run["jobs"] = [
            j
            for j in self.run["jobs"]
            if any(j.get(k) != entry.get(k) for k in logical)
        ] + [entry]
        write_json(self.out / "manifest.json", self.run)
        print(
            f"{state}: {label}"
            + (f" ({result.get('reason')})" if result.get("reason") else ""),
            flush=True,
        )
        return {
            "root": str(folder),
            "result": result,
            "artifact_identity": fingerprint(inventory(folder)),
        }

    def stress(self, job, folder):
        if job["scene"]["adapter"] != "reference":
            raise Unsupported(
                "persistent resize/feature stress currently uses the reference adapter; application adapters may supply a command stress provider"
            )
        capture = job.get("capture")
        if not capture or capture["result"]["status"] != "ok":
            raise Unsupported("stress needs a successful capture to resolve its camera")
        camera = capture["result"]["sequences"][0]["frames"][0]
        if not all(k in camera for k in ("eye", "target", "fov")):
            raise Unsupported("stress camera is unavailable")
        cycles = int(job["profile"].get("stress_cycles", 4))
        hold = int(job["profile"].get("stress_hold_frames", 130))
        settle = int(job["profile"].get("stress_settle_frames", 600))
        frames = []
        w, h = job["profile"]["size"]

        def phase(count, enabled, size):
            for f in range(count):
                step = {k: camera[k] for k in ("eye", "target", "fov")}
                step.update(taa=enabled, ao=enabled)
                if f == 0:
                    step.update(resize=size, cut=True)
                frames.append(step)

        # Exercise lazy feature allocations, then expire import/upload/pool slack
        # before establishing a baseline. Startup slack must not hide later leaks.
        phase(hold, True, [w, h])
        phase(settle, False, [w, h])
        baseline_end = len(frames)
        for i in range(cycles):
            phase(
                hold,
                bool(i % 2),
                [w, h] if i % 2 == 0 else [max(16, w // 2), max(16, h // 2)],
            )
        phase(settle, False, [w, h])
        derived = dict(job, kind="bench", stress_frames=frames)
        derived["profile"] = dict(job["profile"], frames=len(frames), warmup=0)
        result = adapters.reference(derived, folder)
        data = rows(folder / result["frames_file"])
        first = [
            r["memory"]["owned_bytes"]
            for r in data[max(hold, baseline_end - 30) : baseline_end]
        ]
        final = [r["memory"]["owned_bytes"] for r in data[-min(30, settle) :]]
        tolerance = int(job["profile"].get("stress_tolerance_bytes", 4096))
        initial_stable = max(first) - min(first) <= tolerance
        final_stable = max(final) - min(final) <= tolerance
        settled = (
            initial_stable and final_stable and max(final) <= max(first) + tolerance
        )
        result["stress"] = {
            "kind": "persistent_view_resize_taa_ao",
            "cycles": cycles,
            "hold_frames": hold,
            "prewarm_frames": hold,
            "settle_frames": settle,
            "baseline_end_frame": baseline_end,
            "initial_plateau_stable": initial_stable,
            "final_plateau_stable": final_stable,
            "measured_frames": len(frames),
            "initial_plateau_bytes": max(first),
            "final_plateau_bytes": max(final),
            "tolerance_bytes": tolerance,
            "owned_plateau_settled": settled,
            "unexercised": [
                "asset_reload",
                "shader_reload",
                "scene_load_unload",
                "multiple_views",
            ],
        }
        if not settled:
            raise EvalError(
                f"owned payload did not return to its initial plateau: {max(first)} -> {max(final)} bytes"
            )
        return result

    def execute(self, stages=None):
        stages = stages or self.plan["profile"]["stages"]
        self.run["requested_stages"] = stages
        captures = {}
        truths = {}
        for entry in self.run["jobs"]:
            if entry["kind"] in ("capture", "truth"):
                folder = self.out / entry["folder"]
                try:
                    marker = read_json(folder / "complete.json")
                    if not cached(folder, marker["key"]):
                        continue
                except (OSError, KeyError, ValueError):
                    continue
                value = {
                    "root": str(folder),
                    "result": read_json(folder / "result.json"),
                    "artifact_identity": fingerprint(inventory(folder)),
                }
                if entry["kind"] == "capture":
                    captures[entry["scene"], entry["variant"]] = value
                else:
                    truths[entry["scene"]] = value

        def base(scene, kind, label=None, repeat=0):
            build = self.plan["builds"].get(label, {})
            binary = build.get("executables", {}).get(scene["adapter"])
            return dict(
                schema="kin.eval-job/1",
                kind=kind,
                scene=scene,
                profile=self.plan["profile"],
                variant=label or "truth",
                repeat=repeat,
                build=build,
                binary=binary,
                tools=self.plan["tools"],
                display=self.plan["display"],
            )

        for kind in ("bench", "capture", "truth", "score", "stress"):
            if kind not in stages:
                continue
            for repeat in range(self.plan["profile"]["runs"] if kind == "bench" else 1):
                labels = list(self.plan["builds"])
                if repeat % 2:
                    labels.reverse()
                for scene in self.plan["scenes"]:
                    sid = scene["id"]
                    if kind == "truth":
                        if scene.get("reference", {}).get("provider") == "baseline":
                            continue
                        source = next(
                            (
                                v
                                for (s, _), v in captures.items()
                                if s == sid and v["result"]["status"] == "ok"
                            ),
                            None,
                        )
                        job = base(scene, kind)
                        if source:
                            job["capture"] = source["result"]
                        else:
                            job["blocked_reason"] = (
                                "truth requires a successful capture job"
                            )
                        truths[sid] = self.perform(job)
                        continue
                    for label in labels:
                        job = base(scene, kind, label, repeat)
                        if kind == "score":
                            capture = captures.get((sid, label))
                            if not capture or capture["result"]["status"] != "ok":
                                job["blocked_reason"] = (
                                    "scoring requires a successful capture job"
                                )
                                self.perform(job)
                                continue
                            if scene.get("reference", {}).get("provider") == "baseline":
                                reference = captures.get((sid, "baseline"))
                            else:
                                reference = truths.get(sid)
                            if not reference or reference["result"]["status"] != "ok":
                                job["blocked_reason"] = (
                                    "scoring requires its declared baseline or Cycles reference"
                                )
                            else:
                                job.update(capture=capture, reference=reference)
                        if kind == "stress":
                            job["capture"] = captures.get((sid, label))
                        value = self.perform(job)
                        if kind == "capture":
                            captures[sid, label] = value
        # Do not silently bless content that changed while GPU jobs were running.
        if provenance(self.plan) != self.provenance:
            self.run["inputs_changed"] = True
        self.run["status"] = (
            "failed"
            if self.run.get("inputs_changed")
            or any(j["status"] == "failed" for j in self.run["jobs"])
            else (
                "incomplete"
                if any(j["status"] == "unsupported" for j in self.run["jobs"])
                else "ok"
            )
        )
        write_json(self.out / "manifest.json", self.run)
        return self.run
