#!/usr/bin/env python3
"""Exercise instrument/train/use/package and stale-profile rejection offline."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--cxx", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    scratch = args.build.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    source = scratch / "source"
    shutil.copytree(root / "tests/fixtures/release_policy", source, dirs_exist_ok=True)
    build = scratch / "build"
    profiles = scratch / "profiles"

    def run(command, error=None):
        result = subprocess.run(list(map(str, command)), text=True, capture_output=True)
        if error:
            assert result.returncode != 0 and error in result.stdout + result.stderr, result.stdout + result.stderr
        elif result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result

    def configure(mode, extra=()):
        return run(["cmake", "-S", source, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            "-DKIN_ENABLE_LTO=OFF", f"-DKIN_PGO_MODE={mode}", f"-DKIN_PGO_DIR={profiles}",
            f"-DKIN_TEST_MODULE_DIR={root / 'cmake'}", f"-DCMAKE_C_COMPILER={args.cc}",
            f"-DCMAKE_CXX_COMPILER={args.cxx}", *extra])

    configure("generate")
    run(["cmake", "--build", build, "--target", "release_fixture"])
    run([build / "release_fixture"])
    run(["cmake", "--build", build, "--target", "release_fixture_package"], "training tools")
    tool = [sys.executable, root / "tools/pgo_profiles.py"]
    run([*tool, "seal", "--build", build])
    configure("use")
    run(["cmake", "--build", build, "--target", "release_fixture_package"])
    run([build / "release_fixture"])
    manifest = json.loads((profiles / "manifest.json").read_text())
    manifest_path = profiles / "manifest.json"
    original_manifest = manifest_path.read_bytes()
    try:
        manifest_path.write_bytes(original_manifest + b"\n")
        run(["cmake", "--build", build, "--target", "release_fixture"], "manifest changed")
    finally:
        manifest_path.write_bytes(original_manifest)
    profile = profiles / next(iter(manifest["profiles"]))
    original = profile.read_bytes()
    try:
        profile.write_bytes(original + b"tamper")
        run(["cmake", "--build", build, "--target", "release_fixture"], "training data changed")
    finally:
        profile.write_bytes(original)
    main_cpp = source / "main.cpp"
    original = main_cpp.read_bytes()
    try:
        main_cpp.write_bytes(original + b"\n// changed after training\n")
        run(["cmake", "--build", build, "--target", "release_fixture"], "source/header changed")
    finally:
        main_cpp.write_bytes(original)
    configure("use", ["-DCMAKE_CXX_FLAGS=-DKIN_CHANGED_PGO_SETTINGS=1"])
    run(["cmake", "--build", build, "--target", "release_fixture"], "compilation commands changed")
    configure("use", ["-DCMAKE_CXX_FLAGS="])
    configure("use", ["-DCMAKE_EXE_LINKER_FLAGS=-Wl,--as-needed"])
    run(["cmake", "--build", build, "--target", "release_fixture"], "compilation commands changed")
    configure("use", ["-DCMAKE_EXE_LINKER_FLAGS="])
    run(["cmake", "--build", build, "--target", "release_fixture_package"])
    print("Compiler PGO round trip and stale input/profile/command rejection passed")


if __name__ == "__main__":
    main()
