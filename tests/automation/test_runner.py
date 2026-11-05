from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "automation"))
from kintest.runner import (
    Client,
    Runner,
    ConfigurationError,
    ScenarioFailure,
    check_scenario,
    difference,
    loads,
    matches,
    pointer,
    replay,
    MISSING,
)

FAKE = Path(__file__).with_name("fake_game.py")


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def client(self, folder="run", **options):
        client = Client([sys.executable, str(FAKE)], self.root / folder, **options)
        self.addCleanup(client.close)
        return client

    def case(self, steps):
        return {"schema": "kin.scenario/1", "name": "test", "seed": 7, "steps": steps}

    def test_strict_json_and_conditions(self):
        for bad in ['{"a":1,"a":2}', '{"v":NaN}', '{"v":Infinity}']:
            with self.assertRaises((ValueError, ConfigurationError)):
                loads(bad)
        self.assertIs(pointer({"a": [1]}, "/a/2"), MISSING)
        self.assertEqual(pointer({"a/b": {"~key": 7}}, "/a~1b/~0key"), 7)
        self.assertFalse(matches(MISSING, {"value": None}))
        self.assertFalse(matches(True, {"value": 1}))
        self.assertTrue(matches(1.001, {"op": "approx", "value": 1, "tolerance": 0.01}))
        self.assertEqual(difference({"a": 1}, {"a": 2})[0]["path"], "/a")

    def test_reject_bad_plans_before_actions(self):
        client = self.client()
        runner = Runner(client)
        case = self.case([{"call": "input.action", "params": {"name": "fire"}}, {"call": "missing.method"}])
        with self.assertRaises(ConfigurationError):
            runner.run(case)
        self.assertFalse(any(e.get("method") == "input.action" for e in client.events))
        for step in [
            {"wait": {"observe": "scene.current", "within": -1, "value": 0}},
            {"expect": {"observe": "scene.current", "path": "/x~bad", "value": 0}},
            {"capture": "../escape"},
            {"tick": True},
            {"tick": 1, "call": "sim.tick"},
        ]:
            with self.assertRaises(ConfigurationError):
                check_scenario(self.case([step]))

    def test_wait_boundaries_and_no_repeated_action(self):
        client = self.client()
        runner = Runner(client)
        runner.run(
            self.case(
                [
                    {"call": "input.action", "params": {"name": "fire"}},
                    {"wait": {"observe": "scene.current", "path": "/state/count", "value": 1, "within": 1}},
                    {"always": {"observe": "scene.current", "path": "/state/count", "value": 1, "ticks": 3}},
                ]
            )
        )
        self.assertEqual(client.frame, 4)
        commands = [e for e in client.events if e.get("kind") == "command"]
        self.assertEqual(sum(e["method"] == "input.action" for e in commands), 1)
        self.assertEqual(client.call("scene.current")["state"]["wide"], 2**64 - 1)
        runner.artifacts("wait")
        self.assertEqual(json.loads((client.out / "result.json").read_text())["status"], "passed")

    def test_wait_zero_and_exact_timeout(self):
        client = self.client()
        runner = Runner(client)
        runner.condition({"observe": "scene.current", "path": "/state/count", "value": 0, "within": 0}, "wait")
        with self.assertRaises(ScenarioFailure):
            runner.condition({"observe": "scene.current", "path": "/state/count", "value": 2, "within": 2}, "wait")
        self.assertEqual(client.frame, 2)
        with self.assertRaises(ConfigurationError):
            runner.run(self.case([{"wait": {"observe": "sim.tick", "value": 0, "within": 1}}]))

    def test_always_fails_at_first_bad_tick(self):
        client = self.client()
        runner = Runner(client)
        client.call("input.action", {"name": "fire"})
        with self.assertRaises(ScenarioFailure):
            runner.condition({"observe": "scene.current", "path": "/state/count", "value": 0, "ticks": 20}, "always")
        self.assertEqual(client.frame, 1)

    def test_scoped_selector_validation(self):
        client = self.client()
        client.call("ui.action", {"selector": {"key": "fire", "within": {"role": "group"}}})
        with self.assertRaises(ConfigurationError):
            client.call("ui.action", {"selector": {"typo": 1}})
        with self.assertRaises(ConfigurationError):
            client.call("ui.action", {"id": "fire", "selector": {"key": "fire"}})

    def test_failure_bundle_and_trace_escaping(self):
        client = self.client()
        runner = Runner(client)
        case = self.case([{"tick": 1}, {"expect": {"observe": "scene.current", "path": "/state/count", "value": 99}}])
        try:
            runner.run(case)
        except ScenarioFailure as error:
            result = runner.artifacts("</script><script>alert(1)</script>", error)
        self.assertEqual(result["status"], "failed")
        for file in [
            "commands.jsonl",
            "stderr.log",
            "state.json",
            "world.json",
            "ui.json",
            "failure.png",
            "failure.json",
            "junit.xml",
            "trace.html",
        ]:
            self.assertTrue((client.out / file).exists(), file)
        self.assertNotIn("</script><script>alert(1)</script>", (client.out / "trace.html").read_text())
        self.assertTrue(json.loads((client.out / "failure.json").read_text())["state_diff"])

    def test_record_and_replay(self):
        client = self.client("source", seed=7)
        runner = Runner(client)
        runner.run(
            self.case(
                [
                    {"call": "input.action", "params": {"name": "fire"}},
                    {"tick": 1},
                    {"expect": {"observe": "scene.current", "path": "/state/count", "value": 1}},
                ]
            )
        )
        client.close()
        recording = [loads(line) for line in (client.out / "commands.jsonl").read_text().splitlines()]
        target = self.client("target", seed=7)
        replay(target, recording)
        self.assertEqual(target.call("scene.current")["state"]["count"], 1)
        for item in recording:
            if item.get("method") == "scene.current" and item.get("kind") == "command":
                item["result"]["state"]["count"] = 99
                break
        changed = self.client("changed", seed=7)
        with self.assertRaises(ScenarioFailure):
            replay(changed, recording)

    def test_native_frame_checkpoints(self):
        source = self.client("native-source", seed=7)
        result = source.call("sim.tick", {"count": 2})
        snapshot = source.call("automation.snapshot")
        tape = [
            {"kind": "frame", "frame": 1, "snapshot": {}},
            {"kind": "frame", "frame": 2, "snapshot": snapshot},
            {"kind": "command", "method": "sim.tick", "params": {"count": 2}, "result": result},
        ]
        replay(self.client("native-replay", seed=7), tape)
        tape[1]["snapshot"]["scenes"][0]["state"]["count"] = 999
        with self.assertRaisesRegex(ScenarioFailure, "native frame 2"):
            replay(self.client("native-diverged", seed=7), tape)

    def test_process_timeout_and_crash_are_failures(self):
        client = self.client(timeout=0.15)
        start = time.monotonic()
        with self.assertRaises(ScenarioFailure):
            client.call("test.hang")
        runner = Runner(client)
        runner.artifacts("hang", ScenarioFailure("deadline"))
        client.close()
        self.assertLess(time.monotonic() - start, 4)
        crashed = self.client("crash")
        with self.assertRaises(ScenarioFailure):
            crashed.call("test.crash")

    def test_cli_and_failed_exit(self):
        path = self.root / "case.json"
        path.write_text(json.dumps(self.case([{"tick": 1}])))
        command = [
            sys.executable,
            str(ROOT / "tools/automation/kin_test.py"),
            "run",
            str(path),
            "--out",
            str(self.root / "cli"),
            "--game",
            sys.executable,
            str(FAKE),
            "--exit-error",
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(json.loads((self.root / "cli/result.json").read_text())["status"], "failed")
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)

    def test_identity_and_incomplete_recordings(self):
        content = self.root / "content.txt"
        content.write_text("first")
        client = self.client(dependencies=[str(content)])
        content.write_text("changed")
        with self.assertRaises(ScenarioFailure):
            client.verify_identity()
        client.close()
        tape = client.out / "commands.jsonl"
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/automation/kin_test.py"),
                "replay",
                str(tape),
                "--out",
                str(self.root / "identity"),
                "--game",
                sys.executable,
                str(FAKE),
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("identity changed", result.stderr)
        target = self.client("incomplete")
        with self.assertRaisesRegex(ScenarioFailure, "incomplete"):
            replay(target, [{"kind": "request", "sequence": 123, "method": "input.action", "params": {"name": "fire"}}])

    def test_replayed_protocol_errors_and_header(self):
        client = self.client()
        with self.assertRaisesRegex(ScenarioFailure, "reproduced"):
            replay(
                client,
                [{"kind": "command", "ok": False, "method": "absent", "protocol_error": {"message": "unknown method"}}],
            )
        with self.assertRaisesRegex(ScenarioFailure, "changed"):
            replay(client, [{"kind": "command", "ok": False, "method": "absent", "error": "different error"}])
        # One header shape for every writer, whatever the handshake answered.
        header = loads((client.out / "commands.jsonl").read_text().splitlines()[0])
        self.assertEqual((header["kind"], header["game_id"], header["renderer"]), ("header", "fake", "fake"))
        self.assertEqual(header["rng_key"], [0, 0])
        self.assertFalse(any(e.get("kind") == "identity" for e in client.events))

    def test_world_trace_and_xml_controls(self):
        import xml.etree.ElementTree as ET

        client = self.client(trace_world=True)
        runner = Runner(client)
        runner.run(self.case([{"tick": 1}]))
        snapshots = [e for e in client.events if e.get("method") == "automation.snapshot"]
        self.assertTrue(all(e["params"]["world"] for e in snapshots))
        runner.artifacts("control\x01", ScenarioFailure("bad\x00message"))
        ET.parse(client.out / "junit.xml")
        self.assertEqual(
            json.loads((client.out / "failure.json").read_text())["state_diff_scope"], "scene reports, UI and world"
        )


if __name__ == "__main__":
    unittest.main()
