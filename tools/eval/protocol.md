# External scene command protocol, version 1

The runner executes argv arrays, never shell fragments. Only `{job}`, `{out}`,
`{binary}` and `{source}` are substituted; other literal braces survive. Paths
can contain spaces. Declare scripts/assets in `dependencies` for invalidation.
For example, a scene can use its existing executable plus a small adapter:

```json
{
  "schema": "kin.eval-scene/1",
  "id": "my_scene",
  "adapter": "command",
  "source": "/data/my-scene.glb",
  "dependencies": ["/tools/my_adapter.py"],
  "commands": {
    "bench": ["python3", "/tools/my_adapter.py", "{job}", "{out}"],
    "capture": ["python3", "/tools/my_adapter.py", "{job}", "{out}"],
    "truth": ["python3", "/tools/my_adapter.py", "{job}", "{out}"],
    "stress": ["python3", "/tools/my_adapter.py", "{job}", "{out}"]
  },
  "reference": {"provider": "command"}
}
```

The job file (`kin.eval-job/1`) contains `kind`, `scene`, `profile`, `variant`,
`repeat`, `binary`, `build`, `tools` and `display`. Truth receives `capture`
metadata. A parent artifact is `{root,result,artifact_identity}`: referenced
images stay in its root. Treat inputs as read-only. Write outputs within `{out}`;
absolute/escaping image and frame-file paths are rejected. No messages to external
services or engine modifications are required.

Write `result.json` before exiting successfully:

```json
{
  "schema": "kin.eval-result/1",
  "status": "ok",
  "size": [1280, 720],
  "frames_file": "frames.jsonl",
  "sequences": [],
  "effective": {"quality": "high", "seed": 7, "simulation_hz": 60}
}
```

Bench requires `frames_file`; it must contain exactly `profile.frames` unique
integer frame IDs **per view**. A JSONL record is:

```json
{"schema":"kin.eval-frame/1","view":"main","frame":300,"cpu":{"interval_ms":4.2,"work_ms":3.8,"update_ms":0.5,"render_ms":2.9,"present_ms":0.4},"memory":{"owned_bytes":1048576,"pooled_texture_bytes":0,"staging_bytes":65536,"allocations":30,"releases":4},"gpu":{"method":"fence_completion_estimate","sample_serial":25,"latest_ms":3.1,"source_frame":null,"percentiles_valid":false},"workload":{"draws":120,"triangles":900000}}
```

CPU values are finite nonnegative milliseconds, memory/copy sizes bytes.
Workload counters are optional and application-specific. Remove warm-up records
from `frames_file`; retain an unfiltered log separately. Captures/readback must
not occur during this stage. Do not re-emit summary medians as fake frame records.

Capture requires nonempty `sequences` instead of timing records:

```json
{
  "schema": "kin.eval-result/1",
  "status": "ok",
  "size": [1280, 720],
  "sequences": [{
    "id": "walk", "mode": "moving", "fps": 60,
    "transform": {"kind": "srgb"},
    "frames": [
      {"frame":100,"image":"walk/100.png","eye":[0,1,5],"target":[0,1,0],"fov":60},
      {"frame":101,"image":"walk/101.png","eye":[0.1,1,5],"target":[0.1,1,0],"fov":60}
    ]
  }]
}
```

`still` means exactly one frame. `static` and `moving` require at least two
consecutive IDs and explicit positive FPS. Frames/images, dimensions, IDs, mode,
FPS and shared camera fields must match the reference. Additional scene-state
metadata is encouraged, but the runner cannot establish cross-simulator state
parity by itself. Settle/hold/history reset and random seeds belong in the adapter.

Truth returns `seeds: {"0": [sequences], "1": [sequences]}` with the same frame
metadata and distinct image paths, and `reference_role:"cycles"` for a reviewed
Cycles export. Two independent seeds are required for Cycles scoring. Other
reviewed providers can use a descriptive role. A baseline regression instead
sets scene `reference.provider:"baseline"` and compares baseline captures; the
runner will not describe those as Cycles ground truth.

Stress may provide frame records plus a `stress` object describing exercised
operations, initial/final plateaus, tolerance, success and unexercised cases.
Perform reload/resize/load/unload in one persistent process when testing retained
memory. A process restart alone cannot establish absence of a lifetime leak.

Missing capability: return `{"schema":"kin.eval-result/1","status":"unsupported",
"reason":"specific missing capability"}`. Crashes, nonzero exit, timeout,
malformed/incomplete frames, or mismatched images fail the job. Keep full stdout,
raw inputs and effective configuration for diagnosis. The runner owns completion
markers, inventories, cache publication and report generation.

`tests/eval/adapter_fixture.py` is a minimal fourth-adapter example exercised by
the contract tests, including scoring and shared reference reuse across different
engine variants. It is synthetic protocol coverage, not renderer validation.
