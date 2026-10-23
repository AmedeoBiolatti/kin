#!/usr/bin/env python3
"""Verify graph closure, conservative classification and audit-only behavior."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.dont_write_bytecode = True
sys.path.insert(0, str(TOOLS))
from make_asset_report_fixture import create_fixture, create_dependency_fixture
from report_game_assets import analyze


class AssetReportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kin asset report ")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.assets, self.manifest = create_fixture(self.directory)

    def edit(self, change):
        data = json.loads(self.manifest.read_text())
        change(data)
        self.manifest.write_text(json.dumps(data))

    def report(self):
        return analyze(self.assets, self.manifest)

    def test_transitive_dependencies_dynamic_groups_and_real_bytes(self):
        report = self.report()
        self.assertTrue(report["valid"])
        self.assertTrue(report["reachability_complete"])
        self.assertFalse(report["pruning_applied"])
        entries = {entry["path"]: entry for entry in report["files"]}
        self.assertEqual(entries["textures/hero.png"]["status"], "reachable")
        self.assertIn("dependency:catalogs/hero.json", entries["textures/hero.png"]["reasons"])
        for name in ("en", "fr", "ja", "ar"):
            self.assertEqual(entries[f"locales/{name}.kinlang"]["status"], "reachable")
        self.assertEqual(entries["seasonal/winter.bin"]["status"], "reachable")
        self.assertEqual({path for path, item in entries.items() if item["status"] == "unused_candidate"},
                         {"unused/old-level.png", "unused/old-music.wav"})
        actual = sum(path.stat().st_size for path in self.assets.rglob("*") if path.is_file())
        self.assertEqual(actual, report["totals"]["bytes"])
        self.assertEqual(actual, sum(report["totals"][key]["bytes"] for key in
                                    ("reachable", "unused_candidate", "uncertain")))

    def test_unknown_file_blocks_candidates(self):
        (self.assets / "plugin.custom").write_bytes(b"could load unused/old-level.png")
        report = self.report()
        self.assertTrue(report["valid"])
        self.assertFalse(report["reachability_complete"])
        self.assertEqual(report["totals"]["unused_candidate"]["bytes"], 0)
        self.assertIn("plugin.custom", report["unlisted_files"])

    def test_incomplete_roots_and_reachable_dependencies(self):
        for change in (lambda data: data.update(roots_complete=False),
                       lambda data: data["assets"]["catalogs/hero.json"].update(dependencies_complete=False),
                       lambda data: data["assets"].pop("textures/hero.png")):
            with self.subTest(change=change):
                original = self.manifest.read_bytes()
                self.edit(change)
                report = self.report()
                self.assertTrue(report["valid"])
                self.assertEqual(report["totals"]["unused_candidate"]["files"], 0)
                self.manifest.write_bytes(original)

    def test_missing_files_block_candidates_even_when_unreachable(self):
        for path in ("audio/play.wav", "unused/old-level.png"):
            with self.subTest(path=path):
                data = (self.assets / path).read_bytes()
                (self.assets / path).unlink()
                report = self.report()
                self.assertFalse(report["valid"])
                self.assertEqual(report["totals"]["unused_candidate"]["files"], 0)
                self.assertTrue(any(item["code"] == "missing-file" and item["path"] == path
                                    for item in report["diagnostics"]))
                (self.assets / path).write_bytes(data)

    def test_cycles_are_finite_and_reachable(self):
        self.edit(lambda data: data["assets"]["textures/hero.png"].update(dependencies=["scenes/main.json"]))
        report = self.report()
        self.assertTrue(report["reachability_complete"])
        self.assertEqual(report["totals"]["reachable"]["files"], 9)

    def test_no_manifest_keeps_everything_uncertain(self):
        report = analyze(self.assets, None)
        self.assertTrue(report["valid"])
        self.assertEqual(report["totals"]["uncertain"]["files"], report["totals"]["files"])

    def test_bad_paths_types_duplicate_keys_and_typos(self):
        original = self.manifest.read_bytes()
        for path in ("../escape.png", "/absolute.png", "C:/absolute.png", "dir\\file.png", "a//b", "a/./b"):
            with self.subTest(path=path):
                self.edit(lambda data: data.update(roots=[path]))
                self.assertFalse(self.report()["valid"])
                self.manifest.write_bytes(original)
        for text in ('{"schema":"kin.asset-manifest/1","roots":[],"roots":[]}',
                     '{"schema":"kin.asset-manifest/1","roots_complete":"true"}',
                     '{"schema":"kin.asset-manifest/1","assets":{"a":{"dependences":[]}}}',
                     '{"schema":"kin.asset-manifest/1","roots":"file"}'):
            self.manifest.write_text(text)
            self.assertFalse(self.report()["valid"])

    def test_case_collision_and_symlinks(self):
        (self.assets / "DUPLICATE.bin").write_bytes(b"a")
        (self.assets / "duplicate.bin").write_bytes(b"b")
        if not (self.assets / "DUPLICATE.bin").samefile(self.assets / "duplicate.bin"):
            self.assertFalse(self.report()["valid"])
        (self.assets / "DUPLICATE.bin").unlink(missing_ok=True)
        (self.assets / "duplicate.bin").unlink(missing_ok=True)
        link = self.assets / "linked.bin"
        try:
            link.symlink_to(self.directory / "assets.manifest.json")
        except OSError:
            self.skipTest("Symlinks require privileges on this platform")
        self.assertFalse(self.report()["valid"])

    def test_cli_determinism_no_mutation_and_invalid_exit(self):
        before = {path.relative_to(self.assets).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in self.assets.rglob("*") if path.is_file()}
        output = self.directory / "report.json"
        command = [sys.executable, str(TOOLS / "report_game_assets.py"), "--assets", str(self.assets),
                   "--manifest", str(self.manifest), "--output", str(output)]
        subprocess.run(command, check=True, capture_output=True)
        first = output.read_bytes()
        subprocess.run(command, check=True, capture_output=True)
        self.assertEqual(first, output.read_bytes())
        after = {path.relative_to(self.assets).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in self.assets.rglob("*") if path.is_file()}
        self.assertEqual(before, after)
        inside = command[:-1] + [str(self.assets / "report.json")]
        self.assertNotEqual(subprocess.run(inside, capture_output=True).returncode, 0)
        self.assertFalse((self.assets / "report.json").exists())
        manifest_bytes = self.manifest.read_bytes()
        overwrite = command[:-1] + [str(self.manifest)]
        self.assertNotEqual(subprocess.run(overwrite, capture_output=True).returncode, 0)
        self.assertEqual(manifest_bytes, self.manifest.read_bytes())
        self.edit(lambda data: data.update(roots=["missing.png"]))
        self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
        self.assertFalse(json.loads(output.read_text())["valid"])


class DependencyTrackingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kin dependency tracking ")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.assets, self.manifest = create_dependency_fixture(self.directory)

    def report(self):
        return analyze(self.assets, self.manifest)

    def edit(self, change):
        data = json.loads(self.manifest.read_text())
        change(data)
        self.manifest.write_text(json.dumps(data))

    def test_real_formats_transitive_edges_and_explicit_roots(self):
        report = self.report()
        self.assertTrue(report["reachability_complete"], report["diagnostics"])
        entries = {entry["path"]: entry for entry in report["files"]}
        self.assertEqual(entries["textures/hero.png"]["root_chain"], ["scenes/main.kinscene",
            "scripts/main.lua", "catalogs/hero.kinsprites", "textures/hero.png"])
        self.assertEqual(entries["audio/play.wav"]["root_chain"], ["audio/main.kinaudio", "audio/play.wav"])
        self.assertEqual(entries["prefabs/hero.kinprefab"]["status"], "reachable")
        self.assertIn("dynamic-group:seasonal", report["root_sources"]["seasonal/winter.bin"])
        self.assertNotIn("catalogs/hero.kinsprites", report["root_sources"])
        edge = next(edge for edge in report["dependency_edges"] if edge["source"] == "catalogs/hero.kinsprites"
                    and edge["target"] == "textures/hero.png")
        self.assertEqual((edge["origin"], edge["location"]), ("scanner:kinsprites", "line:1"))
        self.assertEqual(entries["catalogs/hero.kinsprites"]["completeness_source"], "scanner")
        self.assertEqual(entries["scripts/main.lua"]["completeness_source"], "manifest")

    def test_all_localized_texture_and_catalog_relative_audio_variants_kept(self):
        entries = {entry["path"]: entry for entry in self.report()["files"]}
        self.assertEqual(entries["l10n/fr/textures/hero.png"]["status"], "reachable")
        self.assertEqual(entries["audio/l10n/ja/play.wav"]["status"], "reachable")
        self.assertEqual(entries["audio/l10n/ja/play.wav"]["root_chain"],
                         ["audio/main.kinaudio", "audio/l10n/ja/play.wav"])

    def test_scanning_does_not_promote_unrooted_scene(self):
        (self.assets / "scenes/unused.kinscene").write_text('{"schema":"kin.scene/1"}')
        entries = {entry["path"]: entry for entry in self.report()["files"]}
        self.assertEqual(entries["scenes/unused.kinscene"]["status"], "unused_candidate")
        self.assertEqual(entries["scenes/unused.kinscene"]["root_chain"], [])

    def test_removed_root_is_not_inferred_from_discovery(self):
        self.edit(lambda data: data.update(roots=["scenes/main.kinscene"]))
        entries = {entry["path"]: entry for entry in self.report()["files"]}
        self.assertEqual(entries["audio/main.kinaudio"]["status"], "unused_candidate")
        self.assertEqual(entries["audio/play.wav"]["status"], "unused_candidate")

    def test_missing_discovered_reference_invalidates_graph(self):
        (self.assets / "audio/main.kinaudio").write_text("clip missing nowhere.wav\n")
        report = self.report()
        self.assertFalse(report["valid"])
        self.assertEqual(report["totals"]["unused_candidate"]["files"], 0)
        self.assertTrue(any(item["path"] == "audio/nowhere.wav" for item in report["diagnostics"]))

    def test_manifest_cannot_erase_discovered_dependencies(self):
        self.edit(lambda data: data["assets"].update({"catalogs/hero.kinsprites":
                  {"dependencies": [], "dependencies_complete": True}}))
        entries = {entry["path"]: entry for entry in self.report()["files"]}
        self.assertIn("textures/hero.png", entries["catalogs/hero.kinsprites"]["dependencies"])

    def test_scripts_custom_components_and_unknown_files_need_declarations(self):
        for path in ("scripts/main.lua", "scenes/main.kinscene", "prefabs/hero.kinprefab"):
            with self.subTest(path=path):
                original = self.manifest.read_bytes()
                self.edit(lambda data: data["assets"].pop(path))
                report = self.report()
                self.assertFalse(report["reachability_complete"])
                self.assertIn(path, report["incomplete_dependencies"])
                self.assertEqual(report["totals"]["unused_candidate"]["files"], 0)
                self.manifest.write_bytes(original)
        (self.assets / "extension.custom").write_text("anything")
        self.assertEqual(self.report()["totals"]["unused_candidate"]["files"], 0)

    def test_explicit_incomplete_overrides_catalog_inference(self):
        self.edit(lambda data: data["assets"].update({"catalogs/hero.kinsprites": {"dependencies_complete": False}}))
        self.assertIn("catalogs/hero.kinsprites", self.report()["incomplete_dependencies"])

    def test_invalid_formats_and_path_escapes_cannot_be_overridden(self):
        path = self.assets / "catalogs/hero.kinsprites"
        for text in ("texture hero ../../outside.png", "unknown hidden.png", "texture hero", "texture hero /outside.png"):
            with self.subTest(text=text):
                path.write_text(text)
                report = self.report()
                self.assertFalse(report["valid"])
                self.assertEqual(report["totals"]["unused_candidate"]["files"], 0)

    def test_catalog_parent_references_normalize_within_root(self):
        (self.assets / "audio/main.kinaudio").write_text("clip play ../audio/play.wav\n")
        report = self.report()
        self.assertTrue(report["valid"])
        node = next(item for item in report["files"] if item["path"] == "audio/main.kinaudio")
        self.assertIn("audio/play.wav", node["dependencies"])

    def test_catalog_edits_change_graph_without_manifest_edits(self):
        before = self.manifest.read_bytes()
        (self.assets / "catalogs/hero.kinsprites").write_text(
            "texture hero unused/old-level.png\nsprite hero hero 0 0 32 32\n")
        entries = {entry["path"]: entry for entry in self.report()["files"]}
        self.assertEqual(entries["unused/old-level.png"]["status"], "reachable")
        self.assertEqual(entries["textures/hero.png"]["status"], "unused_candidate")
        self.assertEqual(before, self.manifest.read_bytes())

    def test_existing_scene_prefab_fixture_and_determinism(self):
        root = TOOLS.parent / "tests/fixtures/scene_with_prefab"
        first = analyze(root / "assets", root / "assets.manifest.json")
        self.assertTrue(first["reachability_complete"], first["diagnostics"])
        self.assertEqual(first["totals"]["reachable"]["files"], 4)
        self.assertEqual(first, analyze(root / "assets", root / "assets.manifest.json"))


if __name__ == "__main__":
    unittest.main()
