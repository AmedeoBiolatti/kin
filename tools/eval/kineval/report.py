"""Offline, self-contained reports. Repeated runs remain separate observations."""

from pathlib import Path
import html
import json
import random
import shutil
import statistics
from .common import fingerprint, read_json, rows, within, write_json


def paired_change(baseline, candidate):
    pairs = [
        (baseline[k], candidate[k])
        for k in sorted(set(baseline) & set(candidate))
        if baseline[k] > 0
    ]
    if not pairs:
        return None
    changes = [b / a - 1 for a, b in pairs]
    estimate = statistics.median(changes)
    if len(pairs) < 3:
        return {
            "percent": 100 * estimate,
            "runs": len(pairs),
            "interval_percent": None,
            "status": "diagnostic",
        }
    rng = random.Random(19)
    draws = sorted(
        statistics.median(rng.choices(changes, k=len(changes))) for _ in range(2000)
    )
    return {
        "percent": 100 * estimate,
        "runs": len(pairs),
        "interval_percent": [100 * draws[50], 100 * draws[1949]],
        "status": "paired_bootstrap",
    }


def generate(root):
    root = Path(root)
    run = read_json(root / "manifest.json")
    out = root / "report"
    out.mkdir(exist_ok=True)
    media = out / "media"
    media.mkdir(exist_ok=True)
    e = lambda value: html.escape(str(value))
    perf = {}
    quality = []
    unavailable = []
    traces = []
    stresses = []
    resources = []
    for job in run["jobs"]:
        folder = within(root, job["folder"])
        result = read_json(folder / "result.json")
        if result["status"] != "ok":
            unavailable.append((job, result.get("reason", "")))
            continue
        for resource_file in folder.rglob("resources.jsonl"):
            samples = rows(resource_file)
            rss = [r.get("VmRSS_bytes", 0) for r in samples]
            resources.append(
                {
                    "scene": job["scene"],
                    "kind": job["kind"],
                    "variant": job["variant"],
                    "peak_sampled_rss_bytes": max(rss, default=0),
                    "samples": len(samples),
                    "source": str(resource_file.relative_to(root)),
                }
            )
        if job["kind"] == "stress":
            stresses.append(
                {
                    "scene": job["scene"],
                    "variant": job["variant"],
                    **result.get("stress", {}),
                }
            )
        if job["kind"] == "bench":
            for view, value in result.get("performance", {}).items():
                key = (job["scene"], view, job["variant"])
                perf.setdefault(key, []).append((job["repeat"], value))
            if result.get("frames_file"):
                data = rows(folder / result["frames_file"])
                values = [r.get("cpu", {}).get("interval_ms", 0) for r in data]
                if values:
                    step = max(1, len(values) // 300)
                    values = values[::step]
                    maximum = max(max(values), 0.01)
                    points = " ".join(
                        f"{i*600/max(1,len(values)-1):.2f},{110-v/maximum*100:.2f}"
                        for i, v in enumerate(values)
                    )
                    traces.append(
                        f'<details><summary>{e(job["scene"])} / {e(job["variant"])} / run {job["repeat"]}: frame intervals</summary><svg viewBox="0 0 600 120"><polyline fill="none" stroke="#71c8ff" stroke-width="1" points="{points}"/></svg><p>0–{maximum:.3f} ms; preview downsampled, raw records retained.</p></details>'
                    )
        elif job["kind"] == "score":
            for score in result.get("scores", []):
                quality.append((job, folder, score))
    lines = [
        '<!doctype html><meta charset="utf-8"><title>Kin evaluation</title>',
        "<style>body{font:15px system-ui;background:#161a20;color:#e6eaf0;margin:28px}table{border-collapse:collapse;width:100%;margin:18px 0}td,th{text-align:left;padding:8px;border-bottom:1px solid #3c4755}a{color:#7fcaff}.row{display:flex;gap:8px}.row figure{flex:1;margin:0;min-width:0}.row img{width:100%}summary{cursor:pointer}svg{max-width:100%;width:600px}code{overflow-wrap:anywhere}</style>",
        f'<h1>Kin evaluation: {e(run.get("status","running"))}</h1><p>Profile: {e(run["plan"]["profile_name"])}. Run identity: <code>{e(run["identity"])}</code>.</p>',
        "<p>CPU values are milliseconds. GPU fence estimates include possible CPU recording gaps and have no per-frame percentile claim. Memory is Kin-owned descriptor payload, excluding hidden driver storage. Stills and clip JOD are separate metrics. Cycles seed comparisons estimate reference noise.</p>",
        "<h2>Performance</h2><table><tr><th>Scene / view</th><th>Variant</th><th>Runs</th><th>Frame p50</th><th>Frame p95</th><th>Frame p99</th><th>Peak owned MiB</th><th>p95 vs baseline</th></tr>",
    ]
    comparisons = []
    for (scene, view, variant), values in sorted(perf.items()):

        def median(key):
            xs = [
                v["cpu"]["interval_ms"][key]
                for _, v in values
                if v["cpu"].get("interval_ms")
            ]
            return statistics.median(xs) if xs else None

        memory = (
            max(
                (v["memory"].get("owned_bytes", {}).get("peak", 0) for _, v in values),
                default=0,
            )
            / 1048576
        )
        change = None
        if variant != "baseline" and (scene, view, "baseline") in perf:
            a = {
                r: v["cpu"]["interval_ms"]["p95"]
                for r, v in perf[scene, view, "baseline"]
                if v["cpu"].get("interval_ms")
            }
            b = {
                r: v["cpu"]["interval_ms"]["p95"]
                for r, v in values
                if v["cpu"].get("interval_ms")
            }
            change = paired_change(a, b)
            if change:
                interval = change.get("interval_percent")
                limit = run["plan"]["thresholds"].get("performance_review_percent", 5)
                change["review_regression"] = bool(
                    interval and interval[0] > 0 and change["percent"] > limit
                )
                comparisons.append(
                    dict(scene=scene, view=view, variant=variant, **change)
                )
        text = "—" if not change else f'{change["percent"]:+.2f}% ({change["status"]})'
        numbers = [
            "—" if median(k) is None else f"{median(k):.3f}"
            for k in ("p50", "p95", "p99")
        ]
        lines.append(
            f"<tr><td>{e(scene)} / {e(view)}</td><td>{e(variant)}</td><td>{len(values)}</td>"
            + "".join(f"<td>{n}</td>" for n in numbers)
            + f"<td>{memory:.2f}</td><td>{e(text)}</td></tr>"
        )
    lines.append(
        "</table><p>Rows summarize per-run percentiles; they do not pool unrelated runs. Short profiles are diagnostic. Paired uncertainty uses repeated runs, not individual frames as independent samples.</p>"
    )
    lines.extend(traces)
    lines.append("<h2>Quality</h2>")

    def copy_image(folder, relative):
        source = within(folder, relative)
        name = fingerprint(str(source))[:16] + source.suffix
        shutil.copyfile(source, media / name)
        return "media/" + name

    for job, folder, score in quality:
        title = f'{job["scene"]} / {score["sequence"]} / {job["variant"]}'
        lines.append(
            f'<section><h3>{e(title)}</h3><p>Reference: {e(score["reference_role"])}; mode: {e(score["mode"])}; {score["frames"]} frames at {score["fps"]} fps.</p>'
        )
        lines.append(
            "<table><tr><th>Metric</th><th>Candidate</th><th>Cycles seed comparison</th></tr>"
        )
        for key, value in score["summary"].items():
            floor = score.get("noise_floor", {}).get("summary", {}).get(key)
            lines.append(
                f'<tr><td>{e(key)}</td><td>{e(value)}</td><td>{e(floor) if floor is not None else "—"}</td></tr>'
            )
        lines.append("</table>")
        for index, preview in enumerate(score.get("previews", [])):
            lines.append(
                f'<details {"open" if index==0 else ""}><summary>Frame {preview["frame"]}</summary><div class="row">'
            )
            for key, label in (
                ("reference", "Reference"),
                ("test", "Candidate"),
                ("diff4", "Absolute difference ×4"),
            ):
                link = copy_image(folder, preview[key])
                lines.append(
                    f'<figure><img src="{e(link)}"><figcaption>{label}</figcaption></figure>'
                )
            lines.append("</div></details>")
        lines.append("</section>")
    lines.append(
        "<h2>Persistent memory stress</h2><pre>"
        + e(json.dumps(stresses, indent=2))
        + "</pre>"
    )
    lines.append(
        "<h2>Process resource observations</h2><p>RSS is sampled at one-second intervals where /proc is available. Device memory/power/thermal samples in the raw logs are whole-device observations, not per-process allocations. Short jobs can finish before a sample is taken.</p><pre>"
        + e(json.dumps(resources, indent=2))
        + "</pre>"
    )
    lines.append("<h2>Unavailable or failed work</h2>")
    if not unavailable:
        lines.append("<p>None.</p>")
    for job, reason in unavailable:
        lines.append(
            f'<p><strong>{e(job["scene"])} / {e(job["kind"])} / {e(job["variant"])}</strong>: {e(reason)}</p>'
        )
    lines.append(
        "<h2>Provenance</h2><pre>"
        + e(json.dumps(run.get("machine", {}), indent=2))
        + "</pre>"
    )
    (out / "index.html").write_text("\n".join(lines), encoding="utf-8")
    summary = {
        "schema": "kin.eval-report/1",
        "status": run.get("status"),
        "run_identity": run["identity"],
        "comparisons": comparisons,
        "jobs": len(run["jobs"]),
        "unavailable_or_failed": len(unavailable),
        "quality_cases": len(quality),
        "stress": stresses,
        "resources": resources,
    }
    write_json(out / "summary.json", summary)
    (out / "summary.md").write_text(
        f'# Kin evaluation\n\nStatus: {run.get("status")}\n\n{len(run["jobs"])} jobs; {len(quality)} quality cases; {len(unavailable)} unavailable/failed.\n\nSee index.html and summary.json.\n',
        encoding="utf-8",
    )
    return out / "index.html"
