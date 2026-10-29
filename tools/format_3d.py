#!/usr/bin/env python3
"""Format the experimental 3D rendering/authoring code with clang-format 18."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PATTERNS = (
    "engine/include/kin/renderer/color.hpp",
    "engine/include/kin/render3d/*.hpp",
    "engine/src/render3d/*.hpp",
    "engine/src/render3d/*.cpp",
    "engine/src/renderer/webgl2*.*pp",
    "experimental/webgl2/*.*pp",
    "engine/src/renderer/gpu/gpu_*3d.hpp",
    "engine/src/renderer/gpu/gpu_*3d.cpp",
    "engine/src/renderer/gpu/gpu_mesh_pool.*pp",
    "engine/include/kin/scene/*3d.hpp",
    "engine/src/scene/*3d.cpp",
    "engine/src/scripting/lua_scene3d*.*pp",
    "engine/include/kin/ecs/render3d.hpp",
    "engine/include/kin/ecs/scene3d.hpp",
    "engine/src/ecs/render3d.cpp",
    "engine/src/ecs/scene3d*.cpp",
    "engine/src/ecs/animator3d_asset.cpp",
    "engine/src/ecs/particles3d_asset.cpp",
    "engine/src/ecs/joint_attachment3d.cpp",
    "tests/render3d*.cpp",
    "tests/*assets3d_tests.cpp",
    "tests/particle_shadow_cast3d_tests.cpp",
    "tests/mesh_cluster_tests.cpp",
    "tests/skeleton_tests.cpp",
    "tests/ik_tests.cpp",
    "tests/scene_*3d_tests.cpp",
    "tests/script_scene3d*_tests.cpp",
    "tests/model_profile3d_tests.cpp",
    "engine/include/kin/pathfinding/navmesh3d.hpp",
    "engine/src/pathfinding/navmesh3d.cpp",
    "tests/navmesh3d_tests.cpp",
    "tests/diagnostics3d_tests.cpp",
    "tests/errors3d_tests.cpp",
    "tests/camera_rig3d_tests.cpp",
    "tests/interaction3d_tests.cpp",
    "tests/joint_attachment3d_tests.cpp",
    "examples/arena_3d.*pp",
    "examples/breach*.*pp",
    "examples/blockfield/*.*pp",
    "examples/industrial_yard/*.*pp",
    "examples/summit/*.*pp",
    "examples/sponza_walk/*.*pp",
    "examples/camp_walk/*.*pp",
    "examples/projectile_range/*.*pp",
    "examples/lua_range/*.*pp",
    "tools/asset_preview/*.*pp",
    "tools/outline3d/*.*pp",
    "tools/surface3d/*.*pp",
    "tools/slowlight3d/*.*pp",
    "tools/vision3d/*.*pp",
    "tools/reference/kin_reference.cpp",
)


def files():
    return sorted({p for pattern in PATTERNS for p in ROOT.glob(pattern) if p.is_file()})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report differences without writing")
    parser.add_argument("--clang-format", default=shutil.which("clang-format-18") or shutil.which("clang-format"))
    options = parser.parse_args()
    if not options.clang_format:
        parser.error("clang-format 18 is required; pass its path with --clang-format")
    version = subprocess.check_output([options.clang_format, "--version"], text=True)
    if not re.search(r"version 18\.", version):
        parser.error("use clang-format 18 for reproducible formatting")
    sources = files()
    mode = ["--dry-run", "--Werror"] if options.check else ["-i"]
    for first in range(0, len(sources), 40):
        result = subprocess.run([options.clang_format, *mode, *map(str, sources[first:first + 40])], cwd=ROOT)
        if result.returncode:
            return result.returncode
    print(f"{'Checked' if options.check else 'Formatted'} {len(sources)} 3D source files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
