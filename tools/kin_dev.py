#!/usr/bin/env python3
"""Build, watch imports and run a Kin game with safe live reload enabled."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def project(path):
    path = Path(path).resolve()
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("schema") != "kin.dev/1":
        raise ValueError("unsupported development project schema")
    for key in ["build", "target", "command", "manifest", "content", "output"]:
        if key not in value:
            raise ValueError(f"development project is missing {key}")
    if (
        not isinstance(value["command"], list)
        or not value["command"]
        or any(not isinstance(x, str) for x in value["command"])
    ):
        raise ValueError("development command must be a nonempty argv array")
    for key in ["build", "manifest", "content", "output"]:
        value[key] = str((path.parent / value[key]).resolve())
    if Path(value["output"]).is_relative_to(Path(value["content"])):
        raise ValueError("development output must be outside source content")
    return value


def stop(process):
    if process is None:
        return
    if os.name != "nt":
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    elif process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        if os.name != "nt":
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        else:
            process.kill()
        process.wait(timeout=2)


def run(value, *, build=True, game_args=()):
    watcher = game = None
    try:
        if build:
            command = ["cmake", "--build", value["build"], "--target", value["target"]]
            if value.get("config"):
                command += ["--config", value["config"]]
            subprocess.run(command, check=True)
        importer = [
            sys.executable,
            str(Path(__file__).with_name("import_2d_assets.py")),
            value["manifest"],
            "--content",
            value["content"],
            "--output",
            value["output"],
        ]
        # A usable complete initial import is required before starting the game.
        subprocess.run(importer, check=True, stdout=sys.stderr)
        watcher = subprocess.Popen([*importer, "--watch"], stdout=sys.stderr, start_new_session=os.name != "nt")
        command = [*value["command"], "--dev", "--content", value["output"], *game_args]
        print(f'kin dev: watching {value["content"]}; running {value["target"]}', file=sys.stderr, flush=True)
        game = subprocess.Popen(command, start_new_session=os.name != "nt")
        while game.poll() is None:
            if watcher.poll() is not None:
                raise RuntimeError(f"asset watcher exited ({watcher.returncode}); development session stopped")
            time.sleep(0.05)
        return game.returncode
    finally:
        stop(game)
        stop(watcher)


def main(argv=None):
    def interrupt(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupt)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("game_args", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    try:
        extra = args.game_args[1:] if args.game_args[:1] == ["--"] else args.game_args
        return run(project(args.project), build=not args.no_build, game_args=extra)
    except KeyboardInterrupt:
        return 130
    except (ValueError, OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"kin dev: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
