"""Exercise safe reload through the real Workshop and recorded RPC revisions."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/automation"))
from kintest.runner import Client, Runner, ScenarioFailure, loads


def watch_launcher(binary, root):
    source = root / "source"
    shutil.copytree(ROOT / "examples/2d_workflow/content", source / "content")
    shutil.copytree(ROOT / "examples/2d_workflow/art", source / "art")
    project = root / "project.json"
    project.write_text(
        json.dumps(
            {
                "schema": "kin.dev/1",
                "build": str(binary.parent.parent),
                "target": "kin_2d_workflow",
                "command": [str(binary)],
                "manifest": str(source / "art/imports.json"),
                "content": str(source / "content"),
                "output": str(root / "imported"),
            }
        )
    )
    client = Client(
        [
            sys.executable,
            str(ROOT / "tools/kin_dev.py"),
            "--project",
            str(project),
            "--no-build",
            "--",
            "--inspect",
            "--seed=7",
        ],
        root / "trace",
        seed=7,
        launch=False,
    )
    runner = Runner(client)
    error = None

    def wait(observe, predicate):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            value = client.call(observe)
            if predicate(value):
                return value
            time.sleep(0.03)
        raise ScenarioFailure("disk watcher condition timed out: " + observe + ": " + str(value))

    try:
        wait("server.capabilities", lambda value: value["mode"] == "inspect" and value["development"])
        wait("scene.current", lambda value: value["state"]["rooms"] == 1)
        identity = client.call("world.query", {"selector": {"authored_id": "player"}})["entities"][0]["id"]
        path = source / "content/game.lua"
        original = path.read_text()
        edited = original.replace("game.counters(state.shots,", "game.counters(state.shots+41,")
        assert edited != original
        path.write_text(edited)
        wait("dev.status", lambda value: value["generation"] == 1)
        wait("scene.current", lambda value: value["state"]["shots"] == 41)
        path.write_text("function broken(")
        rejected = wait("dev.status", lambda value: value["status"] == "rejected")
        assert rejected["generation"] == 1
        assert client.call("scene.current")["state"]["shots"] == 41
        path.write_text(edited + '\nfunction update(ctx) error("gameplay edit error") end\n')
        wait("dev.status", lambda value: value["generation"] == 2)
        wait("scene.current", lambda value: "gameplay edit error" in value["state"]["script_error"])
        path.write_text(edited.replace("state.shots+41", "state.shots+42"))
        wait("dev.status", lambda value: value["generation"] == 3)
        wait("scene.current", lambda value: value["state"]["shots"] == 42)
        assert client.call("world.query", {"selector": {"authored_id": "player"}})["entities"][0]["id"] == identity
        assert client.call("scene.current")["state"]["rooms"] == 1
        assert client.close() == 0
    except Exception as failure:
        error = failure
    finally:
        runner.artifacts("Launcher watched source edits", error)
        client.close()
    if error:
        raise error


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--content", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="session-", dir=args.output))
    command = [str(args.binary.resolve()), "--dev", "--content", str(args.content.resolve())]
    client = Client(command, root / "run", seed=7)
    runner = Runner(client)

    def state():
        return client.call("scene.current")["state"]

    def status():
        return client.call("dev.status")

    def tick(n=1):
        return client.call("sim.tick", {"count": n})

    def wait(path, value, within=16):
        runner.condition(
            {"observe": "scene.current", "path": "/state/" + path, "value": value, "within": within}, "wait"
        )

    def reload(changes, accepted=True, contains=None):
        before = status()
        client.call("dev.reload", {"changes": changes, "expected_revision": before["revision"]})
        tick()
        after = status()
        assert not after["pending"]
        assert after["generation"] == before["generation"] + int(accepted), (before, after)
        assert after["status"] == ("applied" if accepted else "rejected"), after
        if contains:
            assert contains in after["error"], after
        return after

    def move():
        x = state()["player"]["x"]
        client.call("input.key", {"name": "D", "mode": "hold"})
        tick(3)
        dx = state()["player"]["x"] - x
        client.call("input.key", {"name": "D", "mode": "release"})
        tick()
        return dx

    error = None
    try:
        assert client.capabilities["development"]
        wait("rooms", 1)
        baseline = move()
        assert baseline > 0
        player = client.call("world.query", {"selector": {"authored_id": "player"}})["entities"][0]["id"]
        client.call("input.action", {"name": "fire"})
        tick()
        assert state()["shots"] == 1 and state()["active"] == 1
        source = (args.content / "game.lua").read_text()
        edited = source.replace("local MOVE_SCALE = 1", "local MOVE_SCALE = 2")
        assert edited != source
        reload({"game.lua": edited})
        assert state()["shots"] == 1 and state()["active"] == 1
        assert client.call("world.query", {"selector": {"authored_id": "player"}})["entities"][0]["id"] == player
        assert abs(move() - 2 * baseline) < 0.01
        # A candidate cannot touch the world or native game callbacks during
        # migration. Bad edits keep the active VM, assets and scene generation.
        reload(
            {"game.lua": edited + '\nfunction on_reload(s, scene) scene:entity("bad reload entity") end'},
            False,
            "unavailable",
        )
        assert client.call("world.query", {"selector": {"name": "bad reload entity"}})["count"] == 0
        reload(
            {"game.lua": edited + '\nfunction on_reload(s) error("migration rejected") end'},
            False,
            "migration rejected",
        )
        reload({"game.lua": "function invalid("}, False)
        reload({"game.lua": "while true do end"}, False, "budget")
        reload({"images/workshop.png": {"hex": "0001020304"}, "game.lua": source}, False)
        assert abs(move() - 2 * baseline) < 0.01
        # Copy an in-flight preload handle; its native job continues once,
        # instead of restarting loading or rebuilding the input/prefab pools.
        client.call("input.action", {"name": "next_room"})
        tick()
        reload({"game.lua": edited})
        wait("rooms", 2)
        assert state()["commits"] == 2
        room = json.loads((args.content / "rooms/azure.kinscene").read_text())
        room["name"] = "Reloaded Azure"
        reload({"rooms/azure.kinscene": json.dumps(room)})
        assert state()["room_name"] == "Reloaded Azure"
        assert state()["shots"] == 1 and state()["commits"] == 2
        # Repeated activation must keep controller/input ownership, leases and
        # room progress; the released projectile stays released across VMs.
        wait("active", 0, 64)
        for i in range(6):
            reload({"game.lua": edited + "\n-- revision " + str(i)})
        assert state()["recycled"] == 1 and state()["rooms"] == 2
        client.call("ui.action", {"selector": {"key": "fire"}})
        tick()
        wait("shots", 2, 3)
        client.call("view.screenshot", {"path": "reloaded.png"})
        assert client.close() == 0
        client.verify_identity()
    except Exception as failure:
        error = failure
    finally:
        runner.artifacts("Workshop live reload", error)
        client.close()
    if error:
        raise error
    proc = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools/automation/kin_test.py"),
            "replay",
            str(root / "run/commands.jsonl"),
            "--out",
            str(root / "replay"),
            "--game",
            *command,
        ],
        capture_output=True,
        text=True,
        timeout=90,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr + str(root)
    watch_launcher(args.binary.resolve(), root / "watcher")
    print("Stateful Lua, authored room, rollback, in-flight jobs, repeated reload and replay passed. Artifacts:", root)


if __name__ == "__main__":
    main()
