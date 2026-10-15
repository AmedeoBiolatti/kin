"""Input-driven GPU smoke test. Run with a Blockfield binary and an output directory."""
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--quality", choices=["basic", "pbr", "high"], default="basic")
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
world = out / f"world-{time.time_ns()}.bin"
log = (out / "server.log").open("w")
p = subprocess.Popen(
    [str(args.binary.resolve()), "--headless", "--server", "--width", "960", "--height", "540",
     "--distance", "2", "--quality", args.quality, "--world", str(world)],
    cwd=out, env=dict(os.environ, KIN_RENDER_BACKEND="gpu"), stdin=subprocess.PIPE,
    stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1)
replies = queue.Queue()
counter = 0


def read_replies():
    for line in p.stdout:
        try:
            replies.put(json.loads(line))
        except json.JSONDecodeError:
            pass
    replies.put(None)


threading.Thread(target=read_replies, daemon=True).start()


def rpc(method, params=None):
    global counter
    counter += 1
    p.stdin.write(json.dumps({"id": counter, "method": method, "params": params or {}}) + "\n")
    p.stdin.flush()
    deadline = time.monotonic() + 30
    while True:
        try:
            reply = replies.get(timeout=max(0, deadline - time.monotonic()))
        except queue.Empty:
            raise TimeoutError(method) from None
        if reply is None:
            raise RuntimeError(f"Server closed stdout; see {out / 'server.log'}")
        if reply.get("id") != counter:
            continue
        if "error" in reply:
            raise RuntimeError(reply)
        return reply["result"]


def state():
    return rpc("scene.current")["state"]


def tick(n=1):
    return rpc("sim.tick", {"count": n})


def key(name):
    rpc("input.key", {"name": name, "mode": "press"})
    tick(2)


def mouse(button):
    rpc("input.mouse", {"x": 480, "y": 270, "button": button, "mode": "press"})
    tick(3)


try:
    tick(80)
    s = state()
    assert s["resident_chunks"] == 25 and s["collision_ready"], s
    assert s["camera_fov_degrees"] == 90, s
    key("PageUp")
    assert state()["camera_fov_degrees"] == 95, state()
    key("PageDown")
    assert state()["camera_fov_degrees"] == 90, state()
    for _ in range(13):
        key("PageUp")
    assert state()["camera_fov_degrees"] == 150, state()
    for _ in range(12):
        key("PageDown")
    assert state()["camera_fov_degrees"] == 90, state()
    # Exercise the actual character controller, then return to the known mining spot.
    height = s["camera"][1]
    key("Space")
    tick(10)
    assert state()["camera"][1] > height + 0.2, state()
    tick(90)
    assert abs(state()["camera"][1] - height) < 0.1, state()
    key("R")
    key("F")
    assert state()["fly"]
    rpc("input.mouse", {"dx": 0, "dy": 850})
    tick(2)
    s = state()
    assert "target" in s, s
    before = s["blocks_changed"]
    mouse("left")
    s = state()
    assert s["blocks_changed"] == before + 1 and s["edits"] >= 1, s
    rpc("view.screenshot", {"path": "mined.png"})
    mouse("right")
    assert state()["blocks_changed"] == before + 2, state()
    key("3")
    assert state()["selected_block"] == "Stone"
    before = state()["blocks_changed"]
    mouse("right")
    assert state()["blocks_changed"] == before, state()
    mouse("left")
    tick(12)
    key("F5")
    saved = state()
    assert world.exists() and saved["edits"] >= 1, saved
    rpc("sim.reset")
    tick(30)
    assert state()["world_hash"] == saved["world_hash"], state()
    key("F")
    rpc("input.key", {"name": "W", "mode": "hold"})
    tick(300)
    rpc("input.key", {"name": "W", "mode": "release"})
    tick(20)
    s = state()
    assert s["chunks_evicted"] > 0 and s["resident_chunks"] <= 25, s
    assert s["resident_mesh_bytes"] <= s["mesh_budget_bytes"], s
    rpc("view.screenshot", {"path": "streamed.png"})
    key("Equals")
    tick(60)
    assert state()["view_distance_chunks"] == 3 and state()["resident_chunks"] == 49, state()
    key("Minus")
    tick(3)
    assert state()["view_distance_chunks"] == 2 and state()["resident_chunks"] <= 25, state()
    s = state()
    if args.quality != "basic":
        rpc("view.screenshot", {"path": "quality.png"})
        s = state()
        assert s["render_quality"] == args.quality and s["lights_used"] > 0 and s["spots_used"] > 0, s
        assert s["samples_used"] == (4 if args.quality == "pbr" else 1), s
        if args.quality == "high":
            assert all(s[k] > 0 for k in ["local_shadow_maps", "taa_dispatches", "ao_dispatches", "contact_dispatches"]), s
    (out / "result.json").write_text(json.dumps(s, indent=2))
    print(f"Blockfield {args.quality} smoke passed: FOV/distance controls, jump, mining, placement, overlap guard, selection, save/reload, streaming and requested rendering features")
finally:
    if p.poll() is None:
        try:
            rpc("server.shutdown")
            p.wait(timeout=10)
        except Exception:
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
    log.close()
