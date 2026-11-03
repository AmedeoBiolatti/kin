"""Exercise the real Workshop, compiler, scenario runner and both replay writers."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "tools/automation/kin_test.py"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--content", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="session-", dir=args.output))
    game = [str(args.binary.resolve()), "--content", str(args.content.resolve())]

    def cli(mode, source, folder, expected=0, extra=()):
        proc = subprocess.run(
            [sys.executable, str(CLI), mode, str(source), "--out", str(out / folder), *extra, "--game", *game],
            capture_output=True,
            text=True,
            timeout=45,
        )
        if proc.returncode != expected:
            raise AssertionError(f"{folder}: {proc.returncode}\n{proc.stdout}\n{proc.stderr}\nArtifacts: {out}")
        return out / folder

    cases = ROOT / "examples/2d_workflow/tests"
    original = cli("run", cases / "workshop.json", "json", extra=["--trace-world"])
    cli("replay", original / "commands.jsonl", "replay")
    cli("run", cases / "workshop.lua", "lua", extra=["--lua-compiler", str(args.compiler.resolve())])
    negative = json.loads((cases / "workshop.json").read_text())
    negative["dependencies"] = [str((cases / p).resolve()) for p in negative["dependencies"]]
    negative["steps"] = negative["steps"][:4] + [
        {"wait": {"observe": "scene.current", "path": "/state/rooms", "value": 999, "within": 2}}
    ]
    negative_file = out / "failure-case.json"
    negative_file.write_text(json.dumps(negative))
    failed = cli("run", negative_file, "failure", expected=1)
    for name in ["failure.json", "failure.png", "state.json", "ui.json", "world.json", "trace.html", "junit.xml"]:
        assert (failed / name).exists(), name
    failure = json.loads((failed / "failure.json").read_text())
    assert failure["assertion"]["ticks"] == 2
    assert failure["state_diff"]
    # Native session recording captures the same RPCs, without the Python client.
    native = out / "native"
    native.mkdir()
    tape = native / "session.jsonl"
    requests = [
        {"id": i, "method": method, "params": params}
        for i, (method, params) in enumerate(
            [
                ("server.schema", {}),
                ("server.capabilities", {}),
                ("sim.tick", {"count": 4}),
                ("input.action", {"name": "fire"}),
                ("sim.tick", {"count": 3}),
                ("scene.current", {}),
                ("server.shutdown", {}),
            ]
        )
    ]
    proc = subprocess.run(
        [*game, "--server", "--seed=7", "--automation-record=" + str(tape)],
        cwd=native,
        input="".join(json.dumps(r) + "\n" for r in requests),
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    records = [json.loads(line) for line in tape.read_text().splitlines()]
    assert (
        records[0]["seed"] == "7"
        and any(r["kind"] == "request" for r in records)
        and any(r["kind"] == "frame" for r in records)
    )
    cli("replay", tape, "native-replay")
    # Interactive RPC recording is also a usable manual reproduction surface.
    manual = out / "manual"
    proc = subprocess.run(
        [sys.executable, str(CLI), "record", "--out", str(manual), "--seed", "7", "--game", *game],
        input="".join(json.dumps(r) + "\n" for r in requests),
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    cli("replay", manual / "commands.jsonl", "manual-replay")
    # The compiler runs a bounded, file-free Lua sandbox, rejects values that
    # cannot become JSON, and fails loops instead of hanging the runner.
    for i, code in enumerate(
        [
            'test{name="bad",steps={function()end}}',
            "while true do end",
            'test{name="bad",steps={[1]={tick=1},[3]={tick=1}}}',
            'test{name=os.execute("echo bad")}',
        ]
    ):
        path = out / f"bad-{i}.lua"
        path.write_text(code)
        proc = subprocess.run([str(args.compiler), str(path)], capture_output=True, text=True, timeout=10)
        assert proc.returncode != 0, code
    print(
        f"Gameplay scenarios, Lua, both recordings/replays, failure artifacts and compiler checks passed. Artifacts: {out}"
    )


if __name__ == "__main__":
    main()
