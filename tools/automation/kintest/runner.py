"""Tick-driven gameplay tests. Process deadlines are separate from game time."""

from collections import deque
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET

from .process import end_process_group, stop_process_group


class ScenarioFailure(RuntimeError):
    pass


class ConfigurationError(ValueError):
    pass


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ConfigurationError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def loads(text):
    return json.loads(
        text,
        object_pairs_hook=_pairs,
        parse_constant=lambda value: (_ for _ in ()).throw(ConfigurationError(f"non-finite JSON: {value}")),
    )


def dump(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=True, allow_nan=False) + "\n", encoding="utf-8")


def validate(value, schema, path="$"):
    """The JSON-schema subset advertised by server.schema (no external refs)."""
    if "anyOf" in schema:
        for alternative in schema["anyOf"]:
            try:
                validate(value, alternative, path)
                return
            except ConfigurationError:
                pass
        raise ConfigurationError(f"{path}: no permitted schema matches")
    kind = schema.get("type")
    checks = {
        "object": lambda v: isinstance(v, dict),
        "array": lambda v: isinstance(v, list),
        "string": lambda v: isinstance(v, str),
        "boolean": lambda v: isinstance(v, bool),
        "integer": lambda v: type(v) in (int, float) and math.isfinite(v) and v == math.floor(v),
        "number": lambda v: type(v) in (int, float) and math.isfinite(v),
    }
    if kind and (kind not in checks or not checks[kind](value)):
        raise ConfigurationError(f"{path}: expected {kind}")
    if "enum" in schema and value not in schema["enum"]:
        raise ConfigurationError(f'{path}: expected one of {schema["enum"]}')
    if type(value) in (int, float):
        if not math.isfinite(value):
            raise ConfigurationError(f"{path}: non-finite number")
        for bound, test in [
            ("minimum", lambda a, b: a < b),
            ("maximum", lambda a, b: a > b),
            ("exclusiveMinimum", lambda a, b: a <= b),
        ]:
            if bound in schema and test(value, schema[bound]):
                raise ConfigurationError(f"{path}: violates {bound}={schema[bound]}")
    if isinstance(value, dict):
        for required in schema.get("required", []):
            if required not in value:
                raise ConfigurationError(f"{path}: missing {required}")
        for key, item in value.items():
            fields = schema.get("properties", {})
            if key in fields:
                validate(item, fields[key], f"{path}.{key}")
            elif schema.get("additionalProperties") is False:
                raise ConfigurationError(f"{path}: unknown field {key}")
    if isinstance(value, list) and "items" in schema:
        for i, item in enumerate(value):
            validate(item, schema["items"], f"{path}[{i}]")


def load_scenario(path, lua_compiler=None):
    path = Path(path).resolve()
    if path.suffix == ".lua":
        if not lua_compiler:
            raise ConfigurationError("Lua scenarios require --lua-compiler /path/to/kin_lua")
        proc = subprocess.run(
            [str(lua_compiler), "scenario", str(path)], capture_output=True, text=True, timeout=15
        )
        if proc.returncode:
            raise ConfigurationError(proc.stderr.strip() or "Lua scenario compilation failed")
        case = loads(proc.stdout)
    else:
        if path.stat().st_size > 4 * 1024 * 1024:
            raise ConfigurationError("scenario exceeds 4 MiB")
        case = loads(path.read_text(encoding="utf-8"))
    check_scenario(case)
    return case


