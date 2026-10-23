#!/usr/bin/env python3
"""Component contracts tested without fetching or building third parties."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ComponentConfigTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='kin components ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.build = self.root / 'build'
        self.source.mkdir()

    def configure(self, extra='', options=(), success=True):
        (self.source/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.22)
project(components LANGUAGES CXX)
include("{ROOT}/cmake/kin_components.cmake")
{extra}
kin_generate_component_config("${{CMAKE_BINARY_DIR}}/generated")
add_executable(check "{ROOT}/tests/component_tests.cpp")
target_link_libraries(check PRIVATE kin::components)
''', encoding='utf-8')
        result = subprocess.run(['cmake','-S',str(self.source),'-B',str(self.build),*options],
                                capture_output=True,text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout+result.stderr)
        return result.stdout+result.stderr

    def manifest(self):
        return json.loads((self.build/'generated/components.json').read_text(encoding='utf-8'))

    def compile(self):
        result=subprocess.run(['cmake','--build',str(self.build),'--config','Release'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_defaults_and_generated_api(self):
        self.configure()
        manifest=self.manifest()
        self.assertEqual(manifest['schema'],'kin.components/1')
        self.assertEqual(len(manifest['components']),11)
        self.assertTrue(all(c['enabled'] for c in manifest['components'].values()))
        self.assertEqual(manifest['components']['audio']['tests'],['kin-audio','kin-audio-component'])
        self.compile()

    def test_off_and_reconfigure_deterministically(self):
        self.configure()
        before=(self.build/'generated/components.json').read_bytes()
        names=[c['option'] for c in self.manifest()['components'].values()]
        self.configure(options=[f'-D{name}=OFF' for name in names])
        self.assertFalse(any(c['enabled'] for c in self.manifest()['components'].values()))
        self.compile()
        self.configure(options=[f'-D{name}=ON' for name in names])
        self.assertEqual((self.build/'generated/components.json').read_bytes(),before)
        self.compile()

    def test_dependencies_do_not_silently_enable(self):
        extra='kin_declare_component(EXTRA DEFAULT ON DESCRIPTION "extra" REQUIRES AUDIO)'
        text=self.configure(extra, ['-DKIN_ENABLE_AUDIO=OFF'], success=False)
        self.assertIn('requires KIN_ENABLE_AUDIO=ON',text)
        self.configure(extra,['-DKIN_ENABLE_EXTRA=OFF'])
        self.assertFalse(self.manifest()['components']['audio']['enabled'])

    def test_game_requirements(self):
        text=self.configure('kin_require_components(my_game AUDIO)', ['-DKIN_ENABLE_AUDIO=OFF'],success=False)
        self.assertIn('my_game requires KIN_ENABLE_AUDIO=ON',text)
        text=self.configure('kin_require_components(my_game MISSING)',success=False)
        self.assertIn('unknown Kin component MISSING',text)

    def test_late_registration_rejected(self):
        text=self.configure('kin_generate_component_config("${CMAKE_BINARY_DIR}/first")\nkin_declare_component(LATE DEFAULT ON DESCRIPTION "late")',success=False)
        self.assertIn('before generating',text)

    def test_package_metadata(self):
        self.configure(options=['-DCMAKE_BUILD_TYPE=Release','-DKIN_ENABLE_AUDIO=OFF'])
        with (self.source/'CMakeLists.txt').open('a') as stream:
            stream.write(f'\nlist(APPEND CMAKE_MODULE_PATH "{ROOT}/cmake")\ninclude(kin_release)\ninclude(kin_packaging)\nkin_package_game(check VERSION 1.2.3)\n')
        import shutil
        git=shutil.which('git')
        if git:
            (self.source/'content.txt').write_text('original', encoding='utf-8')
            for command in ([git,'init'],[git,'add','.'],[git,'-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-m','fixture']):
                subprocess.run(command,cwd=self.source,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        result=subprocess.run(['cmake','-S',str(self.source),'-B',str(self.build)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        result=subprocess.run(['cmake','--build',str(self.build),'--config','Release','--target','check_package'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        import hashlib
        packages=self.build/'packages'
        report=json.loads(next(packages.glob('*.components.json')).read_text(encoding='utf-8'))
        self.assertFalse(report['components']['audio']['enabled'])
        self.assertEqual(report['package_sha256'],hashlib.sha256(next(packages.glob('*.zip')).read_bytes()).hexdigest())
        identity=json.loads(next(packages.glob('*.release.json')).read_text(encoding='utf-8'))
        self.assertEqual(identity['version'],'1.2.3')
        if git:
            self.assertEqual(identity['source_dirty'],'false')
            (self.source/'content.txt').write_text('changed after configuration', encoding='utf-8')
            subprocess.run(['cmake','-P',str(self.build/'package-check-Release.cmake')],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            exported=json.loads(next(packages.glob('*.release.json')).read_text(encoding='utf-8'))
            self.assertEqual(exported['source_revision'],identity['source_revision'])
            self.assertEqual(exported['source_dirty'],'true')

        self.assertIn('check-1.2.3-',next(packages.glob('*.zip')).name)
        size=json.loads(next(packages.glob('*.size.json')).read_text(encoding='utf-8'))
        self.assertEqual(size['components'],report['components'])
        import zipfile
        with zipfile.ZipFile(next(packages.glob('*.zip'))) as archive:
            self.assertFalse(any(name.endswith('components.json') for name in archive.namelist()))

    def test_forward_dependencies_and_invalid_registry(self):
        self.configure('''kin_declare_component(EXTRA DEFAULT ON DESCRIPTION "extra" REQUIRES LATER)
kin_declare_component(LATER DEFAULT ON DESCRIPTION "later")''')
        for extra, expected in [
            ('kin_declare_component(AUDIO DEFAULT ON DESCRIPTION "duplicate")','Duplicate'),
            ('kin_declare_component(EXTRA DEFAULT ON DESCRIPTION "extra" REQUIRES UNKNOWN)','unknown component'),
            ('''kin_declare_component(EXTRA DEFAULT OFF DESCRIPTION "extra" REQUIRES LATER)
kin_declare_component(LATER DEFAULT OFF DESCRIPTION "later" REQUIRES EXTRA)''','Cyclic')]:
            self.assertIn(expected,self.configure(extra,success=False))


if __name__=='__main__':
    unittest.main()
