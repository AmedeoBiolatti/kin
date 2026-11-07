"""Check incremental shader builds with real glslc dependency files."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PARSER = argparse.ArgumentParser()
PARSER.add_argument("--cmake", default="cmake")
PARSER.add_argument("--glslc", default=shutil.which("glslc"))
PARSER.add_argument("--generator", default="Ninja" if shutil.which("ninja") else None)
ARGS, REMAINING = PARSER.parse_known_args()


@unittest.skipUnless(ARGS.glslc, "glslc is not installed")
class ShaderBuildTests(unittest.TestCase):
    def run_command(self, *command):
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def test_selective_transitive_dependencies_and_unchanged_embedding(self):
        with tempfile.TemporaryDirectory(prefix="kin shader build ") as directory:
            source = Path(directory) / "source with spaces"
            build = Path(directory) / "build with spaces"
            source.mkdir()
            (source / "kin").mkdir()
            (source / "kin/CMakeLists.txt").write_text(
                'include("${KIN_ROOT}/cmake/kin_embed.cmake")\n')
            includes = source / "inc"
            includes.mkdir()
            leaf = includes / "leaf with space.inc.glsl"
            (includes / "leaf").write_text("An existing prefix is not a dependency.\n")
            leaf.write_text("const float OFFSET = 0.0;\n")
            shared = includes / "shared.inc.glsl"
            shared.write_text('#include "leaf with space.inc.glsl"\n')
            used = source / "used.vert.glsl"
            used.write_text('#version 450\n'
                            '#extension GL_GOOGLE_include_directive : require\n'
                            '#include "inc/shared.inc.glsl"\n'
                            'void main() { gl_Position = vec4(OFFSET); }\n')
            (source / "other.vert.glsl").write_text(
                '#version 450\nvoid main() { gl_Position = vec4(0.0); }\n')
            (source / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.22)
project(shader_build_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
add_subdirectory(kin)
kin_compile_glsl(shaders OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    SOURCES used.vert.glsl other.vert.glsl)
add_library(embedded STATIC)
kin_embed_files(embedded NAME fixture FILES ${shaders})
''')
            command = [ARGS.cmake, "-S", str(source), "-B", str(build),
                       f"-DKIN_ROOT={ROOT.as_posix()}", f"-DKIN_GLSLC={ARGS.glslc}"]
            if ARGS.generator:
                command += ["-G", ARGS.generator]
            self.run_command(*command)

            def rebuild():
                return self.run_command(ARGS.cmake, "--build", str(build),
                                        "--config", "Release", "--parallel", "2")

            rebuild()
            spv = build / "shaders/used.vert.spv"
            other = build / "shaders/other.vert.spv"
            embedded = build / "kin_embed/embedded/fixture.cpp"
            def digest():
                return hashlib.sha256(spv.read_bytes()).hexdigest()

            first = digest()
            other_time = other.stat().st_mtime_ns
            used_time = spv.stat().st_mtime_ns
            rebuild()
            self.assertEqual(spv.stat().st_mtime_ns, used_time)
            self.assertEqual(other.stat().st_mtime_ns, other_time)

            # A nested include changes only the shader which actually uses it.
            leaf.write_text("const float OFFSET = 1.0;\n")
            rebuild()
            self.assertNotEqual(digest(), first)
            self.assertEqual(other.stat().st_mtime_ns, other_time)

            # Discover a new dependency after editing an existing include.
            added = includes / "added.inc.glsl"
            added.write_text("const float OFFSET = 2.0;\n")
            shared.write_text('#include "added.inc.glsl"\n')
            rebuild()
            before = digest()
            added.write_text("const float OFFSET = 3.0;\n")
            rebuild()
            self.assertNotEqual(digest(), before)
            self.assertEqual(other.stat().st_mtime_ns, other_time)

            # Identical SPIR-V must not rewrite/recompile its C++ embedding.
            before = digest()
            embedded_time = embedded.stat().st_mtime_ns
            used.write_text(used.read_text() + "\n// No bytecode change.\n")
            rebuild()
            self.assertEqual(digest(), before)
            self.assertEqual(embedded.stat().st_mtime_ns, embedded_time)


if __name__ == "__main__":
    unittest.main(argv=[__file__, *REMAINING])
