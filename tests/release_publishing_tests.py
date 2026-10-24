#!/usr/bin/env python3
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_release import collect, check_tag


class ReleasePublishingTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.incoming = self.root/'incoming'
        self.output = self.root/'publish'
        self.revision = 'a'*40
        self.version = '0.2.6'
        self.packages = {}
        for system in ('Linux', 'Windows'):
            folder = self.incoming/('desktop-v1-'+system)
            packages = folder/'packages'
            packages.mkdir(parents=True)
            package = packages/f'signal_siege-{self.version}-{system.lower()}-x86_64.zip'
            with zipfile.ZipFile(package, 'w') as archive:
                archive.writestr('game/player', system.encode())
            sha = hashlib.sha256(package.read_bytes()).hexdigest()
            identity = {'package_sha256': sha, 'source_revision': self.revision,
                        'source_dirty': 'false', 'version': self.version, 'system': system,
                        'target': 'signal_siege', 'contract': 'desktop-v1'}
            for kind in ('release', 'components', 'assets', 'cook', 'size'):
                self.write(package.with_suffix('.'+kind+'.json'), identity)
            with zipfile.ZipFile(package.with_name(package.stem+'-symbols.zip'), 'w') as archive:
                archive.writestr('symbols/manifest.json', json.dumps({'package_sha256': sha}))
            proof = {'schema': 'kin.release-acceptance/1', 'release': identity,
                     'package_sha256': sha, 'backend': 'software', 'automated_gate_passed': True,
                     'smoke_only': False, 'read_only_enforced': True, 'settings_roundtrip': True,
                     'runs': [{'status': 'ok', 'locale': locale}
                              for locale in ('en', 'en', 'en', 'fr', 'ja', 'ar')]}
            self.write(folder/'acceptance/acceptance.json', proof)
            if system == 'Linux':
                self.write(packages/'release-audit.json', {'passed': True, 'package_sha256': sha})
                for backend in ('software', 'gpu'):
                    value = copy.deepcopy(proof)
                    value['backend'] = backend
                    self.write(self.incoming/f'desktop-v1-clean-Linux-{backend}/acceptance.json', value)
            else:
                self.write(self.incoming/'desktop-v1-fresh-Windows/acceptance.json', proof)
            self.packages[system] = package

    def write(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data), encoding='utf-8')

    def collect(self):
        return collect(self.incoming, self.output, self.version, self.revision)

    def test_collects_exact_packages_and_checksum_inventory(self):
        manifest = self.collect()
        for system, source in self.packages.items():
            self.assertEqual(source.read_bytes(), (self.output/source.name).read_bytes())
            self.assertEqual(manifest['packages'][system]['file'], source.name)
        for line in (self.output/'SHA256SUMS').read_text(encoding='utf-8').splitlines():
            sha, name = line.split('  ', 1)
            self.assertEqual(sha, hashlib.sha256((self.output/name).read_bytes()).hexdigest())
        self.assertTrue((self.output/'release-manifest.json').is_file())
        with self.assertRaisesRegex(ValueError, 'empty'):
            self.collect()

    def test_rejects_changed_archive(self):
        with self.packages['Linux'].open('ab') as stream:
            stream.write(b'changed')
        with self.assertRaisesRegex(ValueError, 'mismatch'):
            self.collect()
        self.assertFalse(self.output.exists())

    def test_requires_clean_runtime_gpu_and_windows_proofs(self):
        for folder in ('desktop-v1-clean-Linux-gpu', 'desktop-v1-fresh-Windows'):
            path = self.incoming/folder/'acceptance.json'
            original = path.read_bytes()
            path.unlink()
            with self.assertRaises(FileNotFoundError):
                self.collect()
            path.write_bytes(original)
        self.assertFalse(self.output.exists())

    def test_rejects_smoke_only_wrong_revision_and_failed_scenarios(self):
        path = self.incoming/'desktop-v1-clean-Linux-gpu/acceptance.json'
        original = json.loads(path.read_text(encoding='utf-8'))
        for key, value in [('smoke_only', True), ('read_only_enforced', False),
                           ('automated_gate_passed', False), ('package_sha256', 'wrong')]:
            self.write(path, {**original, key: value})
            with self.assertRaises(ValueError):
                self.collect()
        value = copy.deepcopy(original)
        value['release']['source_revision'] = 'b'*40
        self.write(path, value)
        with self.assertRaises(ValueError):
            self.collect()
        value = copy.deepcopy(original)
        value['runs'][0]['status'] = 'failed'
        self.write(path, value)
        with self.assertRaises(ValueError):
            self.collect()
        self.assertFalse(self.output.exists())

    def test_rejects_wrong_symbol_binding(self):
        package = self.packages['Windows']
        with zipfile.ZipFile(package.with_name(package.stem+'-symbols.zip'), 'w') as archive:
            archive.writestr('symbols/manifest.json', json.dumps({'package_sha256': 'wrong'}))
        with self.assertRaisesRegex(ValueError, 'symbols'):
            self.collect()

    def test_tag_must_match_project_and_revision(self):
        import prepare_release
        project = self.root/'project'
        project.mkdir()
        (project/'CMakeLists.txt').write_text('project(kin VERSION 0.2.6 LANGUAGES CXX)', encoding='utf-8')
        with patch.object(prepare_release, 'ROOT', project), patch.object(
                prepare_release.subprocess, 'check_output', return_value=self.revision+'\n'):
            self.assertEqual(check_tag('v0.2.6', self.revision), '0.2.6')
            with self.assertRaises(ValueError):
                check_tag('main', self.revision)
            with self.assertRaises(ValueError):
                check_tag('v0.2.6', 'b'*40)


if __name__ == '__main__':
    unittest.main()
