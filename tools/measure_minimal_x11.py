#!/usr/bin/env python3
"""Audit the five trusted, locally built minimal-X11 packages and their host costs.

Run after building minimal_x11_packages. ldd executes the system loader, so use
this only with binaries produced by your own build, never an untrusted export.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import zipfile

STAGES = ("x11_empty", "x11_framebuffer", "x11_hex", "x11_hud", "hex_demo")


def host_dependencies(binary):
    output = subprocess.check_output(["ldd", str(binary)], text=True)
    if "not found" in output:
        raise RuntimeError(output)
    libraries = {}
    for line in output.splitlines():
        match = re.search(r"(?:=>\s*)?(/\S+)\s+\(", line)
        if not match:
            continue  # linux-vdso has no disk file
        path = Path(match[1]).resolve(strict=True)
        if "SDL" in path.name:
            raise RuntimeError(f"SDL unexpectedly linked: {path}")
        libraries[str(path)] = {
            "name": path.name, "path": str(path), "bytes": path.stat().st_size,
            "category": "display" if re.match(r"lib(X11|xcb|Xau|Xdmcp)\.", path.name) else "runtime",
        }
    return sorted(libraries.values(), key=lambda item: item["name"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve(strict=True)
    report = {"schema": "kin.minimal-x11-ladder/1", "platform": platform.platform(), "stages": [],
              "note": "Thin exports. Host libraries are excluded from the ZIP; disk bytes are measured on this machine, not extra download sizes. X server/compositor costs are not included. Headless execution does not exercise X11 presentation."}
    for target in STAGES:
        matches = list((build / "packages").glob(f"{target}-*.size.json"))
        if len(matches) != 1:
            raise RuntimeError(f"Expected one {target} package size report, found {len(matches)}")
        metadata = json.loads(matches[0].read_text())
        name = metadata["name"]
        archive = build / "packages" / f"{name}.zip"
        dependencies = host_dependencies(build / "packages" / name / target)
        with tempfile.TemporaryDirectory(prefix="kin x11 relocated ") as temp:
            destination = Path(temp) / "export with spaces"
            with zipfile.ZipFile(archive) as package:
                package.extractall(destination)
            root = destination / name
            verified = 0
            for line in (root / "SHA256SUMS").read_text().splitlines():
                digest, relative = line.split("  ", 1)
                if hashlib.sha256((root / relative).read_bytes()).hexdigest() != digest:
                    raise RuntimeError(f"Checksum mismatch: {relative}")
                verified += 1
            binary = root / target
            binary.chmod(binary.stat().st_mode | 0o100)
            command = [str(binary), "--headless", "--render", "--frames=10"]
            if target == "hex_demo":
                command.append("--report=-")
            environment = dict(os.environ)
            environment.pop("DISPLAY", None)
            result = subprocess.run(command, check=True, capture_output=True, text=True,
                                    env=environment, cwd=temp, timeout=30)
            state = json.loads(result.stdout) if target == "hex_demo" else None
            if state and (state["path_cells"], state["path_cost"], state["reachable_cells"]) != (17, 170, 28):
                raise RuntimeError(f"Unexpected game state: {state}")
        report["stages"].append({
            "target": target, "executable_bytes": metadata["packaged_executable_bytes"],
            "installed_bytes": metadata["uncompressed_bytes"], "zip_bytes": metadata["archive_bytes"],
            "host_libraries": dependencies,
            "host_display_library_bytes": sum(item["bytes"] for item in dependencies if item["category"] == "display"),
            "host_runtime_library_bytes": sum(item["bytes"] for item in dependencies if item["category"] == "runtime"),
            "relocated_run": "passed", "checksums_verified": verified, "game_state": state,
        })
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for stage in report["stages"]:
        print(f'{stage["target"]}: exe {stage["executable_bytes"]:,} B; ZIP {stage["zip_bytes"]:,} B; relocated/checksums OK')


if __name__ == "__main__":
    main()
