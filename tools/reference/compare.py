"""Two renders of the reference scenes compared (docs/3d_plan.md, G0): kin
before and after a change, or kin against Cycles' truth.

    python3 tools/reference/compare.py <a dir> <b dir> --out <report dir> [--title T]

Each dir is kin_reference's output (results.json, <scene>/<camera>.png) or
blender_truth.py's (pictures only). For each picture in both: the mean
difference a channel (levels), PSNR (dB), SSIM (on luminance, 11 x 11
Gaussian), p99 of the difference, the share of pixels more than 8 levels
apart, and a difference image (x4). Where both have times: render() on the
CPU and the GPU, medians, and whether the change is larger than the noise
(each side's p10-p90 spread). Writes index.html and summary.md; prints the
summary. Needs numpy and Pillow.
"""
import argparse
import html
import json
import os

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)


def gaussian(image, sigma=1.5, radius=5):
    x = np.arange(-radius, radius + 1)
    k = np.exp(-(x * x) / (2 * sigma * sigma))
    k /= k.sum()
    out = np.zeros_like(image)
    padded = np.pad(image, ((radius, radius), (0, 0)), mode="reflect")
    for i, w in enumerate(k):
        out += w * padded[i:i + image.shape[0], :]
    result = np.zeros_like(image)
    padded = np.pad(out, ((0, 0), (radius, radius)), mode="reflect")
    for i, w in enumerate(k):
        result += w * padded[:, i:i + image.shape[1]]
    return result


def ssim(a, b):
    ya = a @ np.array([0.299, 0.587, 0.114])
    yb = b @ np.array([0.299, 0.587, 0.114])
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ma, mb = gaussian(ya), gaussian(yb)
    va = gaussian(ya * ya) - ma * ma
    vb = gaussian(yb * yb) - mb * mb
    cov = gaussian(ya * yb) - ma * mb
    s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))
    return float(s.mean())


def metrics(a, b):
    d = np.abs(a - b)
    mse = float((d * d).mean())
    worst = d.max(axis=2)
    return {
        "mean": float(d.mean()),
        "psnr": float("inf") if mse == 0 else 10 * np.log10(255.0 ** 2 / mse),
        "ssim": ssim(a, b),
        "p99": float(np.percentile(d, 99)),
        "over8": float((worst > 8).mean() * 100),
    }, d


def results(folder):
    path = os.path.join(folder, "results.json")
    if not os.path.isfile(path):
        return {}
    data = json.load(open(path))
    return {(s["name"], c["name"]): c for s in data["scenes"] for c in s["cameras"]}


def pictures(folder):
    found = []
    for scene in sorted(os.listdir(folder)):
        d = os.path.join(folder, scene)
        if os.path.isdir(d):
            found += [(scene, f[:-4]) for f in sorted(os.listdir(d)) if f.endswith(".png")]
    return found


def changed(ta, tb):
    """b's median against a's, and whether apart by more than either's p10-p90 spread."""
    if not ta or not tb or ta["median"] <= 0 or tb["median"] <= 0:
        return None, False
    spread = max(ta["p90"] - ta["p10"], tb["p90"] - tb["p10"])
    return tb["median"] / ta["median"], abs(tb["median"] - ta["median"]) > spread


def main():
    p = argparse.ArgumentParser()
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--out", required=True)
    p.add_argument("--title", default=None)
    o = p.parse_args()
    os.makedirs(o.out, exist_ok=True)
    ra, rb = results(o.a), results(o.b)
    both = [k for k in pictures(o.a) if os.path.isfile(os.path.join(o.b, k[0], k[1] + ".png"))]
    title = o.title or f"{os.path.basename(os.path.normpath(o.a))} vs {os.path.basename(os.path.normpath(o.b))}"
    rows, cards = [], []
    for scene, camera in both:
        a = load(os.path.join(o.a, scene, camera + ".png"))
        b = load(os.path.join(o.b, scene, camera + ".png"))
        if a.shape != b.shape:
            rows.append(f"| {scene}/{camera} | sizes differ | | | | | | |")
            continue
        m, d = metrics(a, b)
        diff_name = f"{scene}_{camera}_diff.png"
        Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(os.path.join(o.out, diff_name))
        times = ""
        ta, tb = ra.get((scene, camera)), rb.get((scene, camera))
        cells = []
        for key in ("cpu_ms", "gpu_ms"):
            if ta and tb:
                ratio, real = changed(ta[key], tb[key])
                mark = "" if ratio is None else (f"x{ratio:.2f}" + (" *" if real else ""))
                cells.append(f"{ta[key]['median']:.3f} / {tb[key]['median']:.3f} {mark}")
            elif tb or ta:
                t = tb or ta
                cells.append(f"{t[key]['median']:.3f}")
            else:
                cells.append("")
        psnr = "identical" if m["psnr"] == float("inf") else f"{m['psnr']:.1f}"
        rows.append(f"| {scene}/{camera} | {m['mean']:.2f} | {psnr} | {m['ssim']:.4f} | {m['p99']:.0f} | {m['over8']:.2f}% | {cells[0]} | {cells[1]} |")
        rel = lambda folder: os.path.relpath(os.path.join(folder, scene, camera + ".png"), o.out)  # noqa: E731
        cards.append(f"""<section><h2>{html.escape(scene)} / {html.escape(camera)}</h2>
<p>mean {m['mean']:.2f} levels, PSNR {psnr} dB, SSIM {m['ssim']:.4f}, p99 {m['p99']:.0f}, {m['over8']:.2f}% of pixels over 8 levels apart.
CPU {html.escape(cells[0])} ms, GPU {html.escape(cells[1])} ms.</p>
<div class="row"><figure><img src="{html.escape(rel(o.a))}"><figcaption>a</figcaption></figure>
<figure><img src="{html.escape(rel(o.b))}"><figcaption>b</figcaption></figure>
<figure><img src="{html.escape(diff_name)}"><figcaption>difference x4</figcaption></figure></div></section>""")
    header = ("| picture | mean (levels) | PSNR (dB) | SSIM | p99 | over 8 | render() CPU ms a / b | GPU ms a / b |\n"
              "|---|---|---|---|---|---|---|---|")
    summary = f"# {title}\n\na: `{o.a}`\nb: `{o.b}`\n\n{header}\n" + "\n".join(rows) + \
        "\n\nTimes: medians; `*` where the change is larger than either side's p10-p90 spread.\n"
    open(os.path.join(o.out, "summary.md"), "w").write(summary)
    page = f"""<!doctype html><html><head><meta charset="utf-8"><title>{html.escape(title)}</title>
<style>body{{font:14px system-ui,sans-serif;background:#16181c;color:#e6e6e6;margin:24px}}
.row{{display:flex;gap:8px;flex-wrap:wrap}} figure{{margin:0;flex:1;min-width:300px}} img{{width:100%;display:block}}
figcaption{{color:#9aa}} section{{margin-bottom:28px}} pre{{white-space:pre-wrap}}</style></head>
<body><h1>{html.escape(title)}</h1><pre>{html.escape(header + chr(10) + chr(10).join(rows))}</pre>{''.join(cards)}</body></html>"""
    open(os.path.join(o.out, "index.html"), "w").write(page)
    print(summary)


if __name__ == "__main__":
    main()
