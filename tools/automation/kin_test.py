#!/usr/bin/env python3
"""Run Kin gameplay scenarios and replay recordings through the driven scene server."""
import argparse
from pathlib import Path
import sys
from kintest.runner import (
    Client,
    Runner,
    ConfigurationError,
    ScenarioFailure,
    load_scenario,
    loads,
    replay,
)


def read_recording(path):
    if Path(path).stat().st_size > 256 * 1024 * 1024:
        raise ConfigurationError("recording exceeds 256 MiB")
    recording = [loads(line) for line in Path(path).read_text(encoding="utf-8").splitlines() if line.strip()]
    if not recording or recording[0].get("schema") != "kin.automation-recording/1":
        raise ConfigurationError("unsupported recording schema")
    return recording


def check_replay_identity(client, header, allow_change):
    """What a recording (from kin_test.py or --automation-record) says it ran on."""
    if not allow_change and header.get("command") and client.command != header["command"]:
        raise ConfigurationError("recorded game arguments changed; use --allow-identity-change explicitly")
    if header.get("rng_key") is not None and header["rng_key"] != client.capabilities.get("rng_key"):
        raise ScenarioFailure("recording initial RNG key does not match")
    if header.get("fixed_dt") is not None and header["fixed_dt"] != client.capabilities.get("fixed_dt"):
        raise ScenarioFailure("recording fixed timestep does not match")
    recorded_game = (header.get("game_id", ""), header.get("game_version", ""))
    running_game = (client.game_info.get("id", ""), client.game_info.get("version", ""))
    if recorded_game[0] and not allow_change and recorded_game != running_game:
        raise ConfigurationError("recorded game identity changed; use --allow-identity-change explicitly")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["run", "replay"])
    parser.add_argument("source", help="JSON/Lua scenario or commands.jsonl recording")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--lua-compiler", type=Path)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--dependency", action="append", default=[])
    parser.add_argument("--allow-identity-change", action="store_true")
    parser.add_argument("--no-compare", action="store_true")
    parser.add_argument("--trace-world", action="store_true", help="include registered ECS state in every checkpoint")
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--game", nargs=argparse.REMAINDER, required=True)
    args = parser.parse_args(argv)
    client = None
    try:
        if args.out.exists() and any(args.out.iterdir()):
            raise ConfigurationError("output directory must be empty")
        case, recording, header, dependencies = None, None, {}, list(args.dependency)
        seed = args.seed
        if args.mode == "run":
            case = load_scenario(args.source, args.lua_compiler)
            if seed is None:
                seed = int(case.get("seed", 0))
            dependencies.extend(
                str((Path(args.source).resolve().parent / p).resolve()) for p in case.get("dependencies", [])
            )
        else:
            recording = read_recording(args.source)
            header = recording[0]
            if seed is None:
                if header.get("seed") is None:
                    raise ConfigurationError("recording has no raw seed; supply --seed")
                seed = int(header["seed"])
            dependencies = header.get("dependencies", []) + dependencies
        client = Client(
            args.game,
            args.out,
            seed=seed,
            dependencies=dependencies,
            timeout=args.timeout,
            trace_world=args.trace_world,
            expected_identity=None if args.allow_identity_change else header.get("identity"),
        )
        runner = Runner(client)
        error = None
        try:
            if case:
                runner.run(case)
            else:
                check_replay_identity(client, header, args.allow_identity_change)
                replay(client, recording, compare=not args.no_compare)
            client.call("server.status")
            if client.close() != 0:
                raise ScenarioFailure("game exited unsuccessfully")
            client.verify_identity()
        except Exception as failure:
            error = failure
        result = runner.artifacts(case["name"] if case else args.mode, error)
        print(f'{result["status"]}: {result["name"]}; artifacts: {client.out}')
        return (2 if isinstance(error, ConfigurationError) else 1) if error else 0
    except (ConfigurationError, OSError, ValueError) as error:
        print(f"configuration error: {error}", file=sys.stderr)
        return 2
    except Exception as error:
        print(f"automation failed: {error}", file=sys.stderr)
        return 1
    finally:
        if client:
            client.close()


if __name__ == "__main__":
    raise SystemExit(main())
