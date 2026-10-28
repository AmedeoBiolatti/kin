# Rendering evaluation

`eval.py` runs independent performance, capture, reference, scoring and stress jobs.
Scene adapters keep their own loaders; a fourth application can implement the JSON
command protocol without changing the engine or this runner. Large content and
rendered artifacts stay outside Git.

## Build and run

Build Release binaries with experimental 3D, scene runtime, physics3d, image I/O,
benchmarks and tests enabled. The native targets are `kin_reference` and
`kin_eval_yard` (or the ordinary `industrial_yard` example). To include the external
Bistro game, configure `KIN_EVAL_BISTRO_SOURCE=/path/to/gallery/bistro`, with
`KIN_ENABLE_AUDIO=ON` and `KIN_ENABLE_TTF=ON`, then build `kin_eval_bistro`.
This compiles the game against the selected engine; it does not modify its source.

Keep independent baseline/candidate binaries. Each build file is:

```json
{
  "schema": "kin.eval-build/1",
  "source_root": "/absolute/path/to/checkout",
  "executables": {
    "reference": "/absolute/path/to/bin/kin_reference",
    "yard": "/absolute/path/to/bin/kin_eval_yard",
    "bistro": "/absolute/path/to/bin/kin_eval_bistro"
  },
  "args": []
}
```

The orchestrator needs Python 3.10+. Capture validation and basic scoring need
Pillow and NumPy. Optional scoring requirements are in `requirements-metrics.txt`;
use a dedicated virtual environment (the pinned set was tested with Python 3.14).
Cycles needs a Blender executable. The tool
never installs packages or downloads scene assets automatically.

```sh
python3 tools/eval/eval.py validate tools/eval/suites/rendering.json \
  --scene sponza --baseline /path/baseline.json --candidate /path/candidate.json
python3 tools/eval/eval.py run tools/eval/suites/rendering.json \
  --scene sponza --profile smoke \
  --baseline /path/baseline.json --candidate /path/candidate.json \
  --blender /path/blender --metrics-python /path/venv/bin/python \
  --out /path/results/smoke-01
```

Sponza uses the existing reference asset cache (`~/.cache/kin/reference`). Yard
uses its deterministic YRD1 generator, or a manifest's `source` recipe. Bistro
uses `KIN_BISTRO_PROJECT` and `KIN_BISTRO_CONTENT` environment variables for its
existing source and baked content. Unselected scenes do not require those paths.
Start with one scene and a smoke run before expensive full-resolution references.

Commands `bench`, `capture`, `truth`, `score`, and `stress` select a single stage.
Use the same suite, profile and output directory with `--resume` to use completed
parent stages. `report RUN` regenerates the offline HTML, JSON and Markdown report.
`--dry-run` prints the resolved plan and observed provenance without launching jobs.
Exit codes: 0 complete, 1 failed work, 2 configuration error, 3 incomplete work.
`--allow-unavailable` permits exit 0 for incomplete work while retaining that
status in artifacts. Missing providers, dependencies and unsupported capabilities
never become passing measurements.

## Suites, profiles and scene adapters

`kin.eval-suite/1` contains `scenes` (inline objects or paths), `builds` (inline
objects or build-file paths), `profiles`, optional `tools`, `display`, `cache`
and `thresholds`. Paths are relative to their declaring file; `~` and environment
variables are expanded. IDs must be unique safe filename components. Unknown
suite keys, non-finite JSON and duplicate JSON keys are rejected.

Smoke defaults: one repeat, four warm-up/eight measured frames, 320x180, two
16-sample Cycles seeds, basic metrics. Standard: five alternating repeats, 300
warm-up/1500 measured frames, 1920x1080, two 1024-sample seeds, FLIP and JOD too.
Both are starting configurations, not evidence of adequate convergence. Override
`runs`, `warmup`, `frames`, `size`, `samples`, `seeds`, `metrics`, `stages`, `timeout`,
`max_score_pixels`, `stress_cycles`, `stress_hold_frames`, `stress_settle_frames`, or
`stress_tolerance_bytes` in a suite profile. A clip defaults to at most 128 million
pixels; built-in adapters reject oversized captures before rendering. Size/FPS,
frames and display models must match; scoring never crops or resizes images.

Scene manifests (`kin.eval-scene/1`) require `id` and `adapter`:

- `reference`: `source` is any supported reference scene JSON, with `asset_root`
  and optional `views`. Static scenes with a declared `truth` mapping use the
  existing Blender exporter. An optional `timeline` defines frame-indexed
  eye/target/perspective-FOV keys, FPS, cuts and captured frame IDs. Each renderer
  consumes the resolved cameras. Consecutive captures form a moving clip; one
  capture is a still. Sparse samples need separate still cases. Paths hold their
  final camera when a longer benchmark is requested. Procedural simulation is
  fixed at 60 Hz; unsupported procedural/animated Cycles exports are explicit.
- `yard`: optional `source`, otherwise `tier` selects a generated recipe.
  `args` select workload counts/features; `capture: {"from":0,"count":12}`
  selects a consecutive route clip at 60 Hz. Warm-up is limited to 300 and the
  measured range to 6000 frames by this application. Existing CSV workload
  counters are joined to raw runtime records by native frame ID.
- `bistro`: `source` points to its views JSON; `asset_root`, `project` and
  `script` identify baked content and the reviewed Cycles exporter. `views`
  selects cases, `settle_frames` defaults to 600, and `capture.count` can request
  a static clip. Its path's explicit FPS/hold/keyframes define moving clips.
  Quality defaults to 1. Fog and bloom are disabled for the reviewed truth
  comparison. The ISO/ACES/saturation mapping is recorded explicitly. Night
  lamp references are unsupported. A short benchmark may only exercise a path's
  hold section; choose a warm-up/window that includes the intended movement.
