"""kin's picture of a scene against Cycles' (the truth), from a snapshot (write_scene_snapshot).

    python3 tools/cycles/compare.py <snapshot dir> [--samples 256] [--denoise] [--blender path] [--reuse]

The snapshot comes from a kin program that writes one (planar_atrium --cycles-snapshot DIR, with --at and --survivor
to place the camera and the character): DIR/scene.json, its meshes and baked pages, and kin.png, kin's own picture.
This renders the scene with Blender's Cycles (tools/cycles/render_snapshot.py; --reuse keeps a render already there)
and writes DIR/cycles/: cycles.png (through the view's exposure and tonemap), compare.png (kin | Cycles | the
difference x4, portals and other uncompared parts grey) and report.json; and prints the measures (mean difference a
channel in levels, PSNR, SSIM, p99, the share of pixels more than 8 levels apart), over the compared pixels.

Blender: --blender, else $KIN_BLENDER, else the bench's (gamedev/bench/tools/blender-*/blender). Needs numpy and
Pillow. What the truth leaves out (scene.json "notes", bloom) is listed with the measures.
"""
import argparse
import glob
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "reference"))
from compare import ssim  # noqa: E402


def find_blender(explicit):
    if explicit:
        return explicit
    if os.environ.get("KIN_BLENDER"):
        return os.environ["KIN_BLENDER"]
    root = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
    for base in (root, os.path.join(root, "..")):
        found = sorted(glob.glob(os.path.join(base, "bench", "tools", "blender-*", "blender")))
        if found:
            return found[-1]
    sys.exit("compare.py: no Blender (pass --blender or set KIN_BLENDER)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("snapshot")
    ap.add_argument("--samples", type=int, default=256)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--denoise", action="store_true")
    ap.add_argument("--blender")
    ap.add_argument("--reuse", action="store_true", help="keep an existing cycles.exr")
    a = ap.parse_args()
    out = os.path.join(a.snapshot, "cycles")
    if not (a.reuse and os.path.exists(os.path.join(out, "cycles.png"))):
        cmd = [find_blender(a.blender), "-b", "--factory-startup", "--python",
               os.path.join(HERE, "render_snapshot.py"), "--", a.snapshot, "--out", out, "--samples", str(a.samples),
               "--seed", str(a.seed)] + (["--denoise"] if a.denoise else [])
        run = subprocess.run(cmd, capture_output=True, text=True)
        if run.returncode != 0 or not os.path.exists(os.path.join(out, "cycles.png")):
            sys.stdout.write(run.stdout[-4000:])
            sys.stderr.write(run.stderr[-4000:])
            sys.exit("compare.py: Blender failed")
        for line in run.stdout.splitlines():
            if line.startswith("render_snapshot:"):
                print(line)

    doc = json.load(open(os.path.join(a.snapshot, "scene.json")))
    kin = np.asarray(Image.open(os.path.join(a.snapshot, "kin.png")).convert("RGB"), dtype=np.float64)
    cyc = np.asarray(Image.open(os.path.join(out, "cycles.png")).convert("RGB"), dtype=np.float64)
    if kin.shape != cyc.shape:
        sys.exit(f"compare.py: kin's picture is {kin.shape[1]}x{kin.shape[0]}, Cycles' {cyc.shape[1]}x{cyc.shape[0]}")
    holes = np.load(os.path.join(out, "holes.npy")) > 0
    keep = ~holes
    d = np.abs(kin - cyc)
    dk = d[keep]
    mse = float((dk * dk).mean()) if dk.size else 0.0
    # (SSIM over the whole picture with the holes made equal: they then agree, and the edges round them count)
    kin_eq = kin.copy()
    kin_eq[holes] = cyc[holes]
    report = {
        "compared_share": round(float(keep.mean()), 4),
        "mean": round(float(dk.mean()), 3) if dk.size else 0.0,
        "psnr": round(float(10 * np.log10(255 * 255 / mse)) if mse > 0 else 99.0, 2),
        "ssim": round(ssim(kin_eq, cyc), 4),
        "p99": round(float(np.percentile(dk.max(axis=1), 99)), 1) if dk.size else 0.0,
        "over8": round(float((dk.max(axis=1) > 8).mean()), 4) if dk.size else 0.0,
        "mean_rgb_kin": [round(float(v), 1) for v in kin[keep].mean(axis=0)],
        "mean_rgb_cycles": [round(float(v), 1) for v in cyc[keep].mean(axis=0)],
        "notes": doc.get("notes", []) + (["kin's bloom is not in the truth"] if doc["view"].get("bloom", 0) > 0 else []),
    }
    try:
        report["cycles"] = json.load(open(os.path.join(out, "cycles.json")))
    except OSError:
        pass
    diff = np.clip(d * 4, 0, 255)
    grey = np.array([96, 96, 96], dtype=np.float64)
    shown = [kin.copy(), cyc.copy(), diff]
    for picture in shown:
        picture[holes] = grey
    sheet = np.concatenate(shown, axis=1).astype(np.uint8)
    Image.fromarray(sheet).save(os.path.join(out, "compare.png"))
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    print(f"{a.snapshot}: mean {report['mean']} levels, PSNR {report['psnr']} dB, SSIM {report['ssim']}, "
          f"p99 {report['p99']}, >8 levels {report['over8'] * 100:.1f}% of {report['compared_share'] * 100:.0f}% "
          f"compared; mean RGB kin {report['mean_rgb_kin']} Cycles {report['mean_rgb_cycles']}")
    for note in report["notes"]:
        print(f"  not in the truth: {note}")
    print(f"  {os.path.join(out, 'compare.png')}")


if __name__ == "__main__":
    main()
