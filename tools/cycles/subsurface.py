"""kin's subsurface scattering against Cycles': how far each renderer spreads light past a shadow's edge.

    python3 tools/cycles/subsurface.py <sharp snapshot dir> <scattering snapshot dir> [--rows 150:210]

Both folders are snapshots of the `subsurface` reference scene's `edge` camera (kin_reference --snapshot DIR, once
without and once with --subsurface ...), each rendered by tools/cycles/compare.py (so cycles/cycles.png is beside
kin.png). The scene's slab has a straight shadow edge along the picture's columns. For each renderer and colour
channel, the columns' light (a band of rows, in linear light: the view is HDR without a tonemap, its PNG sRGB) is
fitted as the sharp picture's spread by Burley's profile R(r) = (e^(-r/d) + e^(-r/3d)) / (8 pi d r) across the edge
(its line spread, the profile summed along the edge), d in pixels, with a gain (Cycles' albedo inversion need not keep
the light exactly). Each renderer is fitted against its own sharp picture, so their shadows' penumbras and lighting
cancel: the spread each adds should not differ. Also printed: d in metres (the band's pixels a metre on the slab, from
the snapshot's camera) and the d the material asked for (scattering scene.json's subsurface_radius).
Needs numpy and Pillow.
"""
import argparse
import json
import math
import os

import numpy as np
from PIL import Image


def linear(path):
    c = np.asarray(Image.open(path).convert("RGB")).astype(np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def line_spread(d, half):
    """Burley's profile for d (pixels) summed along the edge: its spread across it, x = -half..half (sums to 1)."""
    if d < 0.05:
        k = np.zeros(2 * half + 1)
        k[half] = 1.0
        return k
    fine = 4  # sub-pixel steps
    x = (np.arange(-half * fine, half * fine + 1) + 0.0) / fine
    y = x[:, None]
    r = np.sqrt(x[None, :] ** 2 + y**2)
    r = np.maximum(r, 0.25 / fine)
    R = (np.exp(-r / d) + np.exp(-r / (3 * d))) / (8 * math.pi * d * r)
    across = R.sum(axis=0)
    # back to whole pixels: each pixel the sum of its fine steps
    k = np.add.reduceat(across[:-1], np.arange(0, len(across) - 1, fine))
    k = np.append(k, across[-1])[: 2 * half + 1]
    return k / k.sum()


def fit(sharp, spread, edge, window, max_d):
    """The d (pixels) and gain whose spread of `sharp` best matches `spread` within `window` of `edge`."""
    half = int(max_d * 8)
    padded = np.pad(sharp, half, mode="edge")
    lo, hi = max(edge - window, 0), min(edge + window, len(sharp))
    best = (1e30, 0.0, 1.0)
    for d in np.arange(0.0, max_d + 1e-9, 0.25):
        blurred = np.convolve(padded, line_spread(d, half), mode="same")[half:-half]
        a, b = blurred[lo:hi], spread[lo:hi]
        gain = float((a * b).sum() / max((a * a).sum(), 1e-12))
        err = float(((a * gain - b) ** 2).sum())
        if err < best[0]:
            best = (err, d, gain)
    return best[1], best[2]


def pixels_a_metre(doc, row, height):
    """Pixels a metre across the slab (y = 0) at a row of the picture, from the snapshot's camera."""
    cam = doc["camera"]
    eye, fwd = np.array(cam["position"]), np.array(cam["forward"])
    up = np.array(cam.get("up", [0, 1, 0]))
    right = np.cross(fwd, up)
    right /= np.linalg.norm(right)
    up = np.cross(right, fwd)
    t = math.tan(math.radians(cam["fov"]) / 2)
    ndc_y = 1 - 2 * (row + 0.5) / height
    ray = fwd + up * ndc_y * t
    hit = eye + ray * (-eye[1] / ray[1])
    depth = float(np.dot(hit - eye, fwd))
    return (height / 2) / t / depth


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sharp")
    ap.add_argument("spread")
    ap.add_argument("--rows", default="", help="the band of rows (from:to), else the middle tenth")
    ap.add_argument("--max-d", type=float, default=40.0, help="the widest d tried (pixels)")
    args = ap.parse_args()
    pictures = {
        "kin": (linear(os.path.join(args.sharp, "kin.png")), linear(os.path.join(args.spread, "kin.png"))),
        "Cycles": (
            linear(os.path.join(args.sharp, "cycles", "cycles.png")),
            linear(os.path.join(args.spread, "cycles", "cycles.png")),
        ),
    }
    h, w, _ = pictures["kin"][0].shape
    if args.rows:
        y0, y1 = (int(v) for v in args.rows.split(":"))
    else:
        y0, y1 = int(h * 0.45), int(h * 0.55)
    doc = json.load(open(os.path.join(args.spread, "scene.json")))
    radius = next((o["subsurface_radius"] for o in doc["objects"] if o.get("subsurface", 0) > 0), None)
    per_metre = pixels_a_metre(doc, (y0 + y1) / 2, h) if "camera" in doc else None
    print(f"rows {y0}:{y1}" + (f", {per_metre:.0f} pixels a metre on the slab" if per_metre else ""))
    if radius and per_metre:
        print("asked for d  " + "  ".join(f"{c} {r * per_metre:6.2f} px ({r * 1000:.2f} mm)"
                                          for c, r in zip("RGB", radius)))
    for name, (sharp, spread) in pictures.items():
        a = sharp[y0:y1].mean(axis=0)
        b = spread[y0:y1].mean(axis=0)
        # The edge: where the sharp picture's green crosses halfway between its dark and lit sides.
        g = a[:, 1]
        dark, lit = np.percentile(g, 5), np.percentile(g, 95)
        edge = int(np.argmin(np.abs(g - (dark + lit) / 2)))
        window = int(args.max_d * 4)
        out = []
        for c in range(3):
            d, gain = fit(a[:, c], b[:, c], edge, window, args.max_d)
            metres = f" ({d / per_metre * 1000:.2f} mm)" if per_metre else ""
            out.append(f"{'RGB'[c]} {d:6.2f} px{metres} x{gain:.3f}")
        print(f"{name:7s} d     " + "  ".join(out) + f"   (edge at column {edge})")


if __name__ == "__main__":
    main()