- `command`: supplies argv arrays under `commands` for supported job kinds.
  See [protocol.md](protocol.md). List all external data/scripts in `dependencies`.

Additional build/scene `args` are literal argv elements. Built-in adapters reject
attempts to override flags they own, including output paths, dimensions and
capture controls. Configuration for a custom renderer belongs in its manifest
and adapter. Applications are responsible for deterministic animation, physics,
lights, random seeds and state export; matching a camera alone does not prove
that two simulators produced equivalent scene states.

## Measurements and interpretation

Performance processes do not capture pixels, score images or enable synchronized
pass markers. `KIN_EVAL_FRAME_LOG` opts into raw `kin.eval-frame/1` JSONL records;
ordinary runtime execution opens no evaluation file. Reference benchmarks also
avoid the runner's old final-image and device-idle operations. Explicit readback
belongs in capture jobs, not throughput measurements.

CPU interval, work, update, render and present values have distinct meanings.
Interval includes scheduling, waiting and preceding logging; work excludes JSON
formatting. Logging remains real overhead. Reported p50/p95/p99 are calculated
from measured frame records, separately per view and repeat. Baseline/candidate
process order alternates between repeats. Reports use paired changes and a
bootstrap interval only with at least three independent repeat pairs; smaller
runs are labelled diagnostic. Review flags use the configured percentage
(default 5%) plus a positive interval. They do not automatically reject a patch.

GPU times currently come from fence completion, can include CPU recording gaps,
and are not attributable to an exact source frame. Repeated observations are
deduplicated by sample serial. **No GPU frame-time percentiles are claimed.**
Timestamp queries are a future provider, not fabricated from those estimates.

Memory records cover Kin-owned descriptor payload, idle texture pools, upload
staging, allocation/release and copy counters. They exclude driver alignment,
SDL cycle backings and pending retirement. One-second process RSS/high-water
and device-wide NVIDIA memory/power/clock/temperature observations are saved
where available. They are sampled diagnostics, not per-process GPU residency.
The sampler can perturb timings; keep it consistent between variants.

Reference stress prewarms TAA/AO allocations, disables them and waits 600 frames
by default before establishing its initial plateau. This removes startup upload
and texture-pool slack that could hide a later leak. It then repeatedly resizes
one persistent view and toggles TAA/AO, restores base size/features, waits the
same settle period, and checks stable final versus initial owned payload plateaus. Reports list unexercised asset/shader reload,
scene load/unload and multiple-view cases. Yard/Bistro do not yet provide this
persistent stress protocol. Use a command provider for those lifecycle tests.

## Quality and reference confidence

The scorer retains the existing Bistro display math and metric definitions:
FLIP mean/worst frame, image or video ColorVideoVDP JOD, RGB PSNR/bias, dark/bright
bias split by reference median luma, encoded-luma Laplacian detail ratio, static
luma shimmer and fraction of pixels ever stepping more than eight levels. Moving
clips instead report the error between candidate and reference frame differences.
Kin's encoded-luma SSIM is an additional diagnostic with its original coefficients.
A perfect PSNR is JSON `null` plus `identical:true`; an undefined flat-image detail
ratio is `null`, never NaN or a manufactured perfect score.

Each Cycles comparison also scores two independent reference seeds to estimate
noise. Increase samples until that floor is sufficiently below the differences
under investigation. A successful low-sample smoke run does not establish visual
quality. Stills and clips are separate JOD measurements; moving video requires
one matched reference per consecutive frame and explicit FPS. Raw scalar FLIP
maps and displayed reference/test/difference previews are retained.

`srgb` means an already displayed 8-bit image. Linear EXRs require an explicit
supported transform (`bistro_cycles` currently); Bistro port captures get only
the reviewed saturation adjustment. Other exporters should produce correctly
encoded PNGs or add a reviewed transform. No automatic exposure fitting occurs.
The included laptop model reproduces the earlier agent's *assumed* panel geometry
and photometry. Set a display appropriate to the intended size/viewing conditions;
small smoke images are not calibrated full-panel quality claims.

Sponza has a reviewed static glTF Cycles path. **Yard currently uses baseline
image regression, not Cycles accuracy**, because its procedural/export mapping is
not reviewed. It says so in results. Automatic reference-convergence gates, ROIs,
LPIPS and full reload/multiview lifecycle tests remain future providers.

## Reproducibility and caching

The frozen run manifest hashes binaries, evaluator/exporter code, declared input
files (including external glTF buffers/textures), metric package versions and
display files. Hardware/driver identity and relevant rendering environment are
recorded; input drift is checked at completion. A build's optional `source_root`
is descriptive metadata; a binary hash alone does not attest that it was compiled
from that checkout. Preserve compiler/CMake flags with the build artifacts.

Completed jobs have a hash inventory of **every** artifact. `--resume` rejects
changed plans/builds/providers and repairs missing or changed middle frames by
rerunning the affected job. Old attempts are retained. Timeouts/interruption kill
the child process group on POSIX; incomplete jobs restart at job granularity.
The advisory GPU lock serializes cooperating evaluator processes, not unrelated
applications using the device.

Cycles jobs additionally use a content-addressed shared cache (default
`~/.cache/kin/eval/truth`; override suite `cache`). Its identity includes source,
content, cameras, export/display state, samples, seeds, Blender/provider code and
render context, excluding engine binaries. Changing the candidate alone reuses
verified truth. Corrupted cache entries are rebuilt. A cache entry is published
only after its output validation and inventory complete.

Run the protocol/metric tests with:

```sh
python -m unittest discover -s tests/eval -v
ctest --test-dir /path/build -R kin-eval-python --output-on-failure
```
