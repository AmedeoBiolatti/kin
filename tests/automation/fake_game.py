"""A deliberately small external process for testing the driver, not Kin itself."""

import base64
import json
from pathlib import Path
import sys
import time

frame = count = 0
pending = False
seed = next((int(a.split("=", 1)[1]) for a in sys.argv if a.startswith("--seed=")), 0)
exit_code = 3 if "--exit-error" in sys.argv else 0
schema = {
    "schema": "kin.automation/1",
    "version": 1,
    "methods": {},
    "ui_selector": {
        "type": "object",
        "properties": {"key": {"type": "string"}, "role": {"type": "string"}, "within": {"$ref": "#/ui_selector"}},
        "additionalProperties": False,
    },
}
for method, effect in [
    ("server.schema", "read"),
    ("server.capabilities", "read"),
    ("game.info", "read"),
    ("server.status", "read"),
    ("scene.current", "read"),
    ("automation.snapshot", "read"),
    ("ui.snapshot", "layout"),
    ("world.snapshot", "read"),
    ("ui.query", "layout"),
    ("world.query", "read"),
    ("ui.action", "input"),
    ("input.action", "input"),
    ("sim.tick", "step"),
    ("sim.reset", "reset"),
    ("view.screenshot", "capture"),
    ("server.shutdown", "shutdown"),
    ("test.hang", "step"),
    ("test.crash", "step"),
]:
    schema["methods"][method] = {"effect": effect, "params": {"type": "object"}}
schema["methods"]["sim.tick"]["params"] = {
    "type": "object",
    "properties": {"count": {"type": "integer", "minimum": 0, "maximum": 1000000}},
    "additionalProperties": False,
}
PNG = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jBqkAAAAASUVORK5CYII=")
for line in sys.stdin:
    request = json.loads(line)
    method = request["method"]
    params = request.get("params", {})
    result, error = {}, None
    if method == "server.schema":
        result = schema
    elif method == "server.capabilities":
        result = {
            "ui": True,
            "world": True,
            "reset": True,
            "image_io": True,
            "rng_key": [seed, 0],
            "fixed_dt": 1 / 60,
            "renderer": "fake",
        }
    elif method == "game.info":
        result = {"id": "fake", "version": "1"}
    elif method == "server.status":
        result = {"frame": frame, "failed": False}
    elif method == "scene.current":
        result = {"name": "Counter", "state": {"count": count, "wide": 2**64 - 1}}
    elif method == "automation.snapshot":
        result = {"frame": frame, "scenes": [{"name": "Counter", "state": {"count": count}}]}
    elif method in ["ui.snapshot", "ui.query"]:
        result = {"count": 1, "elements": [{"id": "18446744073709551615", "key": "fire", "role": "button"}]}
    elif method in ["world.snapshot", "world.query"]:
        result = {"count": 1, "entities": [{"id": "18446744073709551615", "authored_id": "hero"}]}
    elif method in ["ui.action", "input.action"]:
        pending = True
        result = {"queued": True}
    elif method == "sim.tick":
        for i in range(params.get("count", 1)):
            frame += 1
            if pending:
                count += 1
                pending = False
        result = {"frame": frame, "failed": False}
    elif method == "sim.reset":
        frame = count = 0
        pending = False
        result = {"frame": frame}
    elif method == "view.screenshot":
        path = params.get("path", "capture.png")
        Path(path).write_bytes(PNG)
        result = {"path": str(Path(path).resolve()), "width": 1, "height": 1}
    elif method == "test.hang":
        time.sleep(60)
    elif method == "test.crash":
        sys.exit(7)
    elif method == "server.shutdown":
        print(json.dumps({"id": request["id"], "result": {"bye": True}}), flush=True)
        sys.exit(exit_code)
    else:
        error = {"message": "unknown method"}
    print(
        json.dumps({"id": request["id"], "error": error} if error else {"id": request["id"], "result": result}),
        flush=True,
    )
