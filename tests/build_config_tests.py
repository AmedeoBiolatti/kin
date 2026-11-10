"""Exercise developer build settings from a parent game's CMake project."""

import argparse
import json
import os
from pathlib import Path
import shlex
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
add_library(script_bindings STATIC EXCLUDE_FROM_ALL engine.cpp)
target_compile_options(script_bindings PRIVATE
    "$<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang>>:-O2>"
    "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Werror=invalid-pch>")
kin_configure_scripting_build(script_bindings)
kin_precompile_target(script_bindings)
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
file(WRITE "${CMAKE_BINARY_DIR}/compiler-id.txt" "${CMAKE_CXX_COMPILER_ID}")
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
            for launcher in settings[:2]:
                self.assertTrue(cache.samefile(launcher),
                                f"{launcher} does not refer to {cache}")
            self.assertEqual(settings[2], "custom-launcher")
            self.configure(source, build, "-DKIN_COMPILER_CACHE=off")
            settings = (build / "settings.txt").read_text().splitlines()
            self.assertTrue(all(value.endswith("-NOTFOUND") for value in settings[:2]))
            self.assertEqual(settings[2], "custom-launcher")

    def test_on_demand_build_keeps_tests_and_builds_only_requested_targets(self):
        with tempfile.TemporaryDirectory(prefix="kin on demand ") as directory:
            source, build = self.fixture(Path(directory))
            optional = source / "optional"
            optional.mkdir()
            (optional / "library.cpp").write_text("int optional_value() { return 7; }\n")
            (optional / "main.cpp").write_text(
                "int optional_value();\nint main() { return optional_value() == 7 ? 0 : 1; }\n")
            (optional / "CMakeLists.txt").write_text('''
add_library(optional_dependency STATIC library.cpp)
add_subdirectory(nested)
foreach(target focused sibling)
    add_executable(${target} main.cpp)
    target_link_libraries(${target} PRIVATE optional_dependency)
    add_test(NAME ${target} COMMAND ${target})
endforeach()
install(TARGETS focused RUNTIME DESTINATION bin COMPONENT focused)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/optional-paths-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:focused>\\n$<TARGET_FILE:sibling>\\n$<TARGET_FILE:optional_dependency>\\n")
''')
            (optional / "nested").mkdir()
            (optional / "nested/CMakeLists.txt").write_text('''
add_custom_target(nested_output ALL
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/nested-output")
''')
            with (source / "CMakeLists.txt").open("a") as stream:
                stream.write('''
include(CTest)
kin_add_optional_subdirectory(optional)
''')
            self.configure(source, build, "-DKIN_BUILD_ON_DEMAND=ON",
                           "-DCMAKE_BUILD_TYPE=Release", "-DKIN_COMPILER_CACHE=off")
            paths = [Path(p) for p in
                     (build / "optional-paths-Release.txt").read_text().splitlines()]
            self.run_command(ARGS.cmake, "--build", str(build), "--config", "Release")
            self.assertTrue(all(not p.exists() for p in paths), paths)
            self.assertFalse((build / "nested-output").exists())
            ctest = str(Path(shutil.which(ARGS.cmake) or ARGS.cmake).with_name(
                "ctest.exe" if os.name == "nt" else "ctest"))
            listing = json.loads(self.run_command(ctest, "--test-dir", str(build),
                                                 "-C", "Release", "--show-only=json-v1"))
            self.assertEqual({t["name"] for t in listing["tests"]}, {"focused", "sibling"})
            self.run_command(ARGS.cmake, "--build", str(build), "--config", "Release",
                             "--target", "focused")
            self.assertTrue(paths[0].exists())
            self.assertFalse(paths[1].exists())
            self.assertTrue(paths[2].exists())
            self.run_command(ctest, "--test-dir", str(build), "-C", "Release",
                             "-R", "^focused$", "--output-on-failure")
            stage = Path(directory) / "stage"
            self.run_command(ARGS.cmake, "--install", str(build), "--config", "Release",
                             "--component", "focused", "--prefix", str(stage))
            self.assertTrue((stage / "bin" / paths[0].name).exists())
            # Reuse the same build: disabling the option restores the full build.
            self.configure(source, build, "-DKIN_BUILD_ON_DEMAND=OFF")
            self.run_command(ARGS.cmake, "--build", str(build), "--config", "Release")
            self.assertTrue(all(p.exists() for p in paths), paths)
            self.assertTrue((build / "nested-output").exists())
            self.run_command(ctest, "--test-dir", str(build), "-C", "Release",
                             "--output-on-failure")

    def test_fast_scripting_is_private_and_uses_a_matching_pch(self):
        with tempfile.TemporaryDirectory(prefix="kin fast scripting ") as directory:
            source, build = self.fixture(Path(directory))
            self.configure(source, build, "-DKIN_ENABLE_PCH=ON", "-DKIN_COMPILER_CACHE=off",
                           "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CONFIGURATION_TYPES=Release",
                           "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
            if (build / "compiler-id.txt").read_text() not in ("GNU", "Clang"):
                self.skipTest("Fast scripting requires GCC or Clang")
            if not (build / "compile_commands.json").exists():
                self.skipTest("Generator does not export compile commands")

            def commands():
                return json.loads((build / "compile_commands.json").read_text())

            def check_binding_optimization(expected):
                entries = [e for e in commands() if "script_bindings.dir" in e["command"]]
                self.assertEqual(len(entries), 2)  # source and PCH
                for entry in entries:
                    options = [x for x in shlex.split(entry["command"]) if x.startswith("-O")]
                    self.assertEqual(options[-1], expected, entry["command"])

            check_binding_optimization("-O2")  # Default remains unchanged.
            self.configure(source, build, "-DKIN_DEV_FAST_SCRIPTING=ON")
            check_binding_optimization("-O1")
            for entry in commands():
                if "script_bindings.dir" not in entry["command"]:
                    self.assertNotIn("-O1", shlex.split(entry["command"]))
            output = self.run_command(ARGS.cmake, "--build", str(build), "--config", "Release",
                                      "--target", "script_bindings", "--parallel", "2")
            self.assertNotIn("not used because", output)
            self.assertNotIn("invalid precompiled", output.lower())
            self.configure(source, build, "-DKIN_DEV_FAST_SCRIPTING=OFF")
            check_binding_optimization("-O2")
            self.run_command(ARGS.cmake, "--build", str(build), "--config", "Release",
                             "--target", "script_bindings", "--parallel", "2")

            for config in ("RelWithDebInfo", "Debug", "MinSizeRel"):
                with self.subTest(config=config):
                    self.configure(source, build, "-DKIN_DEV_FAST_SCRIPTING=ON",
                                   f"-DCMAKE_BUILD_TYPE={config}", f"-DCMAKE_CONFIGURATION_TYPES={config}")
                    if config == "RelWithDebInfo":
                        check_binding_optimization("-O1")
                    else:
                        for entry in commands():
                            self.assertNotIn("-O1", shlex.split(entry["command"]))

    def test_fast_scripting_rejects_shipping_and_unsupported_compilers(self):
        with tempfile.TemporaryDirectory(prefix="kin fast scripting policy ") as directory:
            script = Path(directory) / "check.cmake"
            for shipping, compiler, message in (("ON", "GNU", "disable it for KIN_SHIPPING"),
                                                ("OFF", "MSVC", "requires GCC or Clang")):
                with self.subTest(shipping=shipping, compiler=compiler):
                    script.write_text(f'''
cmake_minimum_required(VERSION 3.22)
set(KIN_DEV_FAST_SCRIPTING ON)
set(KIN_SHIPPING {shipping})
set(CMAKE_CXX_COMPILER_ID {compiler})
include("{ROOT.as_posix()}/cmake/kin_build.cmake")
kin_configure_scripting_build(unused)
''')
                    result = subprocess.run([ARGS.cmake, "-P", str(script)], text=True,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn(message, result.stdout)


if __name__ == "__main__":
    unittest.main(argv=[__file__, *REMAINING])
