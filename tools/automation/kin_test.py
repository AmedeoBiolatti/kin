#!/usr/bin/env python3
"""Run/replay/record Kin gameplay scenarios through the driven scene server."""
import argparse
import json
import math
from pathlib import Path
import sys
from kintest.runner import (
    Client,
    Runner,
    ConfigurationError,
    ScenarioFailure,
    dump,
    identities,
    load_scenario,
    loads,
    replay,
)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["run", "replay", "record"])
    parser.add_argument("source", nargs="?", help="JSON/Lua scenario or commands.jsonl recording")
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
        if not args.game:
            raise ConfigurationError("--game requires an executable")
        if not math.isfinite(args.timeout) or args.timeout <= 0 or args.timeout > 300:
            raise ConfigurationError("timeout must be finite and in 0..300 seconds")
        if args.out.exists() and any(args.out.iterdir()):
            raise ConfigurationError("output directory must be empty")
        case, recording, dependencies = None, None, list(args.dependency)
        seed = args.seed if args.seed is not None else 0
        if args.mode == "run":
            if not args.source:
                raise ConfigurationError("run requires a scenario")
            case = load_scenario(args.source, args.lua_compiler)
            if args.seed is None:
                seed = int(case.get("seed", 0))
            dependencies.extend(
                str((Path(args.source).resolve().parent / p).resolve()) for p in case.get("dependencies", [])
            )
        elif args.mode == "replay":
            if not args.source:
                raise ConfigurationError("replay requires a recording")
            if Path(args.source).stat().st_size > 256 * 1024 * 1024:
                raise ConfigurationError("recording exceeds 256 MiB")
            recording = [
                loads(line) for line in Path(args.source).read_text(encoding="utf-8").splitlines() if line.strip()
            ]
            if not recording or recording[0].get("schema") != "kin.automation-recording/1":
                raise ConfigurationError("unsupported recording schema")
            header = recording[0]
            if args.seed is None:
                if header.get("seed") is None:
                    raise ConfigurationError("recording has no raw seed; supply --seed")
                seed = int(header["seed"])
            dependencies = header.get("dependencies", []) + dependencies
            if header.get("identity") and not args.allow_identity_change:
                observed = identities(args.game, dependencies)
                if observed != header["identity"]:
                    raise ConfigurationError(
                        "recording binary/content identity changed; use --allow-identity-change explicitly"
                    )
        if not 0 <= seed <= 2**64 - 1:
            raise ConfigurationError("invalid seed")
        client = Client(
            args.game,
            args.out,
            seed=seed,
            dependencies=dependencies,
            timeout=args.timeout,
            trace_world=args.trace_world,
        )
        if (
            recording
            and recording[0].get("command")
            and not args.allow_identity_change
            and client.command != recording[0]["command"]
        ):
            raise ConfigurationError("recorded game arguments changed; use --allow-identity-change explicitly")
        runner = Runner(client)
        error = None
        shutdown_requested = False
        try:
            if case:
                runner.run(case)
            elif recording:
                expected = next((r["capabilities"] for r in recording if r.get("kind") == "identity"), None)
                header = recording[0]
                key = expected.get("rng_key") if expected else header.get("rng_key")
                if key is not None and key != client.capabilities.get("rng_key"):
                    raise ScenarioFailure("recording initial RNG key does not match")
                fixed_dt = expected.get("fixed_dt") if expected else header.get("fixed_dt")
                if fixed_dt is not None and fixed_dt != client.capabilities.get("fixed_dt"):
                    raise ScenarioFailure("recording fixed timestep does not match")
                if header.get("game_id") and not args.allow_identity_change:
                    info = client.call("game.info")
                    if info.get("id") != header["game_id"] or info.get("version") != header.get("game_version"):
                        raise ConfigurationError(
                            "recorded game identity changed; use --allow-identity-change explicitly"
                        )
                replay(client, recording, compare=not args.no_compare)
            else:
                for line in sys.stdin:
                    request = loads(line)
                    try:
                        response = {
                            "id": request.get("id"),
                            "result": client.call(request["method"], request.get("params", {}), strict=False),
                        }
                    except ScenarioFailure as failure:
                        response = {"id": request.get("id"), "error": {"message": str(failure)}}
                    print(json.dumps(response), flush=True)
                    if request["method"] == "server.shutdown":
                        shutdown_requested = True
                        break
            if not shutdown_requested:
                client.call("server.status")
            if client.close() != 0:
                raise ScenarioFailure("game exited unsuccessfully")
            client.verify_identity()
        except Exception as failure:
            error = failure
        result = runner.artifacts(case["name"] if case else args.mode, error)
        if args.mode != "record":
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
