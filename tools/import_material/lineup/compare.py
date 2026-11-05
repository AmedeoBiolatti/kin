"""Compares two lineup pictures object by object (lineup.json's layout): mean and largest difference in 8-bit levels,
the share of pixels off by more than 2 levels, PSNR, and (with flip_evaluator installed) FLIP; a sheet of the two
and their difference x8.

    python3 compare.py <lineup.json> <reference.png> <candidate.png> <sheet.png> [--json out.json]
"""
import json
import math
import sys

import numpy as np
from PIL import Image, ImageDraw


def boxes(scene):
    """Each object's box in pixels: its projected bounds (quads 1.6 m, spheres 0.8 m radius), slightly inset."""
    w, h = scene["size"]
    cam = scene["camera"]
    eye = np.array(cam["eye"], float)
    f = (h / 2) / math.tan(math.radians(cam["fov_y"]) / 2)
    out = []
    for o in scene["objects"]:
        p = np.array(o["position"], float)
        r = 0.8
        corners = [p + np.array([sx * r, sy * r, sz * r]) for sx in (-1, 1) for sy in (-1, 1)
                   for sz in ((0,) if o["mesh"] == "quad" else (-1, 1))]
        xs, ys = [], []
        for c in corners:
            d = c - eye
            xs.append(w / 2 + f * d[0] / -d[2])
            ys.append(h / 2 - f * d[1] / -d[2])
        inset = 3
        out.append((o["material"], o["mesh"], int(min(xs)) + inset, int(min(ys)) + inset,
                    int(math.ceil(max(xs))) - inset, int(math.ceil(max(ys))) - inset))
    return out


def main():
    scene = json.load(open(sys.argv[1]))
    a = np.asarray(Image.open(sys.argv[2]).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(sys.argv[3]).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        sys.exit(f"sizes differ: {a.shape} {b.shape}")
    try:
        import flip_evaluator as flip
        fa, fb = a.astype(np.float32) / 255, b.astype(np.float32) / 255
        flip_map = flip.evaluate(fa, fb, "LDR")[0]
    except Exception:
        flip_map = None
    d = np.abs(a - b)
    rows = []
    for name, mesh, x0, y0, x1, y1 in boxes(scene):
        ra, rb, rd = a[y0:y1, x0:x1], b[y0:y1, x0:x1], d[y0:y1, x0:x1]
        lit = (ra.max(axis=2) > 4) | (rb.max(axis=2) > 4)  # the object, not the black around a sphere
        px = rd[lit]
        mse = float(((ra[lit] - rb[lit]) ** 2).mean()) if px.size else 0.0
        row = {
            "material": name, "mesh": mesh, "pixels": int(lit.sum()),
            "mean": float(px.mean()) if px.size else 0.0, "max": int(px.max()) if px.size else 0,
            "over2": float((px.max(axis=1) > 2).mean()) if px.size else 0.0,
            "psnr": 99.0 if mse == 0 else 10 * math.log10(255 ** 2 / mse),
            "mean_ref": [float(c) for c in ra[lit].mean(axis=0)] if px.size else [0, 0, 0],
            "mean_cand": [float(c) for c in rb[lit].mean(axis=0)] if px.size else [0, 0, 0],
        }
        if flip_map is not None:
            row["flip"] = float(flip_map[y0:y1, x0:x1][lit].mean())
        rows.append(row)
    print(f"{'material':<11} {'mesh':<6} {'mean':>6} {'max':>4} {'>2 lv':>7} {'PSNR':>6} {'FLIP':>6}   mean RGB ref -> cand")
    for r in rows:
        fl = f"{r['flip']:.4f}" if "flip" in r else "  -   "
        print(f"{r['material']:<11} {r['mesh']:<6} {r['mean']:6.2f} {r['max']:4d} {100 * r['over2']:6.2f}% "
              f"{r['psnr']:6.1f} {fl}   {tuple(round(c) for c in r['mean_ref'])} -> "
              f"{tuple(round(c) for c in r['mean_cand'])}")
    h, w = a.shape[:2]
    sheet = Image.new("RGB", (w, h * 3))
    sheet.paste(Image.fromarray(a.astype(np.uint8)), (0, 0))
    sheet.paste(Image.fromarray(b.astype(np.uint8)), (0, h))
    sheet.paste(Image.fromarray(np.clip(d * 8, 0, 255).astype(np.uint8)), (0, 2 * h))
    draw = ImageDraw.Draw(sheet)
    for i, label in enumerate((sys.argv[2], sys.argv[3], "difference x 8")):
        draw.text((8, i * h + 6), label.split("/")[-1], fill=(255, 255, 255))
    sheet.save(sys.argv[4])
    if "--json" in sys.argv:
        json.dump(rows, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=1)


if __name__ == "__main__":
    main()
