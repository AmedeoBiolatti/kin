#!/usr/bin/env python3
"""Check renderer/SDL API switches in one build tree; reuse fetched dependencies."""
import argparse
import hashlib
from pathlib import Path
import platform
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
               "-DKIN_BUILD_GAMES=OFF", "-DBUILD_TESTING=OFF", "-DKIN_ENABLE_LTO=OFF",
               "-DKIN_ENABLE_SDL_DYNAMIC_API=ON", "-DKIN_ENABLE_SDL_GAMEPAD=ON",
               "-DKIN_ENABLE_SDL_DESKTOP_SERVICES=ON", "-DKIN_LINUX_VIDEO=auto"]
    for compiler in ("C", "CXX"):
        command.append(f"-DCMAKE_{compiler}_COMPILER=" + cache[f"CMAKE_{compiler}_COMPILER"])
    for dependency in ("sdl3", "flecs", "earcut", "earcut_js", "picojson"):
        key = "FETCHCONTENT_SOURCE_DIR_" + dependency.upper()
        source = Path(cache[key]) if cache.get(key) else reuse / "_deps" / (dependency + "-src")
        if not source.is_dir():
            parser.error(f"missing fetched dependency: {source}")
        command.append(f"-D{key}={source}")
    output.mkdir(parents=True, exist_ok=True)
    sdl_source = Path(next(arg.split("=", 1)[1] for arg in command
                           if arg.startswith("-DFETCHCONTENT_SOURCE_DIR_SDL3=")))
    original_header = sdl_source / "src/dynapi/SDL_dynapi.h"
    original_digest = hashlib.sha256(original_header.read_bytes()).hexdigest()
    with (output / "configure.log").open("w") as log:
        # This is a dedicated regression tree. Establish default SDL preferences
        # once; stale/user SDL overrides from a previous run otherwise make the
        # auto profile's expected capabilities ambiguous. Later transitions must
        # preserve those defaults without any reset.
        subprocess.run(command + ["-DKIN_RENDERER=software", "-USDL_*"], cwd=root,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        # A shipping preset must clear the isolated runtime switch, rather
        # than silently retaining the SDL-free targets in a reused build tree.
        if cache.get("CMAKE_SYSTEM_NAME", platform.system()) == "Linux":
            isolated = command.copy()
            isolated[2] = "hex-minimal-x11"
            subprocess.run(isolated, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
            ninja = (output / "build.ninja").read_text()
            if "SDL_render_sw.c.o" in ninja or "engine/CMakeFiles/kin_engine.dir" in ninja:
                raise RuntimeError("isolated runtime unexpectedly retained the SDL engine")
        shipping = command.copy()
        shipping[2] = "hex-shipping"
        subprocess.run(shipping, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
        selected = cache_values(output / "CMakeCache.txt")
        if selected.get("KIN_EXPERIMENTAL_MINIMAL_X11") != "OFF" or selected.get("KIN_RENDERER") != "software":
            raise RuntimeError("shipping preset retained an experimental runtime or renderer")
        ninja = (output / "build.ninja").read_text()
        if "SDL_render_sw.c.o" not in ninja or "sdl_renderer2d_backend.cpp.o" not in ninja:
            raise RuntimeError("shipping preset removed SDL rendering implementations")
        print("shipping preset: SDL software retained; isolated runtime disabled OK", flush=True)
        for renderer in ("software", "compact", "gpu", "auto", "compact", "software"):
            subprocess.run(command + [f"-DKIN_RENDERER={renderer}"], cwd=root,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            headers = list((output / "_deps/sdl3-build").glob("include*/build_config/SDL_build_config.h"))
            if len(headers) != 1:
                raise RuntimeError(f"expected one SDL configuration header, got {headers}")
            header = headers[0].read_text()
            for macro, disabled in (("GPU", renderer in ("software", "compact")), ("RENDER", renderer in ("gpu", "compact"))):
                if (f"#define SDL_{macro}_DISABLED 1" in header) != disabled:
                    raise RuntimeError(f"stale SDL {macro} setting after switching to {renderer}")
            print(f"renderer switch: {renderer} OK", flush=True)
        # A private SDL source must be used only while the dynamic API is off;
        # switching back must restore the normal source in the same build tree.
        for enabled in ("OFF", "ON", "OFF", "ON"):
            subprocess.run(command + ["-DKIN_RENDERER=software",
                                     f"-DKIN_ENABLE_SDL_DYNAMIC_API={enabled}"], cwd=root,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            ninja = (output / "build.ninja").read_text()
            private = output / "_deps/sdl3-direct-src"
            selected = private if enabled == "OFF" else sdl_source
            if f"{selected.as_posix()}/src/SDL.c" not in ninja:
                raise RuntimeError(f"wrong SDL source after dynamic API switch to {enabled}")
            if enabled == "OFF":
                header = (private / "src/dynapi/SDL_dynapi.h").read_text()
                if "#define SDL_DYNAMIC_API 1" in header:
                    raise RuntimeError("private SDL copy still enables the dynamic API")
            if hashlib.sha256(original_header.read_bytes()).hexdigest() != original_digest:
                raise RuntimeError("dynamic API switch modified the shared SDL header")
            print(f"SDL dynamic API switch: {enabled} OK (shared source preserved)", flush=True)
        baseline_tray_dummy = "#define SDL_TRAY_DUMMY 1" in headers[0].read_text()
        for enabled in ("OFF", "ON", "OFF", "ON"):
            subprocess.run(command + ["-DKIN_RENDERER=software",
                                     f"-DKIN_ENABLE_SDL_GAMEPAD={enabled}",
                                     f"-DKIN_ENABLE_SDL_DESKTOP_SERVICES={enabled}"], cwd=root,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            header = headers[0].read_text()
            for subsystem in ("JOYSTICK", "HIDAPI"):
                if (f"#define SDL_{subsystem}_DISABLED 1" in header) != (enabled == "OFF"):
                    raise RuntimeError(f"stale SDL {subsystem} setting after switch to {enabled}")
            if ("#define SDL_TRAY_DUMMY 1" in header) != (enabled == "OFF" or baseline_tray_dummy):
                raise RuntimeError(f"stale SDL tray setting after switch to {enabled}")
            print(f"SDL optional subsystems: {enabled} OK", flush=True)
        if cache.get("CMAKE_SYSTEM_NAME", platform.system()) == "Linux":
            baseline = headers[0].read_text()
            available = {driver for driver in ("x11", "wayland")
                         if f"#define SDL_VIDEO_DRIVER_{driver.upper()} 1" in baseline}
            for profile in ("x11", "wayland", "x11-wayland", "auto"):
                required = available if profile == "auto" else set(profile.split("-"))
                result = subprocess.run(command + [f"-DKIN_LINUX_VIDEO={profile}"], cwd=root,
                                        stdout=log, stderr=subprocess.STDOUT)
                if not required <= available:
                    if result.returncode == 0:
                        raise RuntimeError(f"unavailable backend accepted: {profile}")
                    print(f"unavailable video profile: {profile} rejected", flush=True)
                    continue
                result.check_returncode()
                header = headers[0].read_text()
                for driver in ("x11", "wayland"):
                    if (f"#define SDL_VIDEO_DRIVER_{driver.upper()} 1" in header) != (driver in required):
                        raise RuntimeError(f"stale SDL {driver} setting after switch to {profile}")
                print(f"Linux video switch: {profile} OK", flush=True)
        result = subprocess.run(command + ["-DKIN_LINUX_VIDEO=typo"], cwd=root,
                                stdout=log, stderr=subprocess.STDOUT)
        if result.returncode == 0:
            raise RuntimeError("invalid video profile accepted")
        print("invalid video profile rejected", flush=True)
        result = subprocess.run(command + ["-DKIN_RENDERER=typo"], cwd=root,
                                stdout=log, stderr=subprocess.STDOUT)
        if result.returncode == 0:
            raise RuntimeError("invalid renderer accepted")
        print("invalid renderer rejected", flush=True)


if __name__ == "__main__":
    main()
