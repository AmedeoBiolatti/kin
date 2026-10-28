"""Versioned evaluation artifacts and process execution; no renderer dependencies."""

from __future__ import annotations
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time


class EvalError(Exception):
    pass


class Unsupported(EvalError):
    pass


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise EvalError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    with Path(path).open(encoding="utf-8") as src:
        return json.load(
            src,
            object_pairs_hook=unique_object,
            parse_constant=lambda s: (_ for _ in ()).throw(
                EvalError(f"non-finite JSON: {s}")
            ),
        )


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".partial")
    tmp.write_text(
        json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8"
    )
    tmp.replace(path)


def canonical(value):
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode()


def fingerprint(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as src:
        for block in iter(lambda: src.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def number(value, name, minimum=None, maximum=None, integer=False):
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(value)
    ):
        raise EvalError(f"{name}: finite number required")
    if integer and int(value) != value:
        raise EvalError(f"{name}: integer required")
    if (
        minimum is not None
        and value < minimum
        or maximum is not None
        and value > maximum
    ):
        raise EvalError(f"{name}: outside supported range")
    return int(value) if integer else value


def identifier(value):
    import re

    if (
        not isinstance(value, str)
        or not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_.-]{0,95}", value)
        or value in (".", "..")
    ):
        raise EvalError(f"invalid identifier: {value!r}")
    return value


def executable_path(value, base):
    # Interpreter symlinks must retain their virtual-environment directory.
    value = os.path.expandvars(os.path.expanduser(str(value)))
    if "$" in value:
        raise EvalError(f"unresolved environment variable: {value}")
    p = Path(value)
    return Path(os.path.abspath(p if p.is_absolute() else Path(base) / p))


def resolve(value, base):
    value = os.path.expandvars(os.path.expanduser(str(value)))
    if "$" in value:
        raise EvalError(f"unresolved environment variable: {value}")
    p = Path(value)
    return (p if p.is_absolute() else Path(base) / p).resolve()


def within(root, relative):
    p = (Path(root) / relative).resolve()
    if not p.is_relative_to(Path(root).resolve()) or p == Path(root).resolve():
        raise EvalError(f"artifact escapes its job: {relative}")
    return p


def rows(path):
    result = []
    with Path(path).open(encoding="utf-8") as src:
        for line in src:
            if line.strip():
                result.append(
                    json.loads(
                        line,
                        object_pairs_hook=unique_object,
                        parse_constant=lambda s: (_ for _ in ()).throw(
                            EvalError(f"non-finite row: {s}")
                        ),
                    )
                )
    return result


def write_rows(path, records):
    with Path(path).open("w", encoding="utf-8") as out:
        for row in records:
            out.write(json.dumps(row, allow_nan=False) + "\n")


def execute(argv, folder, *, cwd=None, env=None, timeout=1800):
    """No shell; kill the entire child process group on timeout/interruption."""
    if not argv or not all(isinstance(a, str) for a in argv):
        raise EvalError("command must be a nonempty argv array")
    folder = Path(folder)
    write_json(
        folder / "command.json",
        {"argv": argv, "cwd": str(cwd) if cwd else None, "environment": env or {}},
    )
    start = time.monotonic()
    with (folder / "process.log").open("ab") as log:
        p = subprocess.Popen(
            argv,
            cwd=cwd,
            env=dict(os.environ, **(env or {})),
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=os.name != "nt",
        )
        try:
            deadline = start + timeout
            next_sample = 0.0
            with (folder / "resources.jsonl").open("w", encoding="utf-8") as resources:
                while True:
                    now = time.monotonic()
                    if now > deadline:
                        raise subprocess.TimeoutExpired(argv, timeout)
                    if now >= next_sample:
                        sample = {
                            "schema": "kin.eval-resources/1",
                            "elapsed_s": now - start,
                            "pid": p.pid,
                        }
                        status = Path(f"/proc/{p.pid}/status")
                        if status.is_file():
                            fields = dict(
                                line.split(":", 1)
                                for line in status.read_text().splitlines()
                                if ":" in line
                            )
                            for key in ("VmRSS", "VmHWM"):
                                if key in fields:
                                    sample[key + "_bytes"] = (
                                        int(fields[key].split()[0]) * 1024
                                    )
                        if __import__("shutil").which("nvidia-smi"):
                            try:
                                observation = subprocess.run(
                                    [
                                        "nvidia-smi",
                                        "--query-gpu=index,memory.used,temperature.gpu,power.draw,clocks.sm",
                                        "--format=csv,noheader,nounits",
                                    ],
                                    capture_output=True,
                                    text=True,
                                    timeout=2,
                                )
                                sample["device_total_csv"] = observation.stdout.strip()
                                sample["device_total_columns"] = [
                                    "index",
                                    "memory_used_MiB",
                                    "temperature_C",
                                    "power_W",
                                    "clock_MHz",
                                ]
                            except (OSError, subprocess.TimeoutExpired):
                                pass
                        resources.write(json.dumps(sample, allow_nan=False) + "\n")
                        next_sample = now + 1.0
                    try:
                        code = p.wait(
                            timeout=min(0.1, max(0.001, deadline - time.monotonic()))
                        )
                        break
                    except subprocess.TimeoutExpired:
                        pass
        except BaseException:
            if os.name != "nt":
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(p.pid, signal.SIGTERM)
            else:
                p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                if os.name != "nt":
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(p.pid, signal.SIGKILL)
                else:
                    p.kill()
                p.wait()
            raise
    if code:
        raise EvalError(f"command exited {code}: {folder / 'process.log'}")
    return time.monotonic() - start


@contextlib.contextmanager
def gpu_lock(path=None):
    """Cross-run advisory lock; process exit releases it, including after interruption."""
    path = (
        Path(path)
        if path is not None
        else Path(tempfile.gettempdir()) / "kin-eval-gpu.lock"
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as f:
        if os.name == "nt":
            import msvcrt

            f.seek(0)
            f.write(b"0")
            f.flush()
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl

            fcntl.flock(f, fcntl.LOCK_EX)
        try:
            yield
        finally:
            if os.name == "nt":
                f.seek(0)
                msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(f, fcntl.LOCK_UN)
