#!/usr/bin/env python3
"""Audit a declared asset graph without cooking, deleting or excluding files."""
import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import unicodedata
import sys

sys.dont_write_bytecode = True
from asset_dependencies import discover, reference


class ManifestError(ValueError):
    pass


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ManifestError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def asset_path(value):
    if (not isinstance(value, str) or not value or "\\" in value or ":" in value
            or any(ord(char) < 32 for char in value)
            or any(part in ("", ".", "..") for part in value.split("/"))):
        raise ManifestError(f"Expected a normalized relative asset path: {value!r}")
    return value


def fields(value, allowed, where):
    if not isinstance(value, dict) or set(value) - set(allowed):
        raise ManifestError(f"Invalid or unknown fields in {where}")


def paths(value, where):
    if not isinstance(value, list):
        raise ManifestError(f"{where} must be an array of relative paths")
    result = [asset_path(path) for path in value]
    if len(result) != len(set(result)):
        raise ManifestError(f"Duplicate paths in {where}")
    return sorted(result)


def parse_manifest(data):
    manifest = json.loads(data, object_pairs_hook=unique_object)
    fields(manifest, ("schema", "roots", "roots_complete", "dynamic_groups", "assets", "discover_dependencies"), "manifest")
    if manifest.get("schema") != "kin.asset-manifest/1":
        raise ManifestError("Expected schema kin.asset-manifest/1")
    if type(manifest.get("roots_complete", False)) is not bool:
        raise ManifestError("roots_complete must be boolean")
    if type(manifest.get("discover_dependencies", False)) is not bool:
        raise ManifestError("discover_dependencies must be boolean")
    roots = paths(manifest.get("roots", []), "roots")
    groups = manifest.get("dynamic_groups", {})
    if not isinstance(groups, dict) or any(not name for name in groups):
        raise ManifestError("dynamic_groups must map nonempty names to path arrays")
    groups = {name: paths(value, f"dynamic group {name}") for name, value in sorted(groups.items())}
    assets = manifest.get("assets", {})
    if not isinstance(assets, dict):
        raise ManifestError("assets must map relative paths to dependency declarations")
    nodes = {}
    for path, node in sorted(assets.items()):
        asset_path(path)
        fields(node, ("dependencies", "dependencies_complete"), path)
        if type(node.get("dependencies_complete", False)) is not bool:
            raise ManifestError(f"dependencies_complete must be boolean: {path}")
        nodes[path] = {"dependencies": paths(node.get("dependencies", []), path),
                       "dependencies_complete": node.get("dependencies_complete", False),
                       "completeness_declared": "dependencies_complete" in node}
    return roots, manifest.get("roots_complete", False), groups, nodes, manifest.get("discover_dependencies", False)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def measure(path):
    digest, size = hashlib.sha256(), 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
    return size, digest.hexdigest()


