import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/eval"))
from kineval.common import (
    EvalError,
    canonical,
    fingerprint,
    read_json,
    rows,
    sha256,
    within,
    write_json,
)
from kineval.model import load_suite, timeline
from kineval.runner import (
    Runner,
    cached,
    inventory,
    percentile,
    summarize_frames,
    validate_result,
)
from kineval.report import generate, paired_change

HAS_IMAGES = (
    importlib.util.find_spec("numpy") is not None
    and importlib.util.find_spec("PIL") is not None
)


class EvaluationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="kin eval tests ")
        self.root = Path(self.tmp.name)
        self.fixture = ROOT / "tests/eval/adapter_fixture.py"
        self.scene = {
            "schema": "kin.eval-scene/1",
            "id": "fourth",
            "adapter": "command",
            "dependencies": [str(self.fixture)],
            "commands": {
                kind: [sys.executable, str(self.fixture), "{job}", "{out}"]
                for kind in ("bench", "capture", "stress")
            },
            "reference": {"provider": "baseline"},
        }
        self.suite = {
            "schema": "kin.eval-suite/1",
            "cache": str(self.root / "cache"),
            "builds": {
                "baseline": {"executables": {"command": sys.executable}},
                "candidate": {"executables": {"command": sys.executable}},
            },
            "scenes": [self.scene],
            "profiles": {
                "smoke": {
                    "stages": ["bench", "capture", "score"],
                    "frames": 5,
                    "size": [16, 12],
                }
            },
        }
        self.path = self.root / "suite.json"
        write_json(self.path, self.suite)

    def tearDown(self):
        self.tmp.cleanup()

    def plan(self):
        return load_suite(self.path)

    def run_plan(self, plan=None, out="run", resume=False, stages=None):
        with contextlib.redirect_stdout(io.StringIO()):
            runner = Runner(plan or self.plan(), self.root / out, resume)
            result = runner.execute(stages)
        return runner, result

    def test_help_standard_library_only(self):
        p = subprocess.run(
            [sys.executable, "-S", str(ROOT / "tools/eval/eval.py"), "--help"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_arbitrary_scene_paths_and_spaces(self):
        path = self.root / "arbitrary scene.json"
        write_json(path, self.scene)
        self.suite["scenes"] = [str(path)]
        write_json(self.path, self.suite)
        # The loader canonicalizes paths, including Windows TEMP's 8.3 alias.
        self.assertEqual(self.plan()["scenes"][0]["manifest"], str(path.resolve()))

    def test_duplicate_scene_ids_rejected(self):
        self.suite["scenes"].append(self.scene)
        write_json(self.path, self.suite)
        with self.assertRaises(EvalError):
            self.plan()

    def test_invalid_dimensions_and_nonfinite(self):
        for value in ([0, 3], [16], [float("nan"), 16]):
            self.suite["profiles"]["smoke"]["size"] = value
            if any(isinstance(x, float) for x in value):
                with self.assertRaises(ValueError):
                    write_json(self.path, self.suite)
            else:
                write_json(self.path, self.suite)
                with self.assertRaises(EvalError):
                    self.plan()

    def test_selected_scene_does_not_resolve_unselected_external_paths(self):
        self.suite["scenes"].append(
            {
                "schema": "kin.eval-scene/1",
                "id": "external",
                "adapter": "bistro",
                "source": "${UNDEFINED_EVAL_TEST_VARIABLE}/views.json",
            }
        )
        write_json(self.path, self.suite)
        self.assertEqual(len(load_suite(self.path, scenes=["fourth"])["scenes"]), 1)

    def test_timeline_is_frame_indexed(self):
        scene = {
            "timeline": {
                "fps": 120,
                "keys": [
                    {"frame": 0, "eye": [0, 0, 5], "target": [0, 0, 0], "fov": 60},
                    {"frame": 4, "eye": [4, 0, 5], "target": [4, 0, 0], "fov": 80},
                ],
                "capture": [1, 2, 3, 4],
            }
        }
        result = timeline(scene)
        self.assertEqual(result["frames"][2]["eye"], [2, 0, 5])
        self.assertEqual(result["frames"][2]["fov"], 70)
        scene["timeline"]["keys"][1]["frame"] = 0
        with self.assertRaises(EvalError):
            timeline(scene)

    def test_real_percentiles_not_summary_repetition(self):
        values = [
            {
                "schema": "kin.eval-frame/1",
                "view": "v",
                "frame": i,
                "cpu": {"interval_ms": v},
                "gpu": {"sample_serial": 4, "latest_ms": 2},
            }
            for i, v in enumerate([1, 2, 3, 100])
        ]
        s = summarize_frames(values)["v"]
        self.assertAlmostEqual(s["cpu"]["interval_ms"]["p95"], 85.45)
        self.assertEqual(s["gpu"]["fresh_observations"], 1)
        self.assertFalse(s["gpu"]["percentiles_valid"])

    def test_performance_only_end_to_end(self):
        plan = self.plan()
        plan["profile"]["stages"] = ["bench"]
        plan["profile"]["runs"] = 2
        runner, result = self.run_plan(plan)
        self.assertEqual(result["status"], "ok")
        self.assertEqual(len(result["jobs"]), 4)
        self.assertEqual(
            [j["variant"] for j in result["jobs"]],
            ["baseline", "candidate", "candidate", "baseline"],
        )
        p = generate(runner.out)
        self.assertTrue(p.is_file())

    @unittest.skipUnless(HAS_IMAGES, "NumPy/Pillow metric dependencies unavailable")
    def test_fourth_adapter_complete_and_resumable(self):
        runner, result = self.run_plan()
        self.assertEqual(result["status"], "ok")
        self.assertEqual(len(result["jobs"]), 6)
        files = {
            str(p): sha256(p) for p in (runner.out / "jobs").rglob("*") if p.is_file()
        }
        again, resumed = self.run_plan(resume=True)
        self.assertEqual(
            files,
            {str(p): sha256(p) for p in (again.out / "jobs").rglob("*") if p.is_file()},
        )
        report = generate(runner.out)
        self.assertIn("Reference: baseline", report.read_text())

    @unittest.skipUnless(HAS_IMAGES, "image dependency unavailable")
    def test_missing_middle_frame_invalidates_cache(self):
        runner, result = self.run_plan()
        entry = next(j for j in result["jobs"] if j["kind"] == "capture")
        folder = runner.out / entry["folder"]
        marker = read_json(folder / "complete.json")
        self.assertTrue(cached(folder, marker["key"]))
        (folder / "1.png").unlink()
        self.assertFalse(cached(folder, marker["key"]))
        again, result = self.run_plan(resume=True)
        self.assertTrue((folder / "1.png").is_file())
        self.assertEqual(result["status"], "ok")
        self.assertEqual(len(result["jobs"]), 6)

    def test_resume_rejects_changed_build_or_sources(self):
        plan = self.plan()
        plan["profile"]["stages"] = ["bench"]
        self.run_plan(plan)
        changed = copy.deepcopy(plan)
        changed["profile"]["frames"] = 6
        with self.assertRaises(EvalError):
            Runner(changed, self.root / "run", True)

    def test_unknown_adapter_capability_is_not_a_pass(self):
        plan = self.plan()
        plan["profile"]["stages"] = ["stress"]
        plan["scenes"][0]["commands"].pop("stress")
        _, result = self.run_plan(plan)
        self.assertEqual(result["status"], "incomplete")
        self.assertTrue(all(j["status"] == "unsupported" for j in result["jobs"]))

    def test_missing_dependency_is_not_silently_skipped(self):
        plan = self.plan()
        plan["profile"]["stages"] = ["score"]
        _, result = self.run_plan(plan)
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(len(result["jobs"]), 2)

    @unittest.skipUnless(HAS_IMAGES, "image dependency unavailable")
    def test_missing_parent_can_recover_without_stale_unsupported_results(self):
        _, result = self.run_plan(stages=["score"])
        self.assertEqual(result["status"], "incomplete")
        self.run_plan(resume=True, stages=["capture"])
        runner, result = self.run_plan(resume=True, stages=["score"])
        self.assertEqual(result["status"], "ok")
        self.assertEqual(len(result["jobs"]), 4)
        generate(runner.out)
        self.assertEqual(
            read_json(runner.out / "report/summary.json")["quality_cases"], 2
        )

    def test_output_path_cannot_escape(self):
        with self.assertRaises(EvalError):
            within(self.root, "../secret")
        with self.assertRaises(EvalError):
            within(self.root, str(self.root.parent / "secret"))

    def test_measured_frame_ids_must_be_unique(self):
        rec = {
            "schema": "kin.eval-frame/1",
            "view": "v",
            "frame": 0,
            "cpu": {"interval_ms": 1},
        }
        (self.root / "frames.jsonl").write_text(
            json.dumps(rec) + "\n" + json.dumps(rec) + "\n"
        )
        with self.assertRaises(EvalError):
            validate_result(
                self.root,
                {
                    "schema": "kin.eval-result/1",
                    "status": "ok",
                    "size": [16, 12],
                    "frames_file": "frames.jsonl",
                },
                self.plan()["profile"],
            )

    def test_duplicate_json_keys_rejected(self):
        path = self.root / "duplicate.json"
        path.write_text('{"frames":1,"frames":2}')
        with self.assertRaises(EvalError):
            read_json(path)

    def test_unknown_profile_and_settings_rejected(self):
        with self.assertRaises(EvalError):
            load_suite(self.path, profile="stnadard")
        self.suite["profiles"]["smoke"]["sample_count"] = 12
        write_json(self.path, self.suite)
        with self.assertRaises(EvalError):
            self.plan()

    def test_sequence_identifier_cannot_escape_preview_directory(self):
        result = {
            "schema": "kin.eval-result/1",
            "status": "ok",
            "size": [16, 12],
            "sequences": [
                {
                    "id": "../escape",
                    "mode": "still",
                    "fps": 60,
                    "frames": [{"frame": 0, "image": "0.png"}],
                }
            ],
        }
        with self.assertRaises(EvalError):
            validate_result(self.root, result, self.plan()["profile"])

    def test_sparse_video_timeline_rejected(self):
        scene = {
            "timeline": {
                "keys": [
                    {"frame": 0, "eye": [0, 0, 5], "target": [0, 0, 0]},
                    {"frame": 3, "eye": [1, 0, 5], "target": [0, 0, 0]},
                ],
                "capture": [1, 3],
            }
        }
        with self.assertRaises(EvalError):
            timeline(scene)

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "POSIX process group and /proc check"
    )
    def test_timeout_stops_child_process_group_and_preserves_logs(self):
        import os
        import signal
        import time
        from kineval.common import execute

        pid_file = self.root / "child.pid"
        program = "import subprocess,sys,time; from pathlib import Path; child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']); Path(sys.argv[1]).write_text(str(child.pid)); time.sleep(60)"
        try:
            with self.assertRaises(subprocess.TimeoutExpired):
                execute(
                    [sys.executable, "-c", program, str(pid_file)],
                    self.root,
                    timeout=0.5,
                )
            self.assertTrue((self.root / "command.json").is_file())
            self.assertTrue((self.root / "process.log").is_file())
            self.assertTrue(pid_file.is_file())
            pid = int(pid_file.read_text())
            status = Path(f"/proc/{pid}/stat")
            for _ in range(10):
                if not status.exists() or status.read_text().split()[2] == "Z":
                    break
                time.sleep(0.02)
            else:
                self.fail("timed-out adapter left a running child")
        finally:
            if pid_file.exists():
                try:
                    os.kill(int(pid_file.read_text()), signal.SIGKILL)
                except ProcessLookupError:
                    pass

    def test_interpreter_path_keeps_virtual_environment_symlink(self):
        from kineval.common import executable_path

        link = self.root / "python-link"
        try:
            link.symlink_to(sys.executable)
        except OSError:
            self.skipTest("symlinks unavailable")
        self.assertEqual(executable_path(link, self.root), link)
        self.suite["tools"] = {"metrics_python": str(link)}
        write_json(self.path, self.suite)
        self.assertEqual(self.plan()["tools"]["metrics_python"], str(link))

    def test_capture_budget_and_managed_flags(self):
        from kineval.adapters import capture_budget, checked_args

        job = {
            "profile": {"size": [1920, 1080]},
            "scene": {"args": ["--size=16x16"]},
            "build": {},
        }
        with self.assertRaises(EvalError):
            capture_budget(job, 200)
        with self.assertRaises(EvalError):
            checked_args(job, {"--size"})

    @unittest.skipUnless(HAS_IMAGES, "image dependency unavailable")
    def test_shared_truth_cache_survives_engine_variant_changes(self):
        self.scene["reference"] = {"provider": "command"}
        self.scene["commands"]["truth"] = [
            sys.executable,
            str(self.fixture),
            "{job}",
            "{out}",
        ]
        self.suite["profiles"]["smoke"]["stages"] = ["capture", "truth", "score"]
        write_json(self.path, self.suite)
        plan = self.plan()
        first, result = self.run_plan(plan)
        self.assertEqual(result["status"], "ok")
        truth = next(j for j in result["jobs"] if j["kind"] == "truth")
        a = read_json(first.out / truth["folder"] / "result.json")
        self.assertFalse(a["reference_cache_hit"])
        plan["builds"]["candidate"]["args"] = ["changed-engine-variant"]
        second, result = self.run_plan(plan, out="different-run")
        self.assertEqual(result["status"], "ok")
        truth = next(j for j in result["jobs"] if j["kind"] == "truth")
        b = read_json(second.out / truth["folder"] / "result.json")
        self.assertTrue(b["reference_cache_hit"])
        self.assertEqual(a["reference_cache_key"], b["reference_cache_key"])
        # Corrupted content is rejected even if paths and dimensions still exist.
        frame = second.out / truth["folder"] / "0.png"
        frame.write_bytes(frame.read_bytes() + b"corruption")
        key = read_json(second.out / truth["folder"] / "complete.json")["key"]
        self.assertFalse(cached(second.out / truth["folder"], key))

    def test_memory_stress_does_not_use_startup_slack_as_leak_budget(self):
        from unittest.mock import patch
        from kineval.common import write_rows

        camera = {"eye": [0, 0, 5], "target": [0, 0, 0], "fov": 60}
        job = {
            "scene": {"adapter": "reference"},
            "profile": {
                "size": [16, 16],
                "stress_cycles": 2,
                "stress_hold_frames": 2,
                "stress_settle_frames": 4,
                "stress_tolerance_bytes": 0,
            },
            "capture": {
                "result": {"status": "ok", "sequences": [{"frames": [camera]}]}
            },
        }

        def fake(derived, folder):
            n = len(derived["stress_frames"])
            records = [
                {
                    "memory": {
                        "owned_bytes": 1000000 if i < 2 else 200 if i >= n - 4 else 100
                    }
                }
                for i in range(n)
            ]
            write_rows(folder / "frames.jsonl", records)
            return {"frames_file": "frames.jsonl"}

        with patch("kineval.runner.adapters.reference", side_effect=fake):
            with self.assertRaises(EvalError):
                Runner.stress(None, job, self.root)

    def test_paired_runs_report_uncertainty(self):
        value = paired_change({i: 1 for i in range(5)}, {i: 1.1 for i in range(5)})
        self.assertAlmostEqual(value["percent"], 10)
        self.assertEqual(value["runs"], 5)
        self.assertGreater(value["interval_percent"][0], 0)


@unittest.skipUnless(HAS_IMAGES, "NumPy/Pillow unavailable")
class MetricTests(unittest.TestCase):
    def setUp(self):
        import numpy as np

        self.np = np
        self.ref = np.arange(96 * 128 * 3, dtype=np.uint8).reshape(96, 128, 3)

    def test_identity_is_finite_json_and_detail_defined(self):
        from kineval.metrics import compare

        result, _ = compare([self.ref], [self.ref], 60, "still", [])
        self.assertIsNone(result["summary"]["psnr_db"])
        self.assertTrue(result["summary"]["identical"])
        self.assertEqual(result["summary"]["detail_ratio"], 1)
        canonical(result)

    def test_flat_reference_detail_is_unavailable(self):
        from kineval.metrics import compare

        flat = self.np.zeros_like(self.ref)
        r, _ = compare([flat], [flat], 60, "still", [])
        self.assertIsNone(r["summary"]["detail_ratio"])
        self.assertIsNone(r["summary"]["bias_dark_levels"])

    def test_static_shimmer_and_temporal_error_are_different(self):
        from kineval.metrics import compare

        np = self.np
        ref = self.ref.astype(float)
        shifted = np.clip(ref + 10, 0, 255).astype(np.uint8)
        a, _ = compare([self.ref, self.ref], [self.ref, shifted], 60, "static", [])
        b, _ = compare([self.ref, shifted], [self.ref, shifted], 60, "moving", [])
        self.assertGreater(a["summary"]["shimmer_levels"], 0)
        self.assertEqual(b["summary"]["temporal_error_levels"], 0)
        self.assertNotIn("shimmer_levels", b["summary"])

    def test_bistro_numerical_compatibility(self):
        from kineval.metrics import compare
        from kineval.model import ROOT

        # Independent implementation of the original scorer's specified equations.
        np = self.np
        a = self.ref
        b = np.clip(a.astype(float) * 0.9 + 3, 0, 255).astype(np.uint8)
        r, _ = compare([a], [b], 60, "still", [])
        s = r["summary"]
        diff = b.astype(float) - a.astype(float)
        self.assertAlmostEqual(
            s["psnr_db"], 10 * np.log10(255**2 / (diff * diff).mean())
        )
        self.assertAlmostEqual(s["bias_levels"], diff.mean())

    def test_mismatched_frames_and_shapes_rejected(self):
        from kineval.metrics import compare

        with self.assertRaises(EvalError):
            compare([self.ref], [], 60, "still", [])
        with self.assertRaises(EvalError):
            compare([self.ref], [self.ref[:10]], 60, "still", [])


if __name__ == "__main__":
    unittest.main()
