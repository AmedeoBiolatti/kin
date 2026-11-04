"""Exercise author-facing generation, diagnostics and the isolated test runner."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NATIVE = Path(sys.argv[1]).resolve()
sys.argv = [sys.argv[0]]
sys.path.insert(0, str(ROOT / "tools"))
import kin_lua  # noqa: E402


class LuaToolingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scene = kin_lua.schema(NATIVE, "scene")
        cls.entity = kin_lua.schema(NATIVE, "entity")
        cls.test_api = kin_lua.schema(NATIVE, "test")
        cls.scenario_api = kin_lua.schema(NATIVE, "scenario")
        cls.hosts = (cls.scene, cls.entity, cls.test_api, cls.scenario_api)

    def diagnostics(self, code, host="scene"):
        api = self.scene if host == "scene" else self.entity
        return "\n".join(message for _, message in kin_lua.check_source(code, api))

    def test_all_bindings_documented_and_definitions_compile(self):
        # Every binding has a signature, and every signature a binding (in some
        # host): the documentation cannot drift from the build either way.
        bound = {
            (table, member)
            for api in self.hosts
            for table, members in api["tables"].items()
            for member in members
        } | {("_G", name) for api in self.hosts for name in api["functions"]}
        documented = {
            (table, member)
            for table, members in kin_lua.SIGNATURES.items()
            for member in members
        }
        self.assertEqual(sorted(bound - documented), [], "bindings without a signature")
        self.assertEqual(sorted(documented - bound), [], "signatures without a binding")
        for api in self.hosts:
            with tempfile.TemporaryDirectory() as directory:
                out = Path(directory) / "kin.lua"
                out.write_text(kin_lua.generate(api))
                subprocess.run([NATIVE, "check", out], check=True, capture_output=True)
                text = out.read_text()
                self.assertIn("---@class KinComponent_Transform2D", text)
                if api["host"] != "scenario":
                    self.assertIn("component: 'Transform2D'", text)
                    self.assertIn("---@param seconds number", text)
        scenario = kin_lua.generate(self.scenario_api)
        self.assertIn("function scenario(case) end", scenario)
        self.assertIn("function wait_until(condition) end", scenario)
        self.assertEqual(self.scene["component_aliases"]["transform"], "Transform2D")
        self.assertIn("KinContext", self.scene["properties"])
        self.assertEqual(self.entity["callbacks"]["on_update"], ["Entity", "number"])

    def test_scenarios_compile_to_automation_json(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "case.lua"
            file.write_text(
                "scenario{name='walk',steps={tick(2),call('input.action',{name='jump'}),"
                "wait_until{observe='scene.current',path='/state/x',value=1,within=4}}}"
            )
            result = subprocess.run(
                [NATIVE, "scenario", file], capture_output=True, text=True, timeout=10
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            case = json.loads(result.stdout)
            self.assertEqual(case["schema"], "kin.scenario/1")
            self.assertEqual(case["steps"][0], {"tick": 2})
            self.assertEqual(case["steps"][2]["wait"]["within"], 4)
            for code in (
                "test{name='old entry point',steps={}}",
                "scenario{name='bad',steps={function() end}}",
                "scenario{name='holes',steps={[1]={tick=1},[3]={tick=1}}}",
            ):
                file.write_text(code)
                result = subprocess.run(
                    [NATIVE, "scenario", file], capture_output=True, text=True, timeout=10
                )
                self.assertNotEqual(result.returncode, 0, code)

    def test_project_components_and_method_call_syntax(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "project.kincomponents"
            path.write_text(
                json.dumps(
                    {
                        "schema": "kin.components/1",
                        "components": [
                            {
                                "name": "Health",
                                "fields": [
                                    {"name": "hp", "kind": "i32", "default": 10},
                                    {
                                        "name": "locked",
                                        "kind": "bool",
                                        "default": False,
                                        "readonly": True,
                                    },
                                ],
                            }
                        ],
                    }
                )
            )
            api = kin_lua.schema(NATIVE, "entity", [path])
            self.assertIn("Health", api["components"])
            diagnostics = kin_lua.check_source(
                "function on_update(self,dt) self:set('Health',{locked=true}) end", api
            )
            self.assertTrue(any("read-only" in message for _, message in diagnostics))
            self.assertIn("KinComponent_Health", kin_lua.generate(api))
        self.assertIn(
            "use ':'",
            self.diagnostics("function on_load(scene) scene.entity('Hero') end"),
        )

    def test_member_arity_types_and_components(self):
        message = self.diagnostics(
            """
            function on_load(scene)
                local e = scene:entity('Hero')
                e:missing()
                e:set_transform('fast', 1)
                e:patch('Transform2D', {rotaton=2})
                local c = e:component('Transform2D')
                local p = c.misspelled
                e:get('DoesNotExist')
            end
            task.wait()
        """
        )
        for expected in (
            "unknown KinEntity member",
            "expects number, got string",
            "rotaton",
            "misspelled",
            "DoesNotExist",
            "got 0",
        ):
            self.assertIn(expected, message)

    def test_nested_tasks_comments_strings_and_shadowing(self):
        self.assertEqual(
            self.diagnostics(
                """
            -- task.wait('bad')
            local text = [=[scene:missing()]=]
            task.run(function()
                local x, y = 1, 2
                if x < y then task.wait(0.1) end
                for i=1,2 do task.frames(1) end
            end)
            local task = {}
            task.custom('anything')
            function custom(scene) scene:custom() end
        """
            ),
            "",
        )

    def test_entity_callbacks_and_readonly_fields(self):
        self.assertIn(
            "unknown Entity member",
            self.diagnostics(
                "function on_update(self,dt) self:missing() end", "entity"
            ),
        )
        self.assertEqual(
            self.diagnostics(
                "function on_update(self,dt) self:move(1,2) end", "entity"
            ),
            "",
        )
        self.assertEqual(
            self.diagnostics(
                """function on_load(scene)
            local e=scene:entity()
            local old=e:get('Transform2D')
            local x=old.x
            local current=e:component('Transform2D')
            local p=current.pos
            current:commit()
            local alias=e:get('transform')
            local x=alias.x
            e:has('script_owned')
        end"""
            ),
            "",
        )
        self.assertEqual(
            self.diagnostics(
                """function on_start(self)
            local id=self:tween('Transform2D.pos',{x=1,y=2},0.5,{done=function(self) end})
            self:stop_tween(id)
            local timer=self:every(0.5,function(self) end)
            self:cancel(timer)
        end""",
                "entity",
            ),
            "",
        )

    def test_example_and_isolation_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "result.json"
            junit = Path(directory) / "result.xml"
            result = subprocess.run(
                [
                    NATIVE,
                    "test",
                    "--root",
                    ROOT / "examples/lua_testing/content",
                    "--json",
                    report,
                    "--junit",
                    junit,
                    ROOT / "examples/lua_testing/tests/project.lua",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(report.read_text())
            self.assertEqual(data["failures"], 0)
            self.assertEqual(data["tests"], 6)
            self.assertEqual(ET.parse(junit).getroot().attrib["tests"], "6")
            self.assertTrue(all(case["profile"]["callbacks"] for case in data["cases"]))

    def test_failing_assertions_load_errors_and_duplicate_tests(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for code in (
                "test('bad',function() assert(false,'expected failure') end)",
                "test('same',function() end);test('same',function() end)",
                "function broken(",
            ):
                file = root / "test.lua"
                file.write_text(code)
                result = subprocess.run(
                    [NATIVE, "test", "--root", root, file],
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertGreater(json.loads(result.stdout)["failures"], 0)

    def test_fixture_paths_and_unhandled_script_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            file = root / "test.lua"
            file.write_text(
                """test('paths',function()
                t.raises(function() t.edit('../escape.lua','') end,'escape')
                t.edit('bad.lua','function on_update(self) error("unhandled") end')
                local e=t.spawn('Hero',{})
                t.attach(e,'bad.lua');t.step()
            end)"""
            )
            result = subprocess.run(
                [NATIVE, "test", "--root", root, file],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("unhandled", json.loads(result.stdout)["cases"][0]["error"])

    def test_syntax_cli_and_generation_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            subprocess.run(
                [
                    sys.executable,
                    ROOT / "tools/kin_lua.py",
                    "--native",
                    NATIVE,
                    "--host",
                    "entity",
                    "generate",
                    "--output",
                    out,
                ],
                check=True,
                capture_output=True,
            )
            self.assertEqual(
                json.loads((out / "api.json").read_text())["host"], "entity"
            )
            self.assertEqual(
                json.loads((out / ".luarc.json").read_text())["runtime.version"],
                "Lua 5.4",
            )
            file = out / "bad.lua"
            file.write_text(
                "-- 日本語 🚀\nfunction on_update(self,dt) self:move('fast',0) end",
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    sys.executable,
                    ROOT / "tools/kin_lua.py",
                    "--native",
                    NATIVE,
                    "--host",
                    "entity",
                    "check",
                    file,
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("expects number, got string", result.stderr)

    def test_saved_input_profiles_are_isolated_between_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            file = root / "test.lua"
            file.write_text(
                r"""
                test('write private profile',function()
                    t.edit('actor.lua', [[function on_start(self)
                        input_contexts.push('play','key fire Space\n')
                        assert(input_contexts.save('play','kin_lua_fixture_test'))
                    end]])
                    local actor=t.spawn('Actor',{})
                    t.attach(actor,'actor.lua');t.step()
                end)
                test('profile is absent in next fixture',function()
                    t.edit('actor.lua', [[function on_start(self)
                        input_contexts.push('play','key fire Space\n')
                        assert(not pcall(input_contexts.load,'play','kin_lua_fixture_test'))
                    end]])
                    local actor=t.spawn('Actor',{})
                    t.attach(actor,'actor.lua');t.step()
                end)
            """
            )
            result = subprocess.run(
                [NATIVE, "test", "--root", root, file],
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["tests"], 2)

    def test_unfinished_test_tasks_fail_the_case(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            file = root / "test.lua"
            file.write_text(
                "test('forgot to step',function() task.run(function() assert(false) end) end)"
            )
            result = subprocess.run(
                [NATIVE, "test", "--root", root, file],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn(
                "unfinished task", json.loads(result.stdout)["cases"][0]["error"]
            )


if __name__ == "__main__":
    unittest.main()