def analyze(root, manifest_file):
    diagnostics = []

    def diagnostic(severity, code, path, message):
        diagnostics.append(dict(severity=severity, code=code, path=path, message=message))

    inventory = {}
    portable = {}
    if not root.is_dir():
        raise ManifestError(f"Asset directory does not exist: {root}")
    # Directory links are never traversed. File links are also rejected: the
    # report must measure the same self-contained tree on every build machine.
    def walk_error(error):
        diagnostic("error", "unreadable-directory", "", str(error))

    for directory, directories, files in os.walk(root, followlinks=False, onerror=walk_error):
        for name in sorted(directories + files):
            entry = Path(directory) / name
            relative = entry.relative_to(root).as_posix()
            try:
                asset_path(relative)
                key = unicodedata.normalize("NFC", relative).casefold()
                if key in portable:
                    diagnostic("error", "path-collision", relative, f"Collides with {portable[key]}")
                portable[key] = relative
                if entry.is_symlink():
                    diagnostic("error", "symbolic-link", relative, "Symlink assets are not audited; use regular files")
                elif entry.is_file():
                    size, digest = measure(entry)
                    inventory[relative] = {"path": relative, "bytes": size, "sha256": digest}
                elif not entry.is_dir():
                    diagnostic("error", "special-file", relative, "Expected a regular asset file")
            except (OSError, ManifestError) as error:
                diagnostic("error", "unreadable-or-invalid-path", relative, str(error))
        directories[:] = sorted(name for name in directories if not (Path(directory) / name).is_symlink())

    roots, roots_complete, groups, nodes, discovery = [], False, {}, {}, False
    manifest_hash = None
    if manifest_file:
        try:
            data = manifest_file.read_bytes()
            manifest_hash = sha256(data)
            roots, roots_complete, groups, nodes, discovery = parse_manifest(data)
        except (OSError, ValueError) as error:
            diagnostic("error", "invalid-manifest", "", str(error))
    else:
        diagnostic("warning", "no-manifest", "", "No declarations; every file remains uncertain and kept")
    edges = [{"source": path, "target": target, "origin": "manifest", "location": "dependencies"}
             for path, node in sorted(nodes.items()) for target in node["dependencies"]]
    scans = {}
    if discovery:
        for path in sorted(inventory):
            try:
                scanned = discover(root, path, inventory, unique_object)
                if scanned is None:
                    continue
                scans[path] = {key: scanned[key] for key in ("format", "notes")}
                node = nodes.setdefault(path, {"dependencies": [], "completeness_declared": False})
                node["dependencies"] = sorted(set(node["dependencies"]) | {edge["target"] for edge in scanned["edges"]})
                if not node["completeness_declared"]:
                    node["dependencies_complete"] = scanned["complete"]
                edges.extend(scanned["edges"])
            except (OSError, ValueError) as error:
                diagnostic("error", "dependency-discovery", path, str(error))
    starts = set(roots)
    reasons = {path: {"entry-root"} for path in roots}
    for name, members in groups.items():
        for path in members:
            starts.add(path)
            reasons.setdefault(path, set()).add(f"dynamic-group:{name}")
    root_sources = {path: sorted(reasons[path]) for path in sorted(starts)}
    if discovery:
        for path in sorted(starts):
            # Explicit roots also keep all existing localized variants. They
            # remain dependency edges of the root, not independently guessed roots.
            variants = [target for target in reference(path, path, inventory) if target != path]
            if variants:
                node = nodes.setdefault(path, {"dependencies": [], "dependencies_complete": False})
                node["dependencies"] = sorted(set(node["dependencies"]) | set(variants))
                edges.extend({"source": path, "target": target, "origin": "localization", "location": "root"}
                             for target in variants)
    declared = set(nodes) | starts
    for node in nodes.values():
        declared.update(node["dependencies"])
    for path in sorted(declared - set(inventory)):
        diagnostic("error", "missing-file", path, "Declared asset/root/dependency does not exist")
    reachable = set()
    queue = deque(sorted(starts))
    chains = {path: [path] for path in starts}
    while queue:
        path = queue.popleft()
        if path in reachable:
            continue
        reachable.add(path)
        for dependency in nodes.get(path, {}).get("dependencies", []):
            reasons.setdefault(dependency, set()).add(f"dependency:{path}")
            if dependency not in chains:
                chains[dependency] = chains[path] + [dependency]
            queue.append(dependency)
    incomplete = sorted(path for path in reachable
                        if not nodes.get(path, {}).get("dependencies_complete", False))
    if not roots_complete:
        diagnostic("warning", "incomplete-roots", "", "Direct and dynamic load roots have not been declared complete")
    for path in incomplete:
        diagnostic("warning", "incomplete-dependencies", path, "Reachable dependencies are undeclared or incomplete")
    unlisted = sorted(set(inventory) - set(nodes))
    for path in unlisted:
        diagnostic("warning", "unlisted-file", path, "Unknown content may load other assets; candidate classification is blocked")
    valid = not any(item["severity"] == "error" for item in diagnostics)
    complete = valid and roots_complete and not incomplete and not unlisted
    totals = {status: {"files": 0, "bytes": 0} for status in ("reachable", "unused_candidate", "uncertain")}
    entries = []
    for path, entry in sorted(inventory.items()):
        why = sorted(reasons.get(path, set()))
        if path in reachable:
            status = "reachable"
        elif path not in nodes:
            status, why = "uncertain", ["unlisted-file"]
        elif complete:
            status, why = "unused_candidate", ["unreachable-in-declared-graph"]
        else:
            status, why = "uncertain", ["invalid-or-incomplete-graph"]
        entry.update(status=status, reasons=why, dependencies=nodes.get(path, {}).get("dependencies", []),
                     root_chain=chains.get(path, []),
                     dependencies_complete=nodes.get(path, {}).get("dependencies_complete", False),
                     completeness_source=("manifest" if nodes.get(path, {}).get("completeness_declared")
                                          else "scanner" if path in scans else "unknown"))
        entries.append(entry)
        totals[status]["files"] += 1
        totals[status]["bytes"] += entry["bytes"]
    fingerprint = [{key: entry[key] for key in ("path", "bytes", "sha256")} for entry in entries]
    return {"schema": "kin.asset-report/1", "mode": "dry-run", "pruning_applied": False,
            "valid": valid, "roots_complete": roots_complete, "reachability_complete": complete,
            "manifest_sha256": manifest_hash,
            "inventory_sha256": sha256(json.dumps(fingerprint, sort_keys=True).encode()),
            "roots": roots, "dynamic_groups": groups, "incomplete_dependencies": incomplete, "unlisted_files": unlisted,
            "root_sources": root_sources, "dependency_discovery": discovery, "scanned_assets": scans,
            "dependency_edges": sorted(edges, key=lambda edge: (edge["source"], edge["target"], edge["origin"], edge["location"])),
            "totals": {"files": len(entries), "bytes": sum(entry["bytes"] for entry in entries), **totals},
            "files": entries, "diagnostics": sorted(diagnostics, key=lambda item: (item["severity"], item["code"], item["path"]))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = args.assets.resolve()
    output = args.output.resolve()
    # Reports inside the tree would become assets on the next audit/copy.
    if output == root or root in output.parents:
        parser.error("Write the report outside the asset directory")
    if args.manifest and output == args.manifest.resolve():
        parser.error("The report must not overwrite its input manifest")
    try:
        report = analyze(root, args.manifest)
    except (OSError, ManifestError) as error:
        raise SystemExit(str(error))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    totals = report["totals"]
    print(f"Assets: {totals['files']} files / {totals['bytes']} bytes; "
          f"reachable={totals['reachable']['bytes']}, candidates={totals['unused_candidate']['bytes']}, "
          f"uncertain={totals['uncertain']['bytes']}; all files kept")
    for item in report["diagnostics"]:
        print(f"{item['severity']}: {item['code']}: {item['path']}: {item['message']}")
    raise SystemExit(0 if report["valid"] else 1)


if __name__ == "__main__":
    main()
