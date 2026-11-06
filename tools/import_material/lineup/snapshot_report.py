"""Unreal's and kin's snapshots of the same materials (unreal_snapshots.py, kin_material_lineup --material), compared:
per material and object (snapshot.json's quad and sphere) compare.py's measures (mean and largest difference in 8-bit
levels, the share off by more than 2 levels, PSNR, FLIP with flip_evaluator), as a table (report.md, report.json), and
a sheet, a row a material: Unreal's picture, kin's, their difference x 4.

    python3 snapshot_report.py <snapshot.json> <list> <unreal folder> <kin folder> <out folder>

<list> is unreal_snapshots.py's (a material a line, then its note); pictures are named <path, / as __>.png.
"""
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare import boxes  # noqa: E402


def measure(a, b, box, flip_map):
    x0, y0, x1, y1 = box
    ra, rb = a[y0:y1, x0:x1], b[y0:y1, x0:x1]
    lit = (ra.max(axis=2) > 4) | (rb.max(axis=2) > 4)
    px = np.abs(ra - rb)[lit]
    if not px.size:
        return None
    mse = float(((ra[lit] - rb[lit]).astype(np.float64) ** 2).mean())
    row = {"pixels": int(lit.sum()), "mean": float(px.mean()), "max": int(px.max()),
           "over2": float((px.max(axis=1) > 2).mean()), "psnr": 99.0 if mse == 0 else 10 * math.log10(255 ** 2 / mse),
           "mean_unreal": [round(float(c)) for c in ra[lit].mean(axis=0)],
           "mean_kin": [round(float(c)) for c in rb[lit].mean(axis=0)]}
    if flip_map is not None:
        row["flip"] = float(flip_map[y0:y1, x0:x1][lit].mean())
    return row


def main():
    scene_path, list_path, unreal_dir, kin_dir, out_dir = sys.argv[1:6]
    scene = json.load(open(scene_path))
    os.makedirs(out_dir, exist_ok=True)
    entries = []
    for line in open(list_path):
        if line.strip() and not line.startswith("#"):
            path, *note = line.split()
            entries.append((path, " ".join(note)))
    try:
        import flip_evaluator as flip
    except ImportError:
        flip = None
    w, h = scene["size"]
    tw, th = w // 2, h // 2
    label_h = 22
    sheet = Image.new("RGB", (tw * 3, (th + label_h) * len(entries)), (24, 24, 28))
    draw = ImageDraw.Draw(sheet)
    report = []
    for i, (path, note) in enumerate(entries):
        name = path.replace("/", "__") + ".png"
        y = i * (th + label_h)
        draw.text((6, y + 5), f"{path.rsplit('/', 1)[-1]}  [{note}]", fill=(255, 255, 255))
        files = [os.path.join(unreal_dir, name), os.path.join(kin_dir, name)]
        if not all(os.path.exists(f) for f in files):
            draw.text((tw * 3 // 2, y + 5), "missing: " + ", ".join(f for f in files if not os.path.exists(f)),
                      fill=(255, 120, 100))
            report.append({"material": path, "note": note, "missing": True})
            continue
        a, b = (np.asarray(Image.open(f).convert("RGB"), dtype=np.int16) for f in files)
        if a.shape != b.shape:
            sys.exit(f"{path}: sizes differ {a.shape} {b.shape}")
        flip_map = None
        if flip is not None:
            flip_map = flip.evaluate(a.astype(np.float32) / 255, b.astype(np.float32) / 255, "LDR",
                                     applyMagma=False)[0][..., 0]  # the error, not its colour map
        rows = {}
        for _, mesh, x0, y0, x1, y1 in boxes(scene):
            rows[mesh] = measure(a, b, (x0, y0, x1, y1), flip_map)
        report.append({"material": path, "note": note, "objects": rows})
        diff = np.clip(np.abs(a - b) * 4, 0, 255).astype(np.uint8)
        for k, img in enumerate((a.astype(np.uint8), b.astype(np.uint8), diff)):
            sheet.paste(Image.fromarray(img).resize((tw, th), Image.LANCZOS), (k * tw, y + label_h))
        for k, label in enumerate(("Unreal", "kin", "difference x 4")):
            draw.text((k * tw + 6, y + label_h + 4), label, fill=(200, 200, 200))
    sheet.save(os.path.join(out_dir, "sheet.png"))
    json.dump(report, open(os.path.join(out_dir, "report.json"), "w"), indent=1)

    lines = ["| material | note | object | mean | max | >2 lv | PSNR | FLIP | mean RGB Unreal -> kin |",
             "|---|---|---|---:|---:|---:|---:|---:|---|"]
    for r in report:
        short = r["material"].rsplit("/", 1)[-1]
        if r.get("missing"):
            lines.append(f"| {short} | {r['note']} | missing | | | | | | |")
            continue
        for mesh, m in r["objects"].items():
            if m is None:
                lines.append(f"| {short} | {r['note']} | {mesh} | black in both | | | | | |")
                continue
            fl = f"{m['flip']:.3f}" if "flip" in m else "-"
            lines.append(f"| {short} | {r['note']} | {mesh} | {m['mean']:.1f} | {m['max']} | {100 * m['over2']:.0f}% | "
                         f"{m['psnr']:.1f} | {fl} | {tuple(m['mean_unreal'])} -> {tuple(m['mean_kin'])} |")
    open(os.path.join(out_dir, "report.md"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))

    # Each material's verdict, worst first: close (mean difference within 4 levels on the quad and 8 on the sphere, whose
    # edge's pixels differ the most), minor (10 and 15), else major; "not a surface" where the list says so (a post
    # process's or a volume's material: a quad and a sphere don't picture it).
    def verdict(r):
        if r.get("missing"):
            return "missing", 1e9
        if "not-surface" in r["note"].split():
            return "not a surface", -1.0
        q, sph = r["objects"].get("quad"), r["objects"].get("sphere")
        qm = q["mean"] if q else 0.0
        sm = sph["mean"] if sph else 0.0
        kind = "close" if qm <= 4 and sm <= 8 else "minor" if qm <= 10 and sm <= 15 else "major"
        return kind, max(qm, sm / 2)
    ranked = sorted(report, key=lambda r: -verdict(r)[1])
    counts = {}
    out = ["| verdict | material | note | quad mean | sphere mean | quad FLIP | sphere FLIP |", "|---|---|---|---:|---:|---:|---:|"]
    for r in ranked:
        kind, _ = verdict(r)
        counts[kind] = counts.get(kind, 0) + 1
        if r.get("missing"):
            out.append(f"| missing | {r['material']} | {r['note']} | | | | |")
            continue
        q, sph = r["objects"].get("quad") or {}, r["objects"].get("sphere") or {}
        cell = lambda o, k, f: (f % o[k]) if o and k in o else "-"
        out.append(f"| {kind} | {r['material'].rsplit('/', 1)[-1]} | {r['note']} | {cell(q, 'mean', '%.1f')} | "
                   f"{cell(sph, 'mean', '%.1f')} | {cell(q, 'flip', '%.3f')} | {cell(sph, 'flip', '%.3f')} |")
    summary = ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))
    open(os.path.join(out_dir, "ranked.md"), "w").write(summary + "\n\n" + "\n".join(out) + "\n")
    print("\n" + summary)


if __name__ == "__main__":
    main()
