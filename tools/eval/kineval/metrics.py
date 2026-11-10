"""Bistro-compatible display and temporal metrics, plus Kin's SSIM diagnostic.

Formulas originate in gallery/bistro/tools/eval/score.py (MIT) and
Kin tools/reference/compare.py. Display transforms are explicit job inputs.
"""

import math
from pathlib import Path
from .common import EvalError, Unsupported

VERSION = "kin.eval-metrics/1"


def transform(path, spec):
    import numpy as np
    from PIL import Image

    if Path(path).suffix.lower() == ".exr":
        try:
            import OpenEXR
        except ImportError as e:
            raise Unsupported("EXR scoring needs OpenEXR") from e
        with OpenEXR.File(str(path)) as image:
            c = image.channels()
            rgb = c.get("RGB", c.get("RGBA"))
            x = np.asarray(
                (
                    rgb.pixels[:, :, :3]
                    if rgb is not None
                    else np.stack([c[k].pixels for k in "RGB"], axis=2)
                ),
                dtype=np.float64,
            )
    else:
        x = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64) / 255.0
    if not np.isfinite(x).all():
        raise EvalError(f"non-finite image: {path}")
    kind = spec.get("kind", "srgb")
    if Path(path).suffix.lower() == ".exr" and kind in ("srgb", "bistro_port"):
        raise EvalError("linear EXR requires an explicit display transform")
    if kind == "bistro_cycles":
        if Path(path).suffix.lower() != ".exr":
            raise EvalError("bistro_cycles requires linear EXR")
        x = x * 683.0 * spec["iso"] / 3072007.0
        rrt = (
            np.array(
                (
                    (0.59719, 0.35458, 0.04823),
                    (0.076, 0.90834, 0.01566),
                    (0.02840, 0.13383, 0.83777),
                )
            )
            * 1.8
        )
        odt = np.array(
            (
                (1.60475, -0.53108, -0.07367),
                (-0.10208, 1.10813, -0.00605),
                (-0.00327, -0.07276, 1.07602),
            )
        )
        x = x @ rrt.T
        x = (x * (x + 0.0245786) - 0.000090537) / (
            x * (0.983729 * x + 0.432951) + 0.238081
        )
        x = x @ odt.T
        w = spec.get("white", 6.0) * 1.8
        x /= (w * (w + 0.0245786) - 0.000090537) / (
            w * (0.983729 * w + 0.432951) + 0.238081
        )
        x = np.clip(x, 0, 1)
        x = np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)
    elif kind not in ("srgb", "bistro_port"):
        raise EvalError(f"unsupported output transform: {kind}")
    if kind.startswith("bistro_"):
        mean = x.mean(axis=2, keepdims=True)
        x = mean + (x - mean) * spec.get("saturation", 1.17)
    return np.round(np.clip(x, 0, 1) * 255).astype(np.uint8)


def luminance(rgb):
    import numpy as np

    return np.asarray(rgb, dtype=np.float64) @ np.array([0.2126, 0.7152, 0.0722])


def detail(rgb):
    y = luminance(rgb) / 255.0
    if min(y.shape) < 3:
        return None
    lap = -4 * y[1:-1, 1:-1] + y[:-2, 1:-1] + y[2:, 1:-1] + y[1:-1, :-2] + y[1:-1, 2:]
    return float(lap.var())


def ssim(a, b):
    import numpy as np

    # Retain the existing Kin encoded-luma SSIM, distinct from the Bistro luma metrics.
    a = np.asarray(a, dtype=np.float64) @ np.array([0.299, 0.587, 0.114])
    b = np.asarray(b, dtype=np.float64) @ np.array([0.299, 0.587, 0.114])

    def gaussian(im):
        radius = min(5, min(im.shape) - 1)
        if radius < 1:
            return im.copy()
        x = np.arange(-radius, radius + 1)
        k = np.exp(-x * x / (2 * 1.5**2))
        k /= k.sum()
        pad = np.pad(im, ((radius, radius), (0, 0)), mode="reflect")
        first = sum(w * pad[i : i + im.shape[0]] for i, w in enumerate(k))
        pad = np.pad(first, ((0, 0), (radius, radius)), mode="reflect")
        return sum(w * pad[:, i : i + im.shape[1]] for i, w in enumerate(k))

    ma, mb = gaussian(a), gaussian(b)
    va, vb = gaussian(a * a) - ma * ma, gaussian(b * b) - mb * mb
    cov = gaussian(a * b) - ma * mb
    return float(
        (
            ((2 * ma * mb + 6.5025) * (2 * cov + 58.5225))
            / ((ma * ma + mb * mb + 6.5025) * (va + vb + 58.5225))
        ).mean()
    )


