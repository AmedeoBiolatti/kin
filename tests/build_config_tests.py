"""Exercise developer build settings from a parent game's CMake project."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PARSER = argparse.ArgumentParser()
PARSER.add_argument("--cmake", default="cmake")
PARSER.add_argument("--generator", default="Ninja" if shutil.which("ninja") else None)
ARGS, REMAINING = PARSER.parse_known_args()


class BuildConfigTests(unittest.TestCase):
    def run_command(self, *command):
        env = os.environ.copy()
        for language in ("C", "CXX"):
            env.pop(f"CMAKE_{language}_COMPILER_LAUNCHER", None)
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=env)
        self.assertEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def fixture(self, root):
        source = root / "source"
        source.mkdir()
        (source / "kin").mkdir()
        (source / "include").mkdir()
        (source / "include/kin/runtime").mkdir(parents=True)
        (source / "include/kin/runtime/scene_app.hpp").write_text(
            "#pragma once\nnamespace kin { struct FixtureScene {}; }\n")
        (source / "include/flecs.h").write_text("#pragma once\nstruct FixtureEntity {};\n")
        (source / "kin/engine.cpp").write_text(
            '#include <vector>\nint value() { return std::vector<int>{1, 2}.size(); }\n')
        (source / "kin/plain.c").write_text("int plain(void) { return 0; }\n")
        (source / "kin/CMakeLists.txt").write_text('''
include("${KIN_ROOT}/cmake/kin_game.cmake")
add_library(engine STATIC engine.cpp plain.c)
kin_precompile_target(engine)
''')
        (source / "main.cpp").write_text('''
#include <flecs.h>
#include <kin/runtime/scene_app.hpp>
int value();
int main() { FixtureEntity entity; (void)entity; return value() == 2 ? 0 : 1; }
''')
        (source / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.22)
project(parent_game LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
add_subdirectory(kin)
add_executable(game main.cpp)
target_link_libraries(game PRIVATE engine)
target_include_directories(game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
kin_configure_game_build(game)
add_executable(custom EXCLUDE_FROM_ALL main.cpp)
set_property(TARGET custom PROPERTY CXX_COMPILER_LAUNCHER "custom-launcher")
target_precompile_headers(custom PRIVATE <vector>)
kin_configure_game_build(custom)
get_target_property(engine_launcher engine CXX_COMPILER_LAUNCHER)
get_target_property(game_launcher game CXX_COMPILER_LAUNCHER)
get_target_property(custom_launcher custom CXX_COMPILER_LAUNCHER)
get_target_property(custom_headers custom PRECOMPILE_HEADERS)
file(WRITE "${CMAKE_BINARY_DIR}/settings.txt"
    "${engine_launcher}\\n${game_launcher}\\n${custom_launcher}\\n${custom_headers}\\n")
''')
        return source, root / "build"

    def configure(self, source, build, *settings):
        command = [ARGS.cmake, "-S", str(source), "-B", str(build),
                   f"-DKIN_ROOT={ROOT.as_posix()}", *settings]
        if ARGS.generator:
            command += ["-G", ARGS.generator]
        self.run_command(*command)

    def test_pch_in_parent_game_with_mixed_c_and_cpp(self):
        with tempfile.TemporaryDirectory(prefix="kin pch ") as directory:
            source, build = self.fixture(Path(directory))
            self.configure(source, build, "-DKIN_ENABLE_PCH=ON", "-DKIN_COMPILER_CACHE=off")
            self.run_command(ARGS.cmake, "--build", str(build), "--config", "Release",
                             "--target", "game", "--parallel", "2")
            executable = build / ("game.exe" if os.name == "nt" else "game")
            if not executable.exists():
                executable = build / "Release" / executable.name
            self.run_command(str(executable))
            settings = (build / "settings.txt").read_text().splitlines()
            self.assertEqual(settings[2:], ["custom-launcher", "<vector>"])

    def test_cache_reaches_parent_game_and_can_be_disabled(self):
        with tempfile.TemporaryDirectory(prefix="kin cache ") as directory:
            root = Path(directory)
            source, build = self.fixture(root)
            tools = root / "tools"
            tools.mkdir()
            cache = tools / ("ccache.exe" if os.name == "nt" else "ccache")
            cache.write_text("fixture: only discovered, never executed\n")
            cache.chmod(0o755)
            self.configure(source, build, "-DKIN_COMPILER_CACHE=ccache",
                           f"-DCMAKE_PROGRAM_PATH={tools.as_posix()}")
            settings = (build / "settings.txt").read_text().splitlines()
            self.assertEqual(settings[:2], [cache.as_posix()] * 2)
            self.assertEqual(settings[2], "custom-launcher")
            self.configure(source, build, "-DKIN_COMPILER_CACHE=off")
            settings = (build / "settings.txt").read_text().splitlines()
            self.assertTrue(all(value.endswith("-NOTFOUND") for value in settings[:2]))
            self.assertEqual(settings[2], "custom-launcher")


if __name__ == "__main__":
    unittest.main(argv=[__file__, *REMAINING])
