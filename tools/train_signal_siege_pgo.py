#!/usr/bin/env python3
"""Exercise real Signal Siege paths for compiler PGO (seeds 17 and 29)."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gpu", action="store_true", help="Train gameplay/grid on the hidden native GPU instead of SDL software; requires X11/Vulkan")
    args = parser.parse_args()
    binary = args.game.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    runs = []
    with tempfile.TemporaryDirectory(prefix="kin-pgo-training-") as temp:
        base_env = dict(os.environ, SDL_AUDIO_DRIVER="dummy", KIN_LOG_FORMAT="text",
                        XDG_DATA_HOME=temp, XDG_CONFIG_HOME=temp)

        def run(label, arguments, renderer="sdl", stdin=None):
            environment = dict(base_env, SDL_VIDEO_DRIVER="x11" if renderer == "gpu" else "dummy",
                               KIN_RENDER_BACKEND=renderer)
            result = subprocess.run([str(binary), *arguments], cwd=temp, env=environment,
                                    input=stdin, capture_output=True, text=True, timeout=600)
            (output / f"{label}.stdout").write_text(result.stdout)
            (output / f"{label}.stderr").write_text(result.stderr)
            result.check_returncode()
            if "profiling error" in result.stderr.lower() or "LLVM Profile Error" in result.stderr:
                raise RuntimeError(f"Compiler profile write failed: {label}")
            backend = "SDL_GPU" if renderer == "gpu" else "software"
            if f"renderer created backend={backend}" not in result.stderr:
                raise RuntimeError(f"Required renderer not selected: {label}")
            runs.append({"label": label, "renderer": backend, "arguments": arguments})
            print(f"Trained {label}: {backend}", flush=True)
            return result

        # Driven server starts on the real title, without user save writes.
        # Select Watch, open/close the upgrade grid, then return to the title.
        requests = [
            ("sim.tick", {"count": 90}), ("scene.current", {}),
            ("input.key", {"name": "Down"}), ("sim.tick", {"count": 1}),
            ("input.key", {"name": "Enter"}), ("sim.tick", {"count": 120}),
            ("scene.current", {}), ("input.action", {"name": "power_grid"}),
            ("sim.tick", {"count": 60}), ("input.action", {"name": "quit"}),
            ("sim.tick", {"count": 1}), ("input.action", {"name": "quit"}),
            ("sim.tick", {"count": 120}), ("scene.current", {}), ("server.shutdown", {})]
        payload = "".join(json.dumps({"id": index, "method": method, "params": params}) + "\n"
                          for index, (method, params) in enumerate(requests))
        text = run("title-transitions", ["--server", "--render", "--seed=17", "--locale=en"], stdin=payload).stdout
        responses = [json.loads(line) for line in text.splitlines() if line.strip()]
        if len(responses) != len(requests) or any("error" in row for row in responses):
            raise RuntimeError("Title/transition training server rejected commands")
        states = [row["result"]["state"] for row in responses if row["id"] in (1, 6, 13)]
        if states[0].get("screen") != "title" or "enemies" not in states[1] or states[2].get("screen") != "title":
            raise RuntimeError(f"Title -> gameplay -> title flow did not complete: {states}")
        # A normal finite windowed run also exercises the audio device path,
        # using SDL's dummy output and a temporary user-data directory.
        audio = run("title-audio", ["--frames=90", "--seed=29", "--locale=en"])
        if "mode=windowed" not in audio.stderr or "sdl audio device opened" not in audio.stderr:
            raise RuntimeError("Windowed title/audio training did not complete")
        for renderer in (["gpu"] if args.gpu else ["sdl"]):
            for seed in (17, 29):
                cases = [("arena", 360, []), ("stress", 240, ["--stress"])]
                cases += [("grid-" + locale, 90, ["--power-grid", "--locale=" + locale])
                          for locale in ("en", "fr", "ja", "ar")]
                for name, frames, options in cases:
                    label = f"{renderer}-{seed}-{name}"
                    report = json.loads(run(label, ["--headless", "--render", f"--frames={frames}",
                        f"--seed={seed}", "--report=-", "--fail-on-missing-text", *options], renderer).stdout)
                    if report["status"] != "ok" or report["localization"]["missing_text"]:
                        raise RuntimeError(f"Training scenario failed: {label}")
    (output / "training.json").write_text(json.dumps({"schema": "kin.signal-siege-pgo-training/1",
        "runs": runs, "seeds": [17, 29], "gpu": args.gpu,
        "note": "Title/Watch/gameplay/grid/title transitions, dummy-device audio, default/stress arenas and four locales. Real audio drivers/hardware and player input distributions still require production training."}, indent=2) + "\n")


if __name__ == "__main__":
    main()