class Perceptual:
    def __init__(self, display, required):
        self.display = display
        self.required = set(required)
        self.cvvdp = None
        if "jod" in self.required:
            try:
                import pycvvdp
                import torch
            except ImportError as e:
                raise Unsupported("JOD needs pycvvdp and torch") from e
            if not display.get("name") or not display.get("file"):
                raise EvalError("JOD requires an explicit display name and model file")
            self.cvvdp = pycvvdp.cvvdp(
                display_name=display["name"],
                config_paths=[str(Path(display["file"]))],
                device=torch.device(
                    display.get(
                        "device", "cuda" if torch.cuda.is_available() else "cpu"
                    )
                ),
                quiet=True,
            )
        if "flip" in self.required:
            try:
                import flip_evaluator
            except ImportError as e:
                raise Unsupported("FLIP needs flip-evaluator") from e

    def flip(self, reference, test):
        import numpy as np
        import flip_evaluator as flip

        params = {}
        if self.display.get("flip_ppd") is not None:
            params["ppd"] = float(self.display["flip_ppd"])
        values = (
            np.asarray(reference, dtype=np.float32) / 255,
            np.asarray(test, dtype=np.float32) / 255,
            "LDR",
        )
        error, mean, _ = (
            flip.evaluate(*values, parameters=params, applyMagma=False)
            if params
            else flip.evaluate(*values, applyMagma=False)
        )
        return float(mean), np.asarray(error)

    def jod(self, reference, frames, fps):
        import numpy as np
        import torch

        with torch.no_grad():
            if len(frames) == 1:
                q, _ = self.cvvdp.predict(frames[0], reference[0], dim_order="HWC")
            else:
                q, _ = self.cvvdp.predict(
                    np.stack(frames),
                    np.stack(reference),
                    dim_order="FHWC",
                    frames_per_second=fps,
                )
        return float(q)


def compare(reference, frames, fps, mode, wanted, perceptual=None):
    import numpy as np

    if not frames or len(reference) != len(frames):
        raise EvalError("reference/test frame sets must match exactly")
    shape = reference[0].shape
    if any(x.shape != shape for x in reference + frames):
        raise EvalError("image dimensions differ; no automatic resize/crop")
    if mode not in ("still", "static", "moving"):
        raise EvalError(f"unknown sequence mode: {mode}")
    if mode == "still" and len(frames) != 1:
        raise EvalError("still sequence needs one frame")
    if mode != "still" and len(frames) < 2:
        raise EvalError("video sequence needs at least two frames")
    if fps <= 0 or not math.isfinite(fps):
        raise EvalError("positive finite FPS required")
    results = []
    flip_maps = []
    for ref, test in zip(reference, frames):
        diff = test.astype(np.float64) - ref.astype(np.float64)
        mse = float((diff * diff).mean())
        dref = detail(ref)
        dtest = detail(test)
        dark = luminance(ref) < np.median(luminance(ref))
        row = {
            "psnr_db": None if mse == 0 else 10 * math.log10(255**2 / mse),
            "identical": mse == 0,
            "ssim": ssim(ref, test),
            "bias_levels": float(diff.mean()),
            "bias_dark_levels": float(diff[dark].mean()) if dark.any() else None,
            "bias_bright_levels": float(diff[~dark].mean()) if (~dark).any() else None,
            "detail_ratio": (
                dtest / dref
                if dref is not None and dref > 1e-20 and dtest is not None
                else None
            ),
            "mean_absolute_levels": float(np.abs(diff).mean()),
            "p99_absolute_levels": float(np.percentile(np.abs(diff), 99)),
            "over8_fraction": float((np.abs(diff).max(axis=2) > 8).mean()),
        }
        if "flip" in wanted:
            row["flip_mean"], error = perceptual.flip(ref, test)
            flip_maps.append(error)
        results.append(row)
    averages = {}
    for key in results[0]:
        if key == "identical":
            continue
        values = [r[key] for r in results]
        averages[key] = (
            float(np.mean(values)) if all(v is not None for v in values) else None
        )
    averages["identical"] = all(r["identical"] for r in results)
    if "flip" in wanted:
        averages["flip_worst_frame"] = max(r["flip_mean"] for r in results)
    if "jod" in wanted:
        averages["jod_image" if mode == "still" else "jod_video"] = perceptual.jod(
            reference, frames, fps
        )
    if len(frames) > 1:
        t = np.stack([luminance(x) for x in frames])
        r = np.stack([luminance(x) for x in reference])
        if mode == "moving":
            averages["temporal_error_levels"] = float(
                np.abs(np.diff(t, axis=0) - np.diff(r, axis=0)).mean()
            )
        elif mode == "static":
            averages["shimmer_levels"] = float(t.std(axis=0).mean())
            averages["stepping_fraction"] = float(
                (np.abs(np.diff(t, axis=0)).max(axis=0) > 8).mean()
            )
    return {
        "schema": "kin.eval-score/1",
        "metric_version": VERSION,
        "mode": mode,
        "fps": fps,
        "frames": len(frames),
        "summary": averages,
        "per_frame": results,
    }, flip_maps
