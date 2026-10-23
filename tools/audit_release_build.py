#!/usr/bin/env python3
"""Verify actual GNU/Clang Ninja commands, not just release option values."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--target", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    build = args.build.resolve()
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    if cache["CMAKE_GENERATOR"] != "Ninja":
        parser.error("This command audit currently supports GNU/Clang with single-config Ninja")
    if cache["CMAKE_BUILD_TYPE"] not in ("Release", "RelWithDebInfo", "MinSizeRel"):
        parser.error("An optimized build configuration is required")
    enabled = lambda key: cache.get(key, "OFF").upper() in ("ON", "TRUE", "YES", "1")
    expected = []
    if enabled("KIN_ENABLE_LTO"):
        expected.append("-flto")
    if enabled("KIN_ENABLE_SIZE_OPTIMIZATIONS"):
        expected.extend(("-ffunction-sections", "-fdata-sections"))
    if enabled("KIN_PACKAGE_DEBUG_SYMBOLS"):
        expected.append("-g")
    pgo = {"mode": cache.get("KIN_PGO_MODE", "off")}
    pgo_flag = None
    if pgo["mode"] != "off":
        settings = json.loads((build / "kin-pgo-settings.json").read_text())
        root = settings["profile_dir"]
        if settings["compiler"] == "GNU":
            pgo_flag = f"-fprofile-{'generate' if pgo['mode'] == 'generate' else 'use'}={root}/raw"
        else:
            pgo_flag = (f"-fprofile-instr-generate={root}/raw/%m-%p.profraw" if pgo["mode"] == "generate"
                        else f"-fprofile-instr-use={root}/merged.profdata")
        expected.append(pgo_flag)
        if pgo["mode"] == "generate":
            expected.append("-fprofile-update=atomic")
    failures = []
    checked = []
    for entry in json.loads((build / "compile_commands.json").read_text()):
        if Path(entry["file"]).suffix not in (".c", ".cc", ".cpp", ".cxx", ".C"):
            continue
        words = entry.get("arguments") or shlex.split(entry["command"])
        missing = [flag for flag in expected if not any(
            word == flag or word.startswith(flag + "=") for word in words)]
        # Explicit later disables invalidate an otherwise present enable flag.
        forbidden = [word for word in words if word in (
            "-fno-lto", "-fno-function-sections", "-fno-data-sections", "-g0")]
        if missing or forbidden:
            failures.append({"source": entry["file"], "missing": missing, "disabled": forbidden})
        checked.append(entry["file"])
    if not checked:
        failures.append({"error": "No C/C++ compilation commands found"})
    commands = subprocess.check_output(
        [cache["CMAKE_MAKE_PROGRAM"], "-C", str(build), "-t", "commands", args.target], text=True)
    links = []
    for command in commands.splitlines():
        words = shlex.split(command)
        if "-o" in words:
            output = words[words.index("-o") + 1]
            if Path(output).name == args.target and "-c" not in words:
                links.append(command)
    if len(links) != 1:
        failures.append({"error": f"Expected one executable link command; found {len(links)}"})
    else:
        words = shlex.split(links[0])
        if enabled("KIN_ENABLE_LTO") and not any(word.startswith("-flto") for word in words):
            failures.append({"error": "Executable link is missing LTO"})
        if enabled("KIN_ENABLE_LTO") and "-fno-lto" in words:
            failures.append({"error": "Executable link explicitly disables LTO"})
        if enabled("KIN_ENABLE_SIZE_OPTIMIZATIONS") and "--gc-sections" not in links[0]:
            failures.append({"error": "Executable link is missing section GC"})
        if enabled("KIN_ENABLE_SIZE_OPTIMIZATIONS") and "--no-gc-sections" in links[0]:
            failures.append({"error": "Executable link explicitly disables section GC"})
        if pgo_flag and pgo_flag not in words:
            failures.append({"error": "Executable link is missing the configured PGO flag"})
    report = {"schema": "kin.release-command-audit/1", "target": args.target,
              "configuration": cache["CMAKE_BUILD_TYPE"], "compile_units": len(checked),
              "required_compile_flags": expected, "link_commands": links, "pgo": pgo,
              "failures": failures, "passed": not failures}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{len(checked)} compilation units; {len(failures)} failures; {args.output}")
    raise SystemExit(bool(failures))


if __name__ == "__main__":
    main()
