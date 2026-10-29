# Building and validating the 3D renderer

The renderer remains opt-in with `KIN_EXPERIMENTAL_3D=ON`. Ordinary 2D and shipping
presets leave it out. Start a game with the [scene-session guide](rendering_3d.md#owning-a-3d-scene-session);
use [render graphs](render_graph.md) for explicit passes, resources or several cameras.

## Build and try it

Run from a checkout; replace the first path with your own. Install the compiler,
CMake, Ninja and [Linux development dependencies](../README.md#building) first.
Native GPU builds also need `glslc` on PATH, or `-DKIN_GLSLC=/absolute/path/to/glslc`.

```sh
cd /path/to/kin
cmake --preset 3d
cmake --build --preset 3d
./build-3d/bin/kin_vision3d
```

The asset-free visibility example draws a normal camera above three observer
masks. Space toggles the wall; Escape exits. Grey means no usable observation,
red means zero target pixels, and green means nonzero pixels. Try
`--resolution 128 --budget 2`; `--help` lists capture/test options.

Other asset-free graph examples are `kin_outline3d`, `kin_ascii3d` and
`kin_surface3d`. The surface example composes explicit shadows, HDR shading,
transparency, bloom/output and ASCII. `kin_slowlight3d` is a tools-only experiment;
its simulation is not part of the stock renderer.

For Sponza, populate the reference cache with `python3 tools/reference/fetch.py`
(downloads the locked reference assets), or supply your own glTF with
`--model /path/to/model.gltf`:

```sh
cd /path/to/kin
./build-3d/bin/sponza_walk --preset gi --width 1280 --height 720
./build-3d/bin/sponza_walk --preset gi --width 2560 --height 1440
```

GI is off in the viewer's baseline preset; `--preset gi` enables it. The panel
also exposes voxel size, levels, ray/update budgets and intensity. WASD moves,
Tab/Escape releases the mouse and shows the panel, F3 shows stats, and F12 saves
a screenshot. Test GI window boundaries, camera cuts, resizing, AO/TAA toggles,
animated meshes and your own imported models.

Stock GI uses clipmap windows with continuous transitions between levels and
the ambient/environment fallback. Include floors and roofs in its requested
area, and allow probes time to converge. Sparse/octree GI is still a
[planned extension](rendering_pipeline_plan.md#optional-adaptive-voxel-gi-experiment),
not a selectable implementation. See the [GI contract](rendering_3d.md#global-illumination)
for budgets, memory and limitations, and [loading/reload](rendering_3d.md#direct-loading-and-reload)
for imported-content workflows.

On Windows, use a Visual Studio 2022 developer prompt and install `glslc`:

```powershell
cd C:\path\to\kin
cmake -S . -B build-3d-vs -G "Visual Studio 17 2022" -A x64 -DKIN_EXPERIMENTAL_3D=ON
cmake --build build-3d-vs --config Release --parallel 4
.\build-3d-vs\bin\Release\kin_vision3d.exe
ctest --test-dir build-3d-vs -C Release --output-on-failure -j 2
```

## Automated checks

Run the normal suite with a working display and Vulkan device:

```sh
cd /path/to/kin
KIN_REQUIRE_GPU_TESTS=1 ctest --preset 3d --timeout 300
python3 tools/format_3d.py --check --clang-format /path/to/clang-format-18
```

`KIN_REQUIRE_GPU_TESTS=1` turns an unavailable GPU into a test failure. Without it,
GPU-dependent tests may skip with exit 77. A dummy SDL video driver does not
exercise rendering. On NVIDIA/X11, five simultaneous-foreign-device tests retain
an existing SDL teardown guard; software Vulkan exercises those cases.

For the dedicated 21-case graph/visibility/resource gate on Ubuntu, install
`mesa-vulkan-drivers vulkan-validationlayers xvfb glslc` and use a build with tests
and image I/O enabled. The gate rejects missing or skipped tests, so use software
Vulkan for its foreign-device cases:

```sh
cd /path/to/kin
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
xvfb-run -a -s "-screen 0 1280x800x24" python3 .github/scripts/rendering-gpu-check.py \
  --build build-3d --mode core --output out/rendering-core
xvfb-run -a -s "-screen 0 1280x800x24" python3 .github/scripts/rendering-gpu-check.py \
  --build build-3d --mode sync --output out/rendering-sync
```

Each output contains verbose `ctest.log`, JUnit `ctest.xml` and `summary.json`.
Both modes require loader evidence that Khronos validation was inserted for
every selected test, and fail on validation/synchronization error markers even
when the executable exits zero. Compatibility warnings remain in the logs for
review. `--mode plain` applies the inventory/skip checks without enabling
validation; the sanitizer job uses it.

The [CI workflow](../.github/workflows/ci.yml) runs on pull requests, main and
`exp/rendering-*` pushes, alongside its existing branch triggers:

| Job | What it establishes |
|---|---|
| Linux GCC/Clang and Windows MSVC | Full compilation/tests, including opt-in 3D and SPIR-V shaders; dummy-driver jobs do not establish GPU runtime coverage |
| Linux 2D-only and shipping profiles | Component isolation and existing export behavior |
| Release software Vulkan | Entire configured CTest suite, then the 21 rendering cases under core and synchronization validation |
| Debug software Vulkan | Scene/physics checks plus graph, visibility, bounds, material and lifetime cases with assertions enabled |
| Rendering ASAN/UBSAN | Rendering-only Debug build, the same 21 integration cases on software Vulkan, fail-fast sanitizer errors; LeakSanitizer disabled |
| [WebGL2 workflow](../.github/workflows/webgl2.yml) | Packaged browser fixture, Hex and Summit smoke checks, including storage-denied behavior and simulation |

GPU/validation/sanitizer jobs upload JUnit and verbose evidence even after
failures. Inspect the run for the exact reviewed commit; a previous commit's
green check does not qualify a later runtime change. Focused graph checks do
not replace the full GPU suite. Sanitizers are scoped and do not claim a
complete engine or driver leak audit.

Native shaders currently select SDL_GPU's Vulkan backend. Windows compilation
and browser SwiftShader checks do not establish native Windows GPU behavior,
Metal/D3D12 support, macOS support or coverage of every physical GPU.

## Multiple cameras and asynchronous observations

Prepare a scene once, set each camera before recording its graph and restore the
main camera before recording the main view. Prepared handles borrow their source
and expire at submission/presentation/readback; record every consumer first.
Each stock view owns its own GI/TAA history. The [visibility contract](render_graph.md#multiple-cameras-and-visibility-masks)
documents target IDs, cutouts, skinning, ignored self geometry and formats.

`tools/vision3d/vision.hpp` demonstrates bounded rotating updates, GPU count
reduction and a small asynchronous readback. It is example code, outside engine
AI systems. Results retain their source frame and caller token/generation;
discard stale results and treat invalid or unscheduled observations as unknown.
Low-resolution counts are approximate geometric visibility. Blended surfaces,
particles and custom displacement are excluded; lighting-based perception and
gameplay confidence policies belong to the game. Polling never waits, while
destruction can wait to drain pending reads.

## Scene quality, timing and memory evaluation

Use the [evaluation tools](../tools/eval/README.md) for Sponza, Industrial Yard,
Bistro or arbitrary adapters. Large scene assets and rendered artifacts stay
outside Git. Bistro needs its external source/content paths; it is not bundled
with CI.

Keep baseline/candidate binaries and output directories separate. Measure normal
frame throughput independently from GPU pass markers and readbacks; include
warm-up, repeated alternating runs, effective settings and hardware telemetry.
Evaluate both resolutions with identical cameras. Compare quality against two
independent Cycles seeds and retain their disagreement as the reference noise
floor; do not subtract it into an accuracy claim. Owned texture/buffer payload
counters are not physical VRAM residency.

The [dated readiness report](rendering_readiness_2026-10-28.md) preserves previous
measurements and build identities. Its older GI-disabled Sponza and thermally
throttled timings are not new results for the current GI transitions or
visibility implementation. Larger-scene load/unload/reload and target-hardware
checks remain separate from the small integration fixtures.