OPERATORS = ["eq", "ne", "lt", "le", "gt", "ge", "approx", "exists", "absent", "contains"]
_BOUND = {"type": "integer", "minimum": 0, "maximum": 1000000}
_CONDITION = {
    "type": "object",
    "required": ["observe"],
    "properties": {
        "observe": {"type": "string"},
        "params": {"type": "object"},
        "path": {"type": "string"},
        "op": {"type": "string", "enum": OPERATORS},
        "value": {},
        "tolerance": {"type": "number", "minimum": 0},
        "within": _BOUND,
        "ticks": _BOUND,
    },
    "additionalProperties": False,
}
_STEP = {
    "type": "object",
    "properties": {
        "name": {"type": "string"},
        "call": {"type": "string"},
        "params": {"type": "object"},
        "tick": _BOUND,
        "expect": _CONDITION,
        "wait": _CONDITION,
        "always": _CONDITION,
        "capture": {"type": "string"},
    },
    "additionalProperties": False,
}
SCENARIO_SCHEMA = {
    "type": "object",
    "required": ["schema", "name", "steps"],
    "properties": {
        "schema": {"type": "string", "enum": ["kin.scenario/1"]},
        "name": {"type": "string"},
        "seed": {"anyOf": [{"type": "integer", "minimum": 0, "maximum": 2**64 - 1}, {"type": "string"}]},
        "requires": {"type": "array", "items": {"type": "string"}},
        "dependencies": {"type": "array", "items": {"type": "string"}},
        "steps": {"type": "array", "items": _STEP},
    },
    "additionalProperties": False,
}
CONDITION_KINDS = ["expect", "wait", "always"]
SAFE_STEM = re.compile(r"[A-Za-z0-9_-]+")


def check_scenario(case):
    """SCENARIO_SCHEMA plus the rules a schema cannot say."""
    try:
        json.dumps(case, allow_nan=False)
    except (ValueError, TypeError) as error:
        raise ConfigurationError(f"scenario is not finite JSON: {error}") from error
    validate(case, SCENARIO_SCHEMA, "scenario")
    if not case["name"]:
        raise ConfigurationError("scenario requires a name")
    seed = case.get("seed", 0)
    if isinstance(seed, str) and (not seed.isdecimal() or int(seed) > 2**64 - 1):
        raise ConfigurationError("seed must be an unsigned 64-bit integer or decimal string")
    if not 1 <= len(case["steps"]) <= 10000:
        raise ConfigurationError("scenario requires 1..10000 steps")
    for i, step in enumerate(case["steps"]):
        operations = step.keys() & {"call", "tick", "capture", *CONDITION_KINDS}
        if len(operations) != 1 or ("params" in step and "call" not in step):
            raise ConfigurationError(f"step {i}: choose one operation")
        if "capture" in step and not SAFE_STEM.fullmatch(step["capture"]):
            raise ConfigurationError(f"step {i}: capture must be a safe filename stem")
        for kind in CONDITION_KINDS:
            if kind in step:
                check_condition(step[kind], kind, f"step {i}")


def check_condition(condition, kind, where):
    path = condition.get("path", "")
    if path and not path.startswith("/"):
        raise ConfigurationError(f"{where}: condition path must be a JSON Pointer")
    if re.search(r"~(?![01])", path):
        raise ConfigurationError(f"{where}: invalid JSON Pointer escape")
    op = condition.get("op", "eq")
    if op not in ["exists", "absent"] and "value" not in condition:
        raise ConfigurationError(f"{where}: condition requires value")
    if op == "approx" and "tolerance" not in condition:
        raise ConfigurationError(f"{where}: approx requires a finite nonnegative tolerance")
    bound = {"wait": "within", "always": "ticks"}.get(kind)
    if bound and bound not in condition:
        raise ConfigurationError(f"{where}: {kind} requires {bound}=0..1000000")
    if any(other in condition for other in ["within", "ticks"] if other != bound):
        raise ConfigurationError(f"{where}: condition has a bound for a different assertion type")


MISSING = object()


def pointer(value, path):
    for raw in path.split("/")[1:]:
        token = raw.replace("~1", "/").replace("~0", "~")
        if isinstance(value, dict):
            value = value.get(token, MISSING)
        elif isinstance(value, list) and token.isdecimal() and (token == "0" or not token.startswith("0")):
            value = value[int(token)] if int(token) < len(value) else MISSING
        else:
            return MISSING
    return value


