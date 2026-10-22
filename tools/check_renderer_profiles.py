#!/usr/bin/env python3
"""Regression check for renderer switches in one CMake build tree; reuse fetched dependencies."""
import argparse
from pathlib import Path
import subprocess


def cache_values(path):
    values = {}
    for line in path.read_text().splitlines():
        if line.startswith(("#", "//")) or "=" not in line or ":" not in line:
            continue
        name, value = line.split("=", 1)
        values[name.split(":", 1)[0]] = value
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reuse-build", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    reuse = args.reuse_build.resolve()
    output = args.output_dir.resolve()
    if output == reuse:
        parser.error("use a separate output directory to preserve the original build")
    cache = cache_values(reuse / "CMakeCache.txt")
    command = ["cmake", "--preset", "hex-small", "-B", str(output),
               "-DKIN_BUILD_GAMES=OFF", "-DBUILD_TESTING=OFF", "-DKIN_ENABLE_LTO=OFF"]
    for compiler in ("C", "CXX"):
        command.append(f"-DCMAKE_{compiler}_COMPILER=" + cache[f"CMAKE_{compiler}_COMPILER"])
    for dependency in ("sdl3", "flecs", "earcut", "earcut_js", "picojson"):
        key = "FETCHCONTENT_SOURCE_DIR_" + dependency.upper()
        source = Path(cache[key]) if cache.get(key) else reuse / "_deps" / (dependency + "-src")
        if not source.is_dir():
            parser.error(f"missing fetched dependency: {source}")
        command.append(f"-D{key}={source}")
    output.mkdir(parents=True, exist_ok=True)
    with (output / "configure.log").open("w") as log:
        for renderer in ("software", "gpu", "auto", "software"):
            subprocess.run(command + [f"-DKIN_RENDERER={renderer}"], cwd=root,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            headers = list((output / "_deps/sdl3-build").glob("include*/build_config/SDL_build_config.h"))
            if len(headers) != 1:
                raise RuntimeError(f"expected one SDL configuration header, got {headers}")
            header = headers[0].read_text()
            for macro, disabled in (("GPU", renderer == "software"), ("RENDER", renderer == "gpu")):
                if (f"#define SDL_{macro}_DISABLED 1" in header) != disabled:
                    raise RuntimeError(f"stale SDL {macro} setting after switching to {renderer}")
            print(f"renderer switch: {renderer} OK", flush=True)
        result = subprocess.run(command + ["-DKIN_RENDERER=typo"], cwd=root,
                                stdout=log, stderr=subprocess.STDOUT)
        if result.returncode == 0:
            raise RuntimeError("invalid renderer accepted")
        print("invalid renderer rejected", flush=True)


if __name__ == "__main__":
    main()
