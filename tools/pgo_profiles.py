#!/usr/bin/env python3
"""Seal compiler PGO training data and reject incompatible use builds."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized(words):
    return [word for word in words if not word.startswith((
        "-fprofile-generate=", "-fprofile-use=", "-fprofile-instr-generate=",
        "-fprofile-instr-use=", "-fprofile-update=", "-fprofile-partial-training",
        "-Werror=coverage-mismatch", "-Werror=profile-instr-out-of-date"))]


def ninja_tool(build):
    cache = (build / "CMakeCache.txt").read_text().splitlines()
    return next(line.split("=", 1)[1] for line in cache if line.startswith("CMAKE_MAKE_PROGRAM:"))


def commands(build):
    result = []
    for entry in json.loads((build / "compile_commands.json").read_text()):
        words = entry.get("arguments") or shlex.split(entry["command"])
        result.append([entry["file"], normalized(words)])
    recipes = subprocess.check_output([ninja_tool(build), "-C", str(build), "-t", "commands"], text=True)
    for line in recipes.splitlines():
        words = shlex.split(line)
        if "-o" in words and "-c" not in words:
            result.append(["link-or-generator", normalized(words)])
    return hashlib.sha256(json.dumps(sorted(result)).encode()).hexdigest()


def inputs(build, settings):
    # Ninja records the actual compiler-discovered headers, including generated
    # and third-party headers, rather than guessing dependencies from filenames.
    ninja = ninja_tool(build)
    deps = subprocess.check_output([ninja, "-C", str(build), "-t", "deps"], text=True)
    paths = {Path(entry["file"]).resolve()
             for entry in json.loads((build / "compile_commands.json").read_text())}
    compiling = False
    for line in deps.splitlines():
        if line and not line.startswith(" "):
            compiling = line.split(": #deps", 1)[0].endswith((".o", ".obj"))
        if compiling and line.startswith("    "):
            path = Path(line.strip())
            paths.add((path if path.is_absolute() else build / path).resolve())
    # Shader generators run before compilation. Catch their source changes
    # before an old generated C++ file is replaced during the use build.
    source = Path(settings["source_dir"])
    if shutil.which("git"):
        files = subprocess.run(["git", "-C", str(source), "ls-files", "--cached", "--others",
                                "--exclude-standard", "-z"], capture_output=True)
        if not files.returncode:
            for name in files.stdout.decode().split("\0"):
                path = source / name
                if path.suffix in (".glsl", ".vert", ".frag", ".comp", ".spv"):
                    paths.add(path.resolve())
    return {str(path): digest(path) for path in sorted(paths) if path.is_file()}


def profile_files(settings):
    root = Path(settings["profile_dir"])
    pattern = "*.gcda" if settings["compiler"] == "GNU" else "*.profraw"
    paths = sorted((root / "raw").rglob(pattern))
    if settings["compiler"] == "Clang":
        paths.append(root / "merged.profdata")
    if not paths or any(not path.is_file() or not path.stat().st_size for path in paths):
        raise ValueError("Missing or empty compiler profiles; complete training and merge first")
    return {str(path.relative_to(root)): digest(path) for path in paths}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("seal", "verify"))
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--llvm-profdata", help="Matching Clang llvm-profdata executable")
    parser.add_argument("--manifest-sha256", help="Require the exact manifest selected at configuration")
    parser.add_argument("--training-report", type=Path, action="append", default=[],
                        help="Bind a workload report into the sealed manifest (repeatable)")
    args = parser.parse_args()
    build = args.build.resolve()
    settings = json.loads((build / "kin-pgo-settings.json").read_text())
    mode = settings.pop("mode")
    root = Path(settings["profile_dir"])
    if args.action == "seal":
        if mode != "generate":
            raise ValueError("Seal requires the instrumented generate configuration")
        if settings["compiler"] == "Clang":
            tool = args.llvm_profdata or shutil.which("llvm-profdata-" + settings["compiler_version"].split(".")[0]) or shutil.which("llvm-profdata")
            raw = sorted((root / "raw").rglob("*.profraw"))
            if not tool or not raw:
                raise ValueError("Clang training requires raw profiles and matching llvm-profdata")
            subprocess.run([tool, "merge", "-o", str(root / "merged.profdata"), *map(str, raw)], check=True)
        manifest = {"schema": "kin.compiler-pgo/1", "settings": settings,
                    "commands_sha256": commands(build), "inputs": inputs(build, settings),
                    "profiles": profile_files(settings),
                    "training": [{"sha256": digest(path), "report": json.loads(path.read_text())}
                                 for path in args.training_report]}
        (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"Sealed {len(manifest['profiles'])} profiles and {len(manifest['inputs'])} source/header inputs")
    else:
        if mode != "use":
            raise ValueError("Verify requires the use configuration")
        if args.manifest_sha256 and digest(root / "manifest.json") != args.manifest_sha256:
            raise ValueError("PGO manifest changed after configuration; reconfigure deliberately")
        manifest = json.loads((root / "manifest.json").read_text())
        if manifest["schema"] != "kin.compiler-pgo/1" or settings != manifest["settings"]:
            raise ValueError("PGO compiler, version, configuration or build/profile directory changed; retrain")
        if commands(build) != manifest["commands_sha256"]:
            raise ValueError("PGO compilation commands changed; retrain")
        for filename, expected in manifest["inputs"].items():
            path = Path(filename)
            if not path.is_file() or digest(path) != expected:
                raise ValueError(f"PGO source/header changed; retrain: {filename}")
        if profile_files(settings) != manifest["profiles"]:
            raise ValueError("PGO training data changed after sealing; retrain/reseal deliberately")
        print(f"PGO compatibility verified ({len(manifest['profiles'])} profiles)")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"PGO: {error}")