def matches(actual, condition):
    op, wanted = condition.get("op", "eq"), condition.get("value")
    if op == "exists":
        return actual is not MISSING
    if op == "absent":
        return actual is MISSING
    if actual is MISSING:
        return False
    if op == "eq":
        return (
            type(actual) is type(wanted)
            and actual == wanted
            or type(actual) in (int, float)
            and type(wanted) in (int, float)
            and actual == wanted
        )
    if op == "ne":
        return not matches(actual, {**condition, "op": "eq"})
    if op == "contains":
        return isinstance(actual, (str, list, dict)) and wanted in actual
    if type(actual) not in (int, float) or type(wanted) not in (int, float):
        return False
    if op == "approx":
        return abs(actual - wanted) <= condition["tolerance"]
    if op == "lt":
        return actual < wanted
    if op == "le":
        return actual <= wanted
    if op == "gt":
        return actual > wanted
    if op == "ge":
        return actual >= wanted
    return False


def difference(before, after, path="", limit=100):
    """Small field-level diffs; never infer bitwise determinism from them."""
    result = []

    def walk(a, b, p):
        if len(result) >= limit:
            return
        if isinstance(a, dict) and isinstance(b, dict):
            for key in sorted(a.keys() | b.keys()):
                walk(a.get(key, MISSING), b.get(key, MISSING), p + "/" + key.replace("~", "~0").replace("/", "~1"))
        elif isinstance(a, list) and isinstance(b, list):
            for i in range(max(len(a), len(b))):
                walk(a[i] if i < len(a) else MISSING, b[i] if i < len(b) else MISSING, p + "/" + str(i))
        elif a is MISSING or b is MISSING or type(a) is not type(b) or a != b:
            result.append(
                {
                    "path": p or "/",
                    "before": "<missing>" if a is MISSING else a,
                    "after": "<missing>" if b is MISSING else b,
                }
            )

    walk(before, after, path)
    return result


def identities(command, dependencies):
    executable = shutil.which(command[0]) or command[0]
    paths = [Path(executable).resolve()]
    for dependency in dependencies:
        path = Path(dependency).resolve()
        if not path.exists():
            raise ConfigurationError(f"missing dependency: {path}")
        paths.extend(
            sorted(
                p
                for p in path.rglob("*")
                if p.is_file()
                and not any(part.startswith(".") or part == "__pycache__" for part in p.relative_to(path).parts)
            )
            if path.is_dir()
            else [path]
        )
    if len(paths) > 20000:
        raise ConfigurationError("too many provenance files")
    result = []
    for path in dict.fromkeys(paths):
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        result.append({"path": str(path), "sha256": digest.hexdigest()})
    return result


