"""The reference scenes (docs/3d_plan.md, G0), end to end.

    python3 tools/reference/run.py fetch
    python3 tools/reference/run.py render <label> [--binary build/bin/kin_reference] [--scenes a,b] [kin_reference options...]
    python3 tools/reference/run.py truth <label> [--blender PATH] [--scenes a,b] [--samples N]
    python3 tools/reference/run.py compare <label a> <label b>     # 'truth' is a label too
    python3 tools/reference/run.py noise <label>                   # the same build again: pictures equal? times' spread

Renders go to $KIN_REFERENCE_OUT (else reference_out/) /<label>; the truth
to .../truth, made with the cameras of the label given; a comparison to
.../compare/<a>_vs_<b>/ (index.html, summary.md). The scenes are
tests/reference/scenes/*.json; the cache is fetch.py's.
"""
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SCENES = os.path.join(ROOT, "tests", "reference", "scenes")


def out_root():
    return os.environ.get("KIN_REFERENCE_OUT") or os.path.join(ROOT, "reference_out")


def cache():
    return os.environ.get("KIN_REFERENCE_CACHE") or os.path.join(os.path.expanduser("~"), ".cache", "kin", "reference")


def option(argv, name, default=None):
    if name in argv:
        i = argv.index(name)
        value = argv[i + 1]
        del argv[i:i + 2]
        return value
    return default


def scene_files(argv):
    only = option(argv, "--scenes")
    files = sorted(glob.glob(os.path.join(SCENES, "*.json")))
    if only:
        wanted = set(only.split(","))
        files = [f for f in files if os.path.splitext(os.path.basename(f))[0] in wanted]
    return files


def main():
    argv = sys.argv[1:]
    if not argv:
        sys.exit(__doc__)
    command = argv.pop(0)
    if command == "fetch":
        subprocess.run([sys.executable, os.path.join(HERE, "fetch.py")] + argv, check=True)
    elif command == "render":
        label = argv.pop(0)
        binary = option(argv, "--binary", os.path.join(ROOT, "build", "bin", "kin_reference"))
        files = scene_files(argv)
        out = os.path.join(out_root(), label)
        subprocess.run([binary, *files, "--out", out, "--cache", cache(), *argv], check=False)
    elif command == "truth":
        label = argv.pop(0)
        blender = option(argv, "--blender", shutil.which("blender") or os.path.expanduser("~/Downloads/blender-5.2.2-linux-x64/blender"))
        samples = option(argv, "--samples")
        for f in scene_files(argv):
            cmd = [blender, "-b", "--factory-startup", "--python", os.path.join(HERE, "blender_truth.py"), "--", f,
                   "--kin", os.path.join(out_root(), label, "results.json"), "--cache", cache(), "--out", os.path.join(out_root(), "truth")]
            if samples:
                cmd += ["--samples", samples]
            subprocess.run(cmd, check=False)
    elif command == "compare":
        a, b = argv[0], argv[1]
        subprocess.run([sys.executable, os.path.join(HERE, "compare.py"), os.path.join(out_root(), a), os.path.join(out_root(), b),
                        "--out", os.path.join(out_root(), "compare", f"{a}_vs_{b}"), "--title", f"{a} vs {b}"], check=True)
    elif command == "noise":
        label = argv.pop(0)
        again = label + "_again"
        binary = option(argv, "--binary", os.path.join(ROOT, "build", "bin", "kin_reference"))
        subprocess.run([binary, *scene_files(argv), "--out", os.path.join(out_root(), again), "--cache", cache(), *argv], check=False)
        subprocess.run([sys.executable, os.path.join(HERE, "compare.py"), os.path.join(out_root(), label), os.path.join(out_root(), again),
                        "--out", os.path.join(out_root(), "compare", f"{label}_noise"), "--title", f"{label}: the same build twice"],
                       check=True)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
