#!/usr/bin/env python3
"""Scene-independent benchmark, capture, reference and scoring jobs.

Run --help for commands. Only optional scoring providers need NumPy/Pillow,
FLIP, ColorVideoVDP or OpenEXR; the orchestration uses the standard library.
"""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kineval.common import EvalError, Unsupported, read_json, write_json


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if argv and argv[0] == "_score":
        from kineval.runner import score_job

        job = read_json(argv[1])
        out = Path(argv[2])
        try:
            result = score_job(job, out)
        except Unsupported as e:
            result = {
                "schema": "kin.eval-result/1",
                "status": "unsupported",
                "reason": str(e),
            }
        write_json(out / "result.json", result)
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("validate", "run", "bench", "capture", "truth", "score", "stress"):
        p = sub.add_parser(name)
        p.add_argument("suite", type=Path)
        p.add_argument("--profile", default="smoke")
        p.add_argument("--scene", action="append")
        p.add_argument("--baseline", type=Path)
        p.add_argument("--candidate", type=Path)
        p.add_argument("--blender", type=Path)
        p.add_argument("--metrics-python", type=Path)
        p.add_argument("--out", type=Path)
        p.add_argument("--resume", action="store_true")
        p.add_argument(
            "--allow-unavailable",
            action="store_true",
            help="retain incomplete status, but permit exit zero for unavailable providers",
        )
        p.add_argument("--dry-run", action="store_true")
    p = sub.add_parser("report")
    p.add_argument("run", type=Path)
    args = parser.parse_args(argv)
    if args.command == "report":
        from kineval.report import generate

        print(generate(args.run))
        return 0
    from kineval.model import load_suite
    from kineval.runner import Runner, provenance

    plan = load_suite(
        args.suite, args.profile, args.scene, args.baseline, args.candidate
    )
    if args.blender:
        plan["tools"]["blender"] = str(args.blender.absolute())
    if args.metrics_python:
        plan["tools"]["metrics_python"] = str(args.metrics_python.absolute())
    if args.command == "validate" or args.dry_run:
        info = provenance(plan)
        print(json.dumps({"plan": plan, "provenance": info}, indent=2))
        return (
            3
            if (info["unavailable"] or info["build_unavailable"])
            and not args.allow_unavailable
            else 0
        )
    if not args.out:
        parser.error("--out is required")
    from kineval.common import gpu_lock

    with gpu_lock(args.out / ".run.lock"):
        runner = Runner(plan, args.out, args.resume, args.allow_unavailable)
        run = runner.execute(None if args.command == "run" else [args.command])
        from kineval.report import generate

        print(generate(args.out))
    return (
        1
        if run["status"] == "failed"
        else 3 if run["status"] == "incomplete" and not args.allow_unavailable else 0
    )


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (EvalError, OSError, ValueError, KeyError) as error:
        print(f"eval: {error}", file=sys.stderr)
        raise SystemExit(2)