class Client:
    """A driven stdio scene server, every exchange journaled to commands.jsonl.

    The journal starts with the kin.automation-recording/1 header the native
    recorder (--automation-record) writes, plus this run's command line and
    build/content identities. With `expected_identity` (a recording's), the run
    is refused before launching when the binary or declared content differs."""

    def __init__(
        self,
        command,
        out,
        *,
        seed=0,
        dependencies=(),
        timeout=15,
        launch=True,
        trace_world=False,
        expected_identity=None,
    ):
        self.out = Path(out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.timeout, self.frame, self.phase, self.index = timeout, 0, "bootstrap", 0
        self.events, self.responses, self.stderr_tail = deque(maxlen=2000), queue.Queue(), deque(maxlen=100)
        self.recorded_bytes = 0
        self.total_events = 0
        self._journal_lock = threading.Lock()
        self.recording_error = None
        self.protocol, self.capabilities, self.closed, self.broken = None, None, False, False
        self.shutdown_sent = False
        self.trace_world = trace_world
        self.last_protocol_error = None
        self.command = list(command)
        if not self.command:
            raise ConfigurationError("a game command is required")
        if type(seed) is not int or not 0 <= seed <= 2**64 - 1:
            raise ConfigurationError("invalid seed")
        if type(timeout) not in (int, float) or not math.isfinite(timeout) or not 0 < timeout <= 300:
            raise ConfigurationError("timeout must be finite and in 0..300 seconds")
        self.command[0] = str(Path(shutil.which(self.command[0]) or self.command[0]).resolve())
        for i, arg in enumerate(self.command[1:], 1):
            if launch and (
                arg in ["--inspect", "--server-mode=realtime", "--server-transport=http"]
                or (
                    arg in ["--server-mode", "--server-transport"]
                    and i + 1 < len(self.command)
                    and self.command[i + 1] != ("driven" if arg == "--server-mode" else "stdio")
                )
            ):
                raise ConfigurationError("scenarios require the driven stdio server")
            if arg in ["--content", "--pack"] and i + 1 < len(self.command):
                self.command[i + 1] = str(Path(self.command[i + 1]).resolve())
            elif arg.startswith(("--content=", "--pack=")):
                key, value = arg.split("=", 1)
                self.command[i] = key + "=" + str(Path(value).resolve())
        dependencies = list(dependencies)
        for i, arg in enumerate(self.command):
            if arg in ["--content", "--pack"] and i + 1 < len(self.command) and Path(self.command[i + 1]).exists():
                dependencies.append(self.command[i + 1])
            elif arg.startswith(("--content=", "--pack=")) and Path(arg.split("=", 1)[1]).exists():
                dependencies.append(arg.split("=", 1)[1])
        dependencies = list(dict.fromkeys(str(Path(p).resolve()) for p in dependencies))
        if any(Path(p).is_dir() and self.out.is_relative_to(Path(p)) for p in dependencies):
            raise ConfigurationError("output directory must be outside declared content/dependency directories")
        inventory = identities(self.command, dependencies)
        if expected_identity is not None and inventory != expected_identity:
            raise ConfigurationError("recording binary/content identity changed; use --allow-identity-change explicitly")
        self.seed, self.dependencies, self.initial_identity = seed, dependencies, inventory
        self.game_info = {}
        self.journal, self._unwritten = None, []  # lines emitted before the header
        self.stderr = (self.out / "stderr.log").open("w", encoding="utf-8")
        try:
            self.process = subprocess.Popen(
                self.command
                + (
                    ["--server", "--server-mode=driven", "--server-transport=stdio", f"--seed={seed}"] if launch else []
                ),
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                cwd=self.out,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                start_new_session=os.name != "nt",
            )
        except Exception:
            self.stderr.close()
            raise
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()
        self._errors = threading.Thread(target=self._read_errors, daemon=True)
        self._errors.start()
        try:
            self.protocol = self.call("server.schema")
            if self.protocol.get("schema") != "kin.automation/1" or self.protocol.get("version") != 1:
                raise ConfigurationError("unsupported automation protocol")
            self.capabilities = self.call("server.capabilities")
            self.game_info = self.call("game.info")
        except Exception:
            self.close()
            raise
        self._open_journal()
        dump(self.out / "schema.json", self.protocol)
        dump(self.out / "capabilities.json", self.capabilities)
        self.phase = "scenario"

    def _emit(self, record):
        encoded = json.dumps(record, ensure_ascii=True, allow_nan=False) + "\n"
        with self._journal_lock:
            if self.recording_error or self.recorded_bytes + len(encoded) > 256 * 1024 * 1024:
                raise ScenarioFailure(self.recording_error or "recording exceeded 256 MiB")
            self.total_events += 1
            self.recorded_bytes += len(encoded)
            if len(encoded) > 65536:
                name = f"event-{self.total_events:06}.json"
                dump(self.out / name, record)
                self.events.append(
                    {k: v for k, v in record.items() if k not in ["result", "params"]} | {"artifact": name}
                )
            else:
                self.events.append(record)
            if self.journal is None:
                self._unwritten.append(encoded)
            else:
                self.journal.write(encoded)
                self.journal.flush()

    def header(self):
        capabilities = self.capabilities or {}
        return {
            "kind": "header",
            "schema": "kin.automation-recording/1",
            "seed": str(self.seed),
            "fixed_dt": capabilities.get("fixed_dt"),
            "rng_key": capabilities.get("rng_key"),
            "renderer": capabilities.get("renderer"),
            "game_id": self.game_info.get("id", ""),
            "game_version": self.game_info.get("version", ""),
            "command": self.command,
            "dependencies": self.dependencies,
            "identity": self.initial_identity,
            "declared_content_verified": bool(self.dependencies),
            "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "python": sys.version.split()[0],
        }

    def _open_journal(self):
        """The header needs the handshake's answers; what came before follows it."""
        header = self.header()
        encoded = json.dumps(header, ensure_ascii=True, allow_nan=False) + "\n"
        with self._journal_lock:
            if self.journal is not None:
                return
            self.journal = (self.out / "commands.jsonl").open("w", encoding="utf-8")
            self.journal.write(encoded)
            self.journal.writelines(self._unwritten)
            self.journal.flush()
            self._unwritten = []
            self.events.appendleft(header)
            self.total_events += 1
            self.recorded_bytes += len(encoded)

    def timeline(self):
        with self._journal_lock:
            return list(self.events)

    def verify_identity(self):
        if identities(self.command, self.dependencies) != self.initial_identity:
            raise ScenarioFailure("binary or declared content changed during the run")

    def _read(self):
        try:
            while True:
                line = self.process.stdout.readline(16 * 1024 * 1024 + 1)
                if not line:
                    break
                if len(line) > 16 * 1024 * 1024:
                    raise ScenarioFailure("server response exceeds 16 MiB")
                self.responses.put(loads(line))
        except Exception as error:
            self.responses.put(error)
        finally:
            self.responses.put(ScenarioFailure("game closed its protocol stream"))

    def _read_errors(self):
        total = 0
        for line in self.process.stderr:
            self.stderr_tail.append(line.rstrip())
            if total < 64 * 1024 * 1024:
                self.stderr.write(line)
                self.stderr.flush()
                total += len(line)
                try:
                    self._emit({"kind": "log", "frame_observed": self.frame, "message": line.rstrip()})
                except Exception as error:
                    self.recording_error = str(error)

    def validate_request(self, method, params):
        descriptor = self.protocol["methods"].get(method)
        if not descriptor or descriptor.get("driven") is False:
            raise ConfigurationError(f"unsupported driven-server method: {method}")
        validate(params, descriptor["params"], method)
        if method == "ui.action" and ("id" in params) == ("selector" in params):
            raise ConfigurationError("ui.action requires exactly one of id or selector")
        if "selector" in params and method.startswith("ui."):
            self._selector(params["selector"])

    def call(self, method, params=None, *, strict=True):
        params = {} if params is None else params
        if self.protocol and strict:
            self.validate_request(method, params)
        if self.closed or self.broken:
            raise ScenarioFailure("server transport is unavailable")
        self.index += 1
        record = {
            "kind": "command",
            "sequence": self.index,
            "phase": self.phase,
            "frame_before": self.frame,
            "method": method,
            "params": params,
        }
        started = time.monotonic()
        self._emit({**record, "kind": "request"})
        try:
            self.process.stdin.write(
                json.dumps({"id": self.index, "method": method, "params": params}, allow_nan=False) + "\n"
            )
            self.process.stdin.flush()
            response = self.responses.get(timeout=self.timeout)
            if isinstance(response, Exception):
                raise response
            if not isinstance(response, dict) or response.get("id") != self.index:
                raise ScenarioFailure(f"protocol response ID mismatch: {response!r}")
            if "error" in response:
                self.last_protocol_error = response["error"]
                record.update(ok=False, error=response["error"], protocol_error=response["error"])
                raise ScenarioFailure(f'{method}: {response["error"]}')
            if "result" not in response:
                raise ScenarioFailure("protocol response has no result")
            result = response["result"]
            if self.protocol and strict:
                try:
                    validate(result, self.protocol["methods"][method].get("result", {}), method + ".result")
                except ConfigurationError as error:
                    raise ScenarioFailure(f"invalid server result: {error}") from error
            if method in ["sim.tick", "sim.reset", "server.status"]:
                self.frame = result.get("frame", self.frame)
                if result.get("failed"):
                    record.update(ok=False, result=result, error="game reported a failure")
                    raise ScenarioFailure(f"game reported a failure at frame {self.frame}: {result}")
            record.update(ok=True, result=result)
            if method == "server.shutdown":
                self.shutdown_sent = True
            return result
        except queue.Empty as error:
            self.broken = True
            record.update(ok=False, error="process deadline exceeded")
            raise ScenarioFailure(f"{method}: process deadline exceeded ({self.timeout}s)") from error
        except (BrokenPipeError, OSError) as error:
            self.broken = True
            record.update(ok=False, error=str(error))
            raise ScenarioFailure(f"{method}: {error}") from error
        except Exception as error:
            if "ok" not in record:
                self.broken = True
            record.update(ok=False, error=str(error))
            raise
        finally:
            record.update(frame=self.frame, elapsed_ms=(time.monotonic() - started) * 1000)
            self._emit(record)

    def _selector(self, selector, depth=0):
        if depth > 16:
            raise ConfigurationError("UI selector nesting exceeds 16")
        if not isinstance(selector, dict):
            raise ConfigurationError("UI selector must be an object")
        schema = self.protocol["ui_selector"]
        validate(
            {k: v for k, v in selector.items() if k != "within"},
            {**schema, "properties": {k: v for k, v in schema["properties"].items() if k != "within"}},
        )
        if "within" in selector:
            self._selector(selector["within"], depth + 1)

    def close(self):
        if self.closed:
            return self.process.returncode
        self._open_journal()
        if self.process.poll() is None:
            try:
                if self.broken:
                    raise ScenarioFailure("broken transport")
                self.phase = "shutdown"
                if not self.shutdown_sent:
                    self.call("server.shutdown")
                self.process.stdin.close()
                self.process.wait(timeout=2)
            except Exception:
                stop_process_group(self.process)
        self.closed = True
        end_process_group(self.process)
        self._reader.join(timeout=2)
        self._errors.join(timeout=2)
        for stream in [self.process.stdin, self.process.stdout, self.process.stderr, self.stderr, self.journal]:
            stream.close()
        return self.process.returncode


class Runner:
    def __init__(self, client):
        self.client, self.steps = client, []

    def check_read_only(self, condition):
        """Conditions only read: an observed method with effects would change what it tests."""
        descriptor = self.client.protocol["methods"].get(condition["observe"], {})
        if descriptor.get("effect") not in ["read", "layout"]:
            raise ConfigurationError("conditions may only observe read/layout methods")

    def observe(self, condition):
        self.check_read_only(condition)
        result = self.client.call(condition["observe"], condition.get("params", {}))
        actual = pointer(result, condition.get("path", ""))
        return matches(actual, condition), actual

    def condition(self, condition, kind):
        bound = condition.get("within" if kind == "wait" else "ticks", 0)
        start = self.client.frame
        for elapsed in range(bound + 1):
            passed, actual = self.observe(condition)
            if kind == "wait" and passed:
                return
            if kind != "wait" and not passed or kind == "wait" and elapsed == bound:
                detail = {
                    "condition": condition,
                    "actual": "<missing>" if actual is MISSING else actual,
                    "frame": self.client.frame,
                    "start_frame": start,
                    "ticks": elapsed,
                }
                self.client._emit({"kind": "assertion", "ok": False, **detail})
                raise ScenarioFailure(f"{kind} failed at tick {self.client.frame}: {detail}")
            if elapsed < bound:
                self.client.call("sim.tick", {"count": 1})

    def run(self, case):
        check_scenario(case)
        for requirement in case.get("requires", []):
            if self.client.capabilities.get(requirement) is not True:
                raise ScenarioFailure(f"required capability unavailable: {requirement}")
        # Validate the complete plan before the first action changes game state.
        for step in case["steps"]:
            if step.get("call") == "server.shutdown":
                raise ConfigurationError("the scenario runner owns process shutdown")
            if "call" in step:
                self.client.validate_request(step["call"], step.get("params", {}))
            for kind in CONDITION_KINDS:
                if kind in step:
                    condition = step[kind]
                    self.client.validate_request(condition["observe"], condition.get("params", {}))
                    self.check_read_only(condition)
        self.client.call("automation.snapshot", {"world": self.client.trace_world})
        for i, step in enumerate(case["steps"]):
            entry = {"index": i, "name": step.get("name", f"Step {i + 1}"), "frame_before": self.client.frame}
            self.client.phase = entry["name"]
            try:
                if "call" in step:
                    self.client.call(step["call"], step.get("params", {}))
                elif "tick" in step:
                    self.client.call("sim.tick", {"count": step["tick"]})
                elif "capture" in step:
                    self.client.call("view.screenshot", {"path": step["capture"] + ".png"})
                else:
                    kind = next(k for k in CONDITION_KINDS if k in step)
                    self.condition(step[kind], kind)
                entry["ok"] = True
                self.client.call("automation.snapshot", {"world": self.client.trace_world})
            except Exception as error:
                entry.update(ok=False, error=str(error))
                raise
            finally:
                entry["frame"] = self.client.frame
                self.steps.append(entry)
                self.client._emit({"kind": "step", **entry})

    def artifacts(self, name, error=None):
        client = self.client
        failure = {"message": str(error)} if error else None
        if error:
            client.phase = "diagnostics"
            for method, params, filename in [
                ("automation.snapshot", {"world": True}, "state.json"),
                ("ui.snapshot", {}, "ui.json"),
                ("world.snapshot", {"raw": False, "animations": False}, "world.json"),
                ("view.screenshot", {"path": "failure.png"}, None),
            ]:
                try:
                    result = client.call(method, params)
                    if filename:
                        dump(client.out / filename, result)
                except Exception as capture_error:
                    failure.setdefault("unavailable_artifacts", {})[method] = str(capture_error)
            events = client.timeline()

            def full(event):
                return loads((client.out / event["artifact"]).read_text()) if "artifact" in event else event

            successful = [
                full(event).get("result")
                for event in events
                if event.get("method") == "automation.snapshot" and event.get("ok")
            ]
            if len(successful) > 1:

                def coverage(value):
                    if client.trace_world:
                        return value
                    return {
                        **value,
                        "scenes": [
                            {k: v for k, v in scene.items() if k != "world"} for scene in value.get("scenes", [])
                        ],
                    }

                snapshots = [coverage(value) for value in successful]
                previous = next((v for v in reversed(snapshots[:-1]) if v != snapshots[-1]), snapshots[-2])
                failure["state_diff"] = difference(previous, snapshots[-1])
                failure["state_diff_scope"] = (
                    "scene reports, UI and world" if client.trace_world else "scene reports and UI"
                )
            assertion = next((e for e in reversed(events) if e.get("kind") == "assertion"), None)
            if assertion:
                failure["assertion"] = assertion
            failure["stderr_tail"] = list(client.stderr_tail)
            dump(client.out / "failure.json", failure)
        result = {
            "schema": "kin.scenario-result/1",
            "name": name,
            "status": "failed" if error else "passed",
            "frame": client.frame,
            "steps": self.steps,
            "failure": failure,
            "trace_window": {"retained": len(client.timeline()), "total": client.total_events},
        }
        dump(client.out / "result.json", result)
        suite = ET.Element("testsuite", name="kin.gameplay", tests="1", failures="1" if error else "0")

        def xml_text(value):
            return "".join(
                c
                for c in str(value)
                if c in "\t\n\r"
                or 0x20 <= ord(c) <= 0xD7FF
                or 0xE000 <= ord(c) <= 0xFFFD
                or 0x10000 <= ord(c) <= 0x10FFFF
            )

        test = ET.SubElement(suite, "testcase", name=xml_text(name), classname="kin.gameplay")
        if error:
            ET.SubElement(test, "failure", message=xml_text(error)).text = xml_text(error)
        ET.ElementTree(suite).write(client.out / "junit.xml", encoding="utf-8", xml_declaration=True)
        timeline = json.dumps({"result": result, "events": client.timeline()}, ensure_ascii=True).replace(
            "<", "\\u003c"
        )
        document = r"""<!doctype html><meta charset="utf-8"><title>Kin test trace</title>
<style>body{font:14px system-ui;margin:24px;background:#101821;color:#e2edf7}button{display:block;text-align:left;width:100%;margin:3px 0;padding:8px;background:#233449;color:inherit;border:0;cursor:pointer}.grid{display:grid;grid-template-columns:330px 1fr;gap:24px}pre{white-space:pre-wrap;overflow-wrap:anywhere}img{max-width:100%}input{padding:8px;width:95%}</style>
<h1>Kin gameplay trace</h1><p id="summary"></p><div class="grid"><aside><input id="filter" placeholder="Filter actions / phases"><div id="events"></div></aside><main><a id="artifact" hidden>Open full event JSON</a><pre id="detail"></pre><img src="failure.png" alt="Failure screenshot" onerror="this.hidden=true"></main></div>
<script id="data" type="application/json">DATA</script><script>
const data=JSON.parse(document.getElementById('data').textContent);document.getElementById('summary').textContent=data.result.name+': '+data.result.status;
function show(){const root=document.getElementById('events');root.replaceChildren();const query=document.getElementById('filter').value.toLowerCase();for(const e of data.events){if(!JSON.stringify(e).toLowerCase().includes(query))continue;const b=document.createElement('button');b.textContent=(e.frame===undefined?'':e.frame+' · ')+(e.phase||e.kind)+' '+(e.method||e.name||'');b.onclick=()=>{document.getElementById('detail').textContent=JSON.stringify(e,null,2);const link=document.getElementById('artifact');link.hidden=!/^event-[0-9]+\.json$/.test(e.artifact||'');if(!link.hidden)link.href=e.artifact};root.append(b)}}document.getElementById('filter').oninput=show;show();document.getElementById('detail').textContent=JSON.stringify(data.result,null,2);
</script>""".replace(
            "DATA", timeline
        )
        (client.out / "trace.html").write_text(document, encoding="utf-8")
        return result


def replay(client, recording, *, compare=True):
    """Replay successful requests in order; diagnostic timings are not an oracle."""
    completed = {r.get("sequence") for r in recording if r.get("kind") == "command"}
    frames = []
    for entry in recording:
        if entry.get("kind") == "frame":
            frames.append(entry)
            continue
        if entry.get("kind") == "assertion" and not entry.get("ok", True):
            condition = entry.get("condition")
            if condition:
                passed, _ = Runner(client).observe(condition)
                if not passed:
                    raise ScenarioFailure("recorded assertion failure reproduced")
        if entry.get("kind") == "request":
            if entry.get("sequence") not in completed:
                client.call(entry["method"], entry.get("params", {}), strict=False)
                raise ScenarioFailure("interrupted recorded command completed; original recording is incomplete")
            continue
        if entry.get("kind") != "command" or entry.get("phase") in ["bootstrap", "shutdown", "diagnostics"]:
            continue
        method = entry["method"]
        if method == "server.shutdown":
            continue
        if not entry.get("ok", True):
            client.last_protocol_error = None
            try:
                client.call(method, entry.get("params", {}), strict=False)
            except ScenarioFailure:
                if client.broken:
                    raise
                expected = entry.get("protocol_error", entry.get("error"))
                actual = client.last_protocol_error
                if isinstance(actual, dict):
                    actual = actual.get("message", actual)
                if isinstance(expected, dict):
                    expected = expected.get("message", expected)
                if compare and expected != actual:
                    raise ScenarioFailure(f"recorded protocol error changed: {method}: {expected!r} -> {actual!r}")
                raise ScenarioFailure(f"recorded protocol error reproduced: {method}")
            raise ScenarioFailure(f"recorded protocol error no longer reproduced: {method}")
        params = dict(entry.get("params", {}))
        if method == "dev.reload" and isinstance(entry.get("result", {}).get("changes"), dict):
            params["changes"] = entry["result"]["changes"]
        if method in ["view.screenshot", "view.frame"]:
            params["path"] = f'replay-{entry.get("sequence", 0)}' + (".png" if method == "view.screenshot" else ".rgba")
        if frames:
            if method != "sim.tick" or len(frames) != params.get("count", 1):
                raise ScenarioFailure("recording contains incomplete or misplaced simulation frames")
            actual = client.call(method, params, strict=False)
            observed = client.call("automation.snapshot")
            delta = difference(frames[-1]["snapshot"], observed) if compare else []
            if delta:
                raise ScenarioFailure(f'replay diverged at native frame {frames[-1].get("frame")}: {delta[:8]}')
            frames.clear()
        else:
            actual = client.call(method, params, strict=False)
        if compare and method not in [
            "frame.timing",
            "server.schema",
            "server.capabilities",
            "world.snapshot",
            "view.screenshot",
            "view.frame",
        ]:
            delta = difference(entry.get("result"), actual)
            if delta:
                raise ScenarioFailure(f"replay diverged at {method}: {delta[:8]}")
    if frames:
        raise ScenarioFailure("recording ends with incomplete simulation frames")
