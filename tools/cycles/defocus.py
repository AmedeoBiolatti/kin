"""kin's depth of field against Cycles': how wide the blur is, band by band, in each renderer's own pictures.

    python3 tools/cycles/defocus.py <sharp snapshot dir> <defocused snapshot dir> [--band 30] [--x0 0 --x1 W]

Both folders are snapshots of the same view (kin_reference --snapshot DIR, once without and once with --dof ...),
each already rendered by tools/cycles/compare.py (so cycles/cycles.png is there beside kin.png). For each band of rows
it finds the disc radius (pixels, steps of half a pixel) whose blur best turns the sharp picture into the defocused
one, for kin and for Cycles separately: their lighting may differ, but the blur each renderer's lens adds should not.
Bands with too little detail to fit (a flat floor, the sky) are left out (--detail).
Needs numpy and Pillow.
"""
import argparse
import os

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float32) / 255.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sharp")
    ap.add_argument("defocused")
    ap.add_argument("--band", type=int, default=30)
    ap.add_argument("--x0", type=int, default=0)
    ap.add_argument("--x1", type=int, default=0)
    ap.add_argument("--max-radius", type=float, default=24.0)
    ap.add_argument("--detail", type=float, default=0.004, help="least mean step between neighbours to fit a band")
    args = ap.parse_args()
    pictures = {
        "kin": (load(os.path.join(args.sharp, "kin.png")), load(os.path.join(args.defocused, "kin.png"))),
        "Cycles": (
            load(os.path.join(args.sharp, "cycles", "cycles.png")),
            load(os.path.join(args.defocused, "cycles", "cycles.png")),
        ),
    }
    h, w, _ = pictures["kin"][0].shape
    x0, x1 = args.x0, args.x1 or w
    yy, xx = np.mgrid[0:h, 0:w]
    corner = np.sqrt(np.minimum(yy, h - yy) ** 2 + np.minimum(xx, w - xx) ** 2)

    def disc(picture, radius):
        if radius < 0.5:
            return picture
        kernel = np.clip(radius + 0.5 - corner, 0.0, 1.0)
        spectrum = np.fft.rfft2(kernel / kernel.sum())
        return np.stack(
            [np.fft.irfft2(np.fft.rfft2(picture[..., c]) * spectrum, s=(h, w)) for c in range(3)], -1
        )

    radii = np.arange(0.0, args.max_radius + 0.01, 0.5)
    blurred = {name: [disc(sharp, r) for r in radii] for name, (sharp, _) in pictures.items()}
    print("rows        kin radius   Cycles radius   (pixels)")
    for y0 in range(0, h - args.band + 1, args.band):
        found = []
        for name, (sharp, defocused) in pictures.items():
            target = defocused[y0 : y0 + args.band, x0:x1]
            # A band with too little detail (a flat floor, the sky) fits any radius: left out.
            if np.mean(np.abs(np.diff(sharp[y0 : y0 + args.band, x0:x1], axis=1))) < args.detail:
                found = None
                break
            errors = [np.mean((b[y0 : y0 + args.band, x0:x1] - target) ** 2) for b in blurred[name]]
            found.append(radii[int(np.argmin(errors))])
        if found:
            print(f"{y0:4d}-{y0 + args.band:4d}   {found[0]:6.1f}       {found[1]:6.1f}")


if __name__ == "__main__":
    main()
