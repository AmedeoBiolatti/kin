#!/usr/bin/env python3
"""Generate editor definitions and check statically recognizable Kin Lua calls.

The native tool supplies the actual build's API surface and component schemas.
This checker deliberately leaves dynamic Lua expressions to runtime validation.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import subprocess
import sys

# Parameter/return documentation supplements the live binding inventory. Entries
# are checked against that inventory; unavailable optional APIs are omitted.
SIGNATURES = {
    "t": {
        "spawn": "name:string,components?:table -> integer",
        "get": "id:integer,component:string -> table|nil",
        "patch": "id:integer,component:string,fields:table",
        "attach": "id:integer,path:string",
        "destroy": "id:integer",
        "alive": "id:integer -> boolean",
        "enabled": "id:integer,enabled:boolean",
        "input": "action:string,held:boolean",
        "step": "frames?:integer,dt?:number",
        "time": "-> number",
        "frame": "-> integer",
        "seed": "seed:integer",
        "random": "-> number",
        "errors": "-> table",
        "edit": "path:string,text:string",
        "reload": "path:string -> boolean",
        "scene": "path:string",
        "reload_scene": "path:string -> boolean",
        "reload_error": "-> string",
        "equal": "actual:any,expected:any,message?:string",
        "near": "actual:number,expected:number,epsilon?:number",
        "raises": "fn:function,pattern?:string",
    },
    "task": {
        "run": "fn:KinTaskBody -> KinTask",
        "wait": "seconds:number",
        "frames": "count:integer",
        "until_": "predicate:KinPredicate,timeout?:number",
        "await": "handle:userdata|table,timeout?:number",
        "cancel": "handle:KinTask",
    },
    "KinTask": {"state": "-> string", "error": "-> string", "cancel": ""},
    "KinEntity": {
        "id": "-> integer",
        "name": "-> string",
        "valid": "-> boolean",
        "alive": "-> boolean",
        "destroy": "",
        "has": "component:string -> boolean",
        "get": "component:string -> table|nil",
        "component": "component:string -> table",
        "child": "name:string -> KinEntity|nil",
        "patch": "component:string,fields:table -> KinEntity",
        "remove": "component:string -> KinEntity",
        "add_script_owned": "-> KinEntity",
        "timer": "name:string,seconds:number,repeating?:boolean -> KinEntity",
        "cooldown": "name:string,seconds:number -> KinEntity",
        "ready": "name:string -> boolean",
        "set_transform": "x:number,y:number -> KinEntity",
        "get_transform": "-> KinVec2|nil",
        "translate": "x:number,y:number -> KinEntity",
        "set_rect": "width:number,height:number,r:integer,g:integer,b:integer,layer?:integer,order?:integer -> KinEntity",
        "get_rect": "-> KinGet_RectRenderer|nil",
        "set_line": "ax:number,ay:number,bx:number,by:number,r:integer,g:integer,b:integer,layer?:integer,order?:integer -> KinEntity",
        "get_line": "-> KinGet_LineRenderer|nil",
    },
    "Entity": {
        "id": "-> integer",
        "name": "-> string",
        "alive": "-> boolean",
        "parent": "-> Entity|nil",
        "child": "name:string -> Entity|nil",
        "get": "component:string -> table|nil",
        "set": "component:string,fields:table",
        "has": "component:string -> boolean",
        "add": "component:string",
        "remove": "component:string",
        "position": "-> number,number",
        "set_position": "x:number,y:number",
        "move": "x:number,y:number",
        "rotation": "-> number",
        "set_rotation": "degrees:number",
        "velocity": "-> number,number",
        "set_velocity": "x:number,y:number",
        "on_floor": "-> boolean",
        "on_wall": "-> boolean",
        "on_ceiling": "-> boolean",
        "destroy": "",
        "play": "animation:string",
        "set_param": "name:string,value:boolean|number",
        "trigger": "name:string",
        "state": "-> string",
        "emit": "effect?:string -> boolean",
    },
    "KinScene": {
        "entity": "name?:string -> KinEntity",
        "find": "name:string -> KinEntity|nil",
        "authored": "id:string -> KinEntity|nil",
        "entities": "-> KinEntity[]",
        "with": "...:string -> KinEntity[]",
        "count": "...:string -> integer",
        "register_system": "id:string,descriptor:table,run:KinSystemCallback -> boolean",
        "clear_script_entities": "",
        "set_clear_color": "r:integer,g:integer,b:integer",
        "mark_render_cache_dirty": "",
    },
    "KinInput": {
        name: "action:string -> boolean" for name in ("pressed", "held", "released")
    },
    "KinApp": {"quit": ""},
    "KinDialoguePlayer": {
        "load": "path:string -> boolean",
        "start": "node?:string",
        "advance": "",
        "choose_index": "index:integer -> boolean",
        "choose_id": "id:string -> boolean",
        "skip_line": "",
        "reset": "",
        "view": "-> table",
        "consume_events": "-> table[]",
    },
    "KinActions": {"add": "action:string,label?:string"},
    "KinCommandBuffer": {
        "create": "name?:string -> KinDeferredEntity",
        "destroy": "entity:KinEntity|KinDeferredEntity|integer",
        "add": "entity:KinEntity|KinDeferredEntity|integer,component:string -> KinCommandBuffer",
        "remove": "entity:KinEntity|KinDeferredEntity|integer,component:string",
        "patch": "entity:KinEntity|KinDeferredEntity|integer,component:string,fields:table",
        "flush": "",
        "clear": "",
    },
    "KinTween2D": {
        "to": "property:string,destination:any,seconds:number,easing?:string -> KinTween2D",
        "wait": "seconds:number -> KinTween2D",
        "parallel": "-> KinTween2D",
        **{name: "" for name in ("pause", "resume", "cancel", "restart")},
        **{name: "-> boolean" for name in ("paused", "finished", "cancelled")},
    },
    "KinPrefabLease2D": {
        "alive": "-> boolean",
        "release": "-> boolean",
        "entity": "-> Entity|KinEntity",
    },
    "KinScenePreload2D": {
        "state": "-> string",
        "progress": "-> number",
        "error": "-> string",
        "cancel": "",
        "commit": "-> boolean",
    },
    "tweens": {
        "to": "entity:string|userdata|table,property:string,destination:any,seconds:number,easing?:string -> KinTween2D"
    },
    "input": {
        **{
            name: "action:string -> boolean" for name in ("held", "pressed", "released")
        },
        "axis": "name:string -> number",
        "mouse": "-> number,number",
    },
    "world": {
        "find": "name:string -> Entity|nil",
        "spawn": "prefab:string,x:number,y:number -> Entity|nil",
    },
    "camera": {"shake": "trauma:number", "trauma": "-> number"},
    "ui": {
        **{name: "id:string -> boolean" for name in ("clicked", "changed", "checked")},
        "value": "id:string -> number",
        "text": "id:string -> string",
        "set_text": "id:string,text:string",
        "set_value": "id:string,value:number",
        "set_checked": "id:string,value:boolean",
        "show": "id:string,visible?:boolean",
        "hide": "id:string",
        "set_image": "id:string,path:string",
        "set_enabled": "id:string,enabled:boolean",
    },
    "physics": {
        "raycast": "x1:number,y1:number,x2:number,y2:number,options?:table -> table|nil",
        "cast_circle": "x1:number,y1:number,x2:number,y2:number,radius:number,options?:table -> table|nil",
        "cast_box": "x1:number,y1:number,x2:number,y2:number,width:number,height:number,options?:table -> table|nil",
        "overlap_point": "x:number,y:number,options?:table -> Entity[]",
        "overlap_circle": "x:number,y:number,radius:number,options?:table -> Entity[]",
        "overlap_box": "x:number,y:number,width:number,height:number,options?:table -> Entity[]",
    },
    "dialogue": {
        "start": "path:string,node?:string",
        "stop": "",
        "active": "-> boolean",
        "advance": "",
        "choose": "choice:integer|string",
        "node": "-> string",
        "get": "name:string -> any",
        "set": "name:string,value:any",
    },
    "scene": {
        "go": "path:string,arrival?:string,transition?:string,seconds?:number",
        "going": "-> boolean",
    },
    "save": {
        "write": "slot:string,title?:string,summary?:string -> boolean|nil,string|nil",
        "read": "slot:string -> boolean,string|nil",
        "exists": "slot:string -> boolean",
        "delete": "slot:string -> boolean",
        "slots": "-> table[]",
        "get": "name:string,default?:any -> any",
        "set": "name:string,value:any",
    },
    "preload": {"scene": "path:string -> KinScenePreload2D"},
    "pools": {
        "create": "name:string,path:string,capacity:integer",
        "acquire": "name:string,x?:number,y?:number -> KinPrefabLease2D|nil",
        "active": "name:string -> integer",
        "destroy": "name:string -> boolean",
    },
    "controllers": {
        "devices": "-> table[]",
        "join": "device:integer,player:integer -> boolean",
        "leave": "player:integer",
        "state": "player:integer -> table",
        "rumble": "player:integer,low:number,high:number,milliseconds:integer -> boolean",
        "label": "player:integer,button:integer -> string",
    },
    "asset_memory": {
        "budget": "cpu:integer,gpu:integer",
        "report": "-> table",
        "residency": "-> table",
        "trim": "-> table",
    },
    "input_contexts": {
        "push": "name:string,map:string,modal?:boolean,consume?:boolean",
        "remove": "name:string -> boolean",
        "enable": "name:string,enabled:boolean",
        **{
            name: "context:string,action:string -> boolean"
            for name in ("held", "pressed", "released")
        },
        "axis": "context:string,axis:string -> number",
        "overrides": "context:string -> string",
        "apply_overrides": "context:string,text:string",
        "save": "context:string,profile:string -> boolean",
        "load": "context:string,profile:string",
    },
    "rebind": {
        "begin": "action:string,timeout?:number",
        "cancel": "",
        "state": "-> string",
        "candidate": "-> string|nil",
        "conflicts": "context:string -> table",
        "apply": "context:string,policy?:string",
    },
    "l10n": {
        "locale": "-> string",
        "direction": "-> string",
        "has": "key:string -> boolean",
        "languages": "-> table[]",
        "set_locale": "locale:string -> boolean",
        "format": "pattern:string,values?:table -> string",
        "tr": "key:string,values?:table -> string",
    },
}
PROPERTIES = {
    "KinDialoguePlayer": {"ended": "boolean", "current_node": "string"},
    "KinContext": {"dt": "number", "input": "KinInput", "app": "KinApp"},
    "KinDeferredEntity": {"index": "integer"},
    "KinSystemContext": {
        "dt": "number",
        "system_id": "string",
        "phase": "string",
        "running_parallel": "boolean",
        "commands": "KinCommandBuffer",
    },
    "KinCommandBuffer": {"size": "integer", "valid": "boolean"},
}
FIELD_TYPES = {
    "bool": "boolean",
    "string": "string",
    "vec2f": "KinVec2",
    "vec2i": "KinVec2",
    "color": "KinColor",
    "asset_ref": "string",
    "entity_ref": "integer",
    "points": "KinVec2[]",
    "vec3f": "table",
    "quatf": "table",
}
CALLABLE_TYPES = {"KinTaskBody", "KinPredicate", "KinSystemCallback"}


def signature(table, member):
    value = SIGNATURES.get(table, {}).get(member)
    if value is None:
        return [("...", "any")], "any"
    args, _, returns = value.partition("->")
    params = [
        tuple(item.strip().split(":", 1))
        for item in args.strip().split(",")
        if item.strip()
    ]
    return params, returns.strip()


def field_type(kind):
    return FIELD_TYPES.get(kind, "integer" if kind.startswith(("i", "u")) else "number")


def schema(native, host, components=()):
    command = [str(native), "api", "--host", host]
    for component in components:
        command.extend(["--components", str(component)])
    result = subprocess.run(
        command, text=True, encoding="utf-8", capture_output=True, check=True
    )
    return json.loads(result.stdout)


def get_fields(api, name):
    if api["host"] == "scene":
        legacy = {
            "Transform2D": ("x", "y"),
            "RectRenderer": ("w", "h", "r", "g", "b", "layer", "order", "visible"),
            "LineRenderer": (
                "ax",
                "ay",
                "bx",
                "by",
                "r",
                "g",
                "b",
                "layer",
                "order",
                "visible",
            ),
        }
        if name in legacy:
            return {
                field: {
                    "kind": "bool" if field == "visible" else "f32",
                    "readonly": False,
                    "description": "",
                }
                for field in legacy[name]
            }
    return api["components"][name]


def generate(api):
    lines = [
        "---@meta",
        "-- Generated by tools/kin_lua.py from this build's bindings. Do not execute.",
        "---@class KinVec2",
        "---@field x number",
        "---@field y number",
        "---@class KinColor",
        "---@field r integer",
        "---@field g integer",
        "---@field b integer",
        "---@field a integer",
        "",
    ]
    names = list(api["components"])
    lines.extend(
        ["---@alias KinTaskBody fun()", "---@alias KinPredicate fun(): boolean"]
    )
    if "KinSystemContext" in api["tables"]:
        lines.append(
            "---@alias KinSystemCallback fun(entity: KinEntity, dt: number, context: KinSystemContext)"
        )
    if api["host"] == "scene":
        lines.append("---@alias Entity KinEntity")
    elif api["host"] == "entity":
        lines.append("---@alias KinEntity Entity")
    if api["host"] == "test":
        lines.extend(
            [
                "---@param name string",
                "---@param fn fun()",
                "function test(name, fn) end",
            ]
        )
    lines.append("---@alias KinComponentName " + "|".join(repr(name) for name in names))
    for name, fields in api["components"].items():
        for patch in (False, True):
            lines.append(f"---@class Kin{'Patch' if patch else 'Component'}_{name}")
            for member, info in fields.items():
                description = info["description"].replace("\n", " ")
                readonly = " Read-only." if info["readonly"] else ""
                lines.append(
                    f"---@field {member}{'?' if patch else ''} {field_type(info['kind'])} {description}{readonly}"
                )
        lines.append(f"---@class KinGet_{name}")
        for member, info in get_fields(api, name).items():
            lines.append(f"---@field {member} {field_type(info['kind'])}")
        lines.append(f"---@class KinMutable_{name}: KinComponent_{name}")
        lines.append(
            f"---@field commit fun(self: KinMutable_{name}): KinMutable_{name}"
        )
        lines.append("")
    for table, members in api["tables"].items():
        cls = table if table[0].isupper() else "KinAPI_" + table
        lines.append(f"---@class {cls}")
        lines.extend(
            f"---@field {member} {typ}"
            for member, typ in PROPERTIES.get(table, {}).items()
            if member in members
        )
        lines.append(
            f"local {table} = {{}}"
            if table[0].isupper() and table != "KinDialoguePlayer"
            else f"{table} = {{}}"
        )
        for member in members:
            prop = PROPERTIES.get(table, {}).get(member)
            if prop:
                continue
            params, returns = signature(table, member)
            if table == "KinEntity" and member == "get":
                for alias, canonical in {
                    "transform": "Transform2D",
                    "rect": "RectRenderer",
                    "line": "LineRenderer",
                }.items():
                    lines.append(
                        f"---@overload fun(self: KinEntity, component: '{alias}'): KinGet_{canonical}?"
                    )
                for alias in ("script_owned", "ScriptOwned"):
                    lines.append(
                        f"---@overload fun(self: KinEntity, component: '{alias}'): boolean|nil"
                    )
            if table in ("Entity", "KinEntity") and member in (
                "get",
                "component",
                "patch",
                "set",
            ):
                for name in names:
                    if member in ("get", "component"):
                        overload = f"fun(self: {table}, component: '{name}'): Kin{'Get' if member=='get' else 'Mutable'}_{name}{'?' if member=='get' else ''}"
                    else:
                        overload = f"fun(self: {table}, component: '{name}', fields: KinPatch_{name})"
                    lines.append("---@overload " + overload)
            if table == "t" and member in ("get", "patch"):
                for name in names:
                    if member == "get":
                        overload = f"fun(id: integer, component: '{name}'): KinComponent_{name}?"
                    else:
                        overload = f"fun(id: integer, component: '{name}', fields: KinPatch_{name})"
                    lines.append("---@overload " + overload)
            lines.extend(f"---@param {name} {typ}" for name, typ in params)
            if returns:
                lines.append("---@return " + returns)
            sep = ":" if table[0].isupper() else "."
            lines.append(
                f"function {table}{sep}{member}({', '.join(name.rstrip('?') for name, _ in params)}) end"
            )
        lines.append("")
        if table == "KinDialoguePlayer":
            lines.extend(
                [
                    "---@return KinDialoguePlayer",
                    "function KinDialoguePlayer.new() end",
                    "",
                ]
            )
    return "\n".join(lines)


@dataclass
class Token:
    value: str
    kind: str
    line: int


def tokens(source):
    result, i, line = [], 0, 1
    while i < len(source):
        start, current = i, line
        c = source[i]
        if c.isspace():
            i += 1
        elif source.startswith("--", i) or c == "[":
            comment = source.startswith("--", i)
            offset = i + 2 if comment else i
            long = re.match(r"\[(=*)\[", source[offset:])
            if long:
                begin = offset + long.end()
                end = source.find("]" + long[1] + "]", begin)
                i = len(source) if end < 0 else end + len(long[1]) + 2
                if not comment:
                    result.append(Token(source[begin:end], "string", current))
            elif comment:
                end = source.find("\n", i)
                i = len(source) if end < 0 else end
            else:
                result.append(Token(c, "symbol", current))
                i += 1
        elif c in "\"'":
            quote = c
            i += 1
            while i < len(source):
                if source[i] == "\\":
                    i += 2
                elif source[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
            text = source[start + 1 : i - 1]
            result.append(
                Token(text, "string" if "\\" not in text else "dynamic-string", current)
            )
        elif c.isalpha() or c == "_":
            i += 1
            while i < len(source) and (source[i].isalnum() or source[i] == "_"):
                i += 1
            result.append(Token(source[start:i], "identifier", current))
        elif c.isdigit():
            match = re.match(
                r"(?:0[xX][0-9a-fA-F]+|\d+(?:\.\d*)?(?:[eE][+-]?\d+)?)", source[i:]
            )
            i += len(match[0])
            result.append(Token(match[0], "number", current))
        else:
            op = next(
                (
                    op
                    for op in (
                        "...",
                        "..",
                        "==",
                        "~=",
                        "<=",
                        ">=",
                        "::",
                        "//",
                        "<<",
                        ">>",
                    )
                    if source.startswith(op, i)
                ),
                c,
            )
            result.append(Token(op, "symbol", current))
            i += len(op)
        line += source[start:i].count("\n")
    return result


def arguments(ts, begin):
    args, start, stack = [], begin + 1, [")"]
    pending_do = 0
    matching = {"(": ")", "{": "}", "[": "]"}
    for i in range(begin + 1, len(ts)):
        value = ts[i].value
        if ts[i].kind in ("string", "dynamic-string"):
            continue
        if (
            value in ("function", "if", "for", "while", "repeat")
            and ts[i].kind == "identifier"
        ):
            stack.append("until" if value == "repeat" else "end")
            if value in ("for", "while"):
                pending_do += 1
        elif value == "do" and ts[i].kind == "identifier":
            if pending_do:
                pending_do -= 1
            else:
                stack.append("end")
        elif value in matching:
            stack.append(matching[value])
        elif value == stack[-1]:
            stack.pop()
            if not stack:
                if i > start:
                    args.append(ts[start:i])
                return args, i + 1
        elif value == "," and len(stack) == 1:
            args.append(ts[start:i])
            start = i + 1
    return [], begin + 1


def literal_type(arg):
    if not arg:
        return None
    if len(arg) == 1:
        t = arg[0]
        if t.kind in ("string", "number"):
            return t.kind
        if t.value in ("true", "false"):
            return "boolean"
        if t.value == "nil":
            return "nil"
    if arg[0].value == "{":
        return "table"
    if arg[0].value == "function":
        return "function"
    if len(arg) == 2 and arg[0].value == "-" and arg[1].kind == "number":
        return "number"
    return None


def check_source(source, api):
    ts = tokens(source)
    tables, components = api["tables"], api["components"]
    symbols = {name: name for name in tables if not name[0].isupper()}
    diagnostics, seen = [], set()

    def report(token, message):
        key = (token.line, message)
        if key not in seen:
            diagnostics.append((token.line, message))
            seen.add(key)

    def expression(i):
        if i >= len(ts):
            return None, i
        typ = symbols.get(ts[i].value)
        i += 1
        while (
            typ
            and i + 1 < len(ts)
            and ts[i].value in (".", ":")
            and ts[i + 1].kind == "identifier"
        ):
            separator = ts[i].value
            member = ts[i + 1]
            i += 2
            typ = typ.removesuffix("|nil")
            if typ.startswith(("component:", "mutable:", "get:")):
                if (
                    typ.startswith("mutable:")
                    and member.value == "commit"
                    and i < len(ts)
                    and ts[i].value == "("
                ):
                    args, i = arguments(ts, i)
                    if args:
                        report(member, "component.commit expects no arguments")
                    continue
                name = typ.split(":", 1)[1]
                fields = (
                    get_fields(api, name)
                    if typ.startswith("get:")
                    else components[name]
                )
                if member.value not in fields:
                    report(member, f"unknown component field '{member.value}'")
                    return None, i
                typ = field_type(fields[member.value]["kind"])
                continue
            if typ not in tables:
                return None, i
            if member.value not in tables[typ]:
                report(member, f"unknown {typ} member '{member.value}'")
                return None, i
            if member.value in PROPERTIES.get(typ, {}):
                typ = PROPERTIES[typ][member.value]
                continue
            if i >= len(ts) or ts[i].value != "(":
                return None, i
            expected_separator = ":" if typ[0].isupper() else "."
            if separator != expected_separator:
                report(
                    member, f"use '{expected_separator}' to call {typ}.{member.value}"
                )
            args, end = arguments(ts, i)
            params, returns = signature(typ, member.value)
            if not any(name == "..." for name, _ in params):
                required = sum(not name.endswith("?") for name, _ in params)
                # A final function call or vararg may supply multiple arguments.
                expands = bool(
                    args
                    and (
                        any(t.value == "..." for t in args[-1])
                        or args[-1][-1].value == ")"
                    )
                )
                if len(args) > len(params) or (len(args) < required and not expands):
                    report(
                        member,
                        f"{typ}.{member.value} expects {required}..{len(params)} arguments, got {len(args)}",
                    )
                for arg, (name, expected) in zip(args, params):
                    actual = literal_type(arg)
                    accepted = expected.replace("integer", "number").split("|")
                    if actual == "function" and any(
                        kind in CALLABLE_TYPES for kind in accepted
                    ):
                        continue
                    if (
                        actual
                        and "any" not in accepted
                        and actual not in accepted
                        and not (actual == "nil" and name.endswith("?"))
                    ):
                        if not (
                            actual == "table"
                            and any(
                                t[0].isupper() and t not in CALLABLE_TYPES
                                for t in accepted
                            )
                        ):
                            report(
                                arg[0],
                                f"{typ}.{member.value} argument '{name.rstrip('?')}' expects {expected}, got {actual}",
                            )
            component_index = 1 if typ in ("KinCommandBuffer", "t") else 0
            if (
                typ in ("Entity", "KinEntity", "KinCommandBuffer", "t")
                and member.value
                in ("get", "component", "has", "set", "patch", "add", "remove")
                and len(args) > component_index
            ):
                name_arg = args[component_index]
                if len(name_arg) == 1 and name_arg[0].kind == "string":
                    name = name_arg[0].value
                    if typ == "KinEntity" and member.value in ("get", "has", "remove"):
                        if name in ("script_owned", "ScriptOwned"):
                            typ, i = "boolean", end
                            continue
                        name = {
                            "transform": "Transform2D",
                            "rect": "RectRenderer",
                            "line": "LineRenderer",
                        }.get(name, name)
                    if name not in components:
                        report(name_arg[0], f"unknown component '{name}'")
                    elif member.value in ("get", "component"):
                        returns = (
                            "get:"
                            if member.value == "get" and typ == "KinEntity"
                            else (
                                "mutable:"
                                if member.value == "component"
                                else "component:"
                            )
                        ) + name
                    elif (
                        member.value in ("set", "patch")
                        and len(args) > component_index + 1
                    ):
                        fields = args[component_index + 1]
                        depth = 0
                        for k, token in enumerate(fields):
                            if token.value in ("{", "(", "["):
                                depth += 1
                            elif token.value in ("}", ")", "]"):
                                depth -= 1
                            if (
                                depth == 1
                                and token.kind == "identifier"
                                and k + 1 < len(fields)
                                and fields[k + 1].value == "="
                            ):
                                if token.value not in components[name]:
                                    report(
                                        token, f"unknown {name} field '{token.value}'"
                                    )
                                elif components[name][token.value]["readonly"]:
                                    report(token, f"{name}.{token.value} is read-only")
                                else:
                                    actual = literal_type(fields[k + 2 : k + 3])
                                    expected = field_type(
                                        components[name][token.value]["kind"]
                                    )
                                    if (
                                        actual
                                        and expected
                                        in ("boolean", "string", "number", "integer")
                                        and actual
                                        != expected.replace("integer", "number")
                                    ):
                                        report(
                                            token,
                                            f"{name}.{token.value} expects {expected}, got {actual}",
                                        )
            typ, i = returns.split(",", 1)[0].strip(), end
        return typ, i

    scopes, pending_do = [], 0
    for i, token in enumerate(ts):
        value = token.value
        if token.kind != "identifier":
            continue
        if value == "function":
            scopes.append(("function", symbols.copy()))
            name = ts[i + 1].value if i + 1 < len(ts) else ""
            begin = i + 1
            while begin < len(ts) and ts[begin].value != "(":
                begin += 1
            if begin < len(ts):
                args, _ = arguments(ts, begin)
                for arg in args:
                    if arg:
                        symbols[arg[0].value] = None
                callback = {
                    "on_load": "KinScene",
                    "on_reload": None,
                    "on_enter": "KinContext",
                    "on_exit": "KinContext",
                    "update": "KinContext",
                }
                if api["host"] == "entity":
                    callback = {
                        name: "Entity"
                        for name in (
                            "on_start",
                            "on_update",
                            "on_contact",
                            "on_contact_end",
                            "on_stop",
                            "on_reload",
                            "on_load",
                            "on_dialogue_event",
                        )
                    }
                if args and args[0] and callback.get(name):
                    symbols[args[0][0].value] = callback[name]
                if api["host"] == "scene" and name == "on_reload" and len(args) > 1:
                    symbols[args[1][0].value] = "KinScene"
        elif value in ("if", "for", "while", "repeat"):
            scopes.append((value, symbols.copy()))
            if value in ("for", "while"):
                pending_do += 1
        elif value == "do":
            if pending_do:
                pending_do -= 1
            else:
                scopes.append((value, symbols.copy()))
        elif value in ("end", "until") and scopes:
            _, symbols = scopes.pop()
        elif (
            value == "local"
            and i + 2 < len(ts)
            and ts[i + 1].kind == "identifier"
            and ts[i + 2].value == "="
        ):
            typ, _ = expression(i + 3)
            symbols[ts[i + 1].value] = typ
        elif value in symbols and not (
            i and ts[i - 1].value in (".", ":", "function", "local")
        ):
            expression(i)
    return sorted(diagnostics)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--native", type=Path, required=True, help="built kin_lua executable"
    )
    parser.add_argument("--host", choices=("scene", "entity", "test"), default="scene")
    parser.add_argument(
        "--components",
        type=Path,
        action="append",
        default=[],
        help="additional project component schema",
    )
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate")
    gen.add_argument("--output", type=Path, required=True)
    check = sub.add_parser("check")
    check.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()
    api = schema(args.native, args.host, args.components)
    if args.command == "generate":
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / "kin.lua").write_text(generate(api), encoding="utf-8")
        (args.output / "api.json").write_text(
            json.dumps(api, indent=2) + "\n", encoding="utf-8"
        )
        (args.output / ".luarc.json").write_text(
            json.dumps(
                {
                    "runtime.version": "Lua 5.4",
                    "workspace.library": [str(args.output.resolve())],
                    "workspace.checkThirdParty": False,
                },
                indent=2,
            )
            + "\n"
        )
        print(args.output / "kin.lua")
        return 0
    syntax = subprocess.run([str(args.native), "check", *map(str, args.files)])
    if syntax.returncode:
        return syntax.returncode
    errors = 0
    for file in args.files:
        for line, message in check_source(file.read_text(encoding="utf-8"), api):
            print(f"{file}:{line}: {message}", file=sys.stderr)
            errors += 1
    return int(errors > 0)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(error.stderr or str(error), file=sys.stderr)
        raise SystemExit(2)
    except (OSError, UnicodeError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(2)
