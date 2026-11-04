from pathlib import Path
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("kin_dev", ROOT / "tools/kin_dev.py")
dev = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dev)


class LauncherTests(unittest.TestCase):
    def test_concurrent_import_waits_for_the_output_owner(self):
        spec = importlib.util.spec_from_file_location("kin_import_lock", ROOT / "tools/import_2d_assets.py")
        importer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(importer)
        output = self.root / "locked-output"
        with importer.output_lock(output):
            child = subprocess.Popen(
                [
                    sys.executable,
                    str(ROOT / "tools/import_2d_assets.py"),
                    str(self.manifest),
                    "--output",
                    str(output),
                    "--content",
                    str(self.content),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                with self.assertRaises(subprocess.TimeoutExpired):
                    child.wait(timeout=0.2)
            except BaseException:
                child.kill()
                child.communicate()
                raise
        stdout, stderr = child.communicate(timeout=10)
        self.assertEqual(child.returncode, 0, stdout + stderr)
        self.assertEqual((output / "game.lua").read_text(), "initial")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.content = self.root / "content"
        self.content.mkdir()
        (self.content / "game.lua").write_text("initial")
        self.manifest = self.root / "imports.json"
        self.manifest.write_text('{"schema":"kin.imports/1","imports":[]}')
        self.game = self.root / "game.py"
        self.game.write_text(
            'import json,sys;from pathlib import Path;Path(sys.argv[1]).write_text(json.dumps(sys.argv[2:]));sys.exit(3 if "--fail" in sys.argv else 0)'
        )
        self.value = {
            "schema": "kin.dev/1",
            "build": str(self.root),
            "target": "fixture",
            "command": [sys.executable, str(self.game), str(self.root / "argv.json")],
            "manifest": str(self.manifest),
            "content": str(self.content),
            "output": str(self.root / "output"),
        }

    def test_project_validation_and_relative_paths(self):
        path = self.root / "project.json"
        self.value["content"] = "content"
        path.write_text(json.dumps(self.value))
        self.assertEqual(dev.project(path)["content"], str(self.content))
        self.value["output"] = "content/generated"
        path.write_text(json.dumps(self.value))
        with self.assertRaises(ValueError):
            dev.project(path)
        self.value["schema"] = "unknown"
        path.write_text(json.dumps(self.value))
        with self.assertRaises(ValueError):
            dev.project(path)

    def test_real_import_launch_arguments_exit_and_cleanup(self):
        try:
            import PIL
        except ImportError:
            self.skipTest("Pillow is optional outside import workflows")
        processes = []
        popen = subprocess.Popen

        def start(*args, **kwargs):
            process = popen(*args, **kwargs)
            processes.append(process)
            return process

        with mock.patch.object(dev.subprocess, "Popen", side_effect=start):
            self.assertEqual(dev.run(self.value, build=False, game_args=["--frames=2"]), 0)
        self.assertTrue(all(process.poll() is not None for process in processes))
        args = json.loads((self.root / "argv.json").read_text())
        self.assertIn("--dev", args)
        self.assertIn("--frames=2", args)
        self.assertEqual((self.root / "output/game.lua").read_text(), "initial")
        self.assertFalse((self.root / "output/.kin-import-publishing").exists())
        self.assertEqual(dev.run(self.value, build=False, game_args=["--fail"]), 3)

    def test_initial_import_failure_does_not_launch_game(self):
        self.manifest.write_text("invalid")
        with self.assertRaises(subprocess.CalledProcessError):
            dev.run(self.value, build=False)
        self.assertFalse((self.root / "argv.json").exists())


if __name__ == "__main__":
    unittest.main()
