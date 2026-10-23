#!/usr/bin/env python3
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from cook_game_assets import cook, minify_json, recompress_png


def chunk(kind, payload):
    return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))


def png(extra=b''):
    raw = (b'\0' + b'\x10\x20\x30\xff' * 32) * 32
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB',32,32,8,6,0,0,0))
            + extra + chunk(b'IDAT', zlib.compress(raw, 0)) + chunk(b'IEND', b''))


class CookingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.assets = self.base / 'assets'
        self.assets.mkdir()
        (self.assets / 'en.kinlang').write_bytes(b'{ "n": 1.2300e+03, "text": "a \\" b", "u": "\\u00e9" }\n')
        (self.assets / 'image.png').write_bytes(png(chunk(b'tEXt', b'Author\0Kin')))
        (self.assets / 'sound.wav').write_bytes(b'unchanged audio')
        self.manifest = self.base / 'manifest.json'
        self.manifest.write_text(json.dumps({'schema':'kin.asset-manifest/1', 'roots_complete':True,
            'roots':['en.kinlang','image.png','sound.wav'], 'discover_dependencies':True}))
        self.options = dict(assets=self.assets, manifest=self.manifest, output=self.base/'cooked',
                            cache=self.base/'cache', report=self.base/'report.json')

    def test_lossless_cache_and_incremental(self):
        original = {p.name:p.read_bytes() for p in self.assets.iterdir()}
        a = cook(**self.options)
        self.assertEqual(a['cache_hits'], 0)
        self.assertLess(a['bytes'], a['source_bytes'])
        self.assertEqual(cook(**self.options)['cache_hits'], 3)
        self.assertIn(b'1.2300e+03', (self.base/'cooked/en.kinlang').read_bytes())
        self.assertEqual(json.loads(original['en.kinlang']), json.loads((self.base/'cooked/en.kinlang').read_bytes()))
        self.assertEqual((self.base/'cooked/sound.wav').read_bytes(), original['sound.wav'])
        self.assertEqual(original, {p.name:p.read_bytes() for p in self.assets.iterdir()})
        (self.assets/'sound.wav').write_bytes(b'new audio')
        self.assertEqual(cook(**self.options)['cache_hits'], 2)
        self.assertEqual(cook(**self.options, target='other')['cache_hits'], 0)

    def test_corrupt_cache_and_independent_output(self):
        cook(**self.options)
        for blob in (self.base/'cache').glob('*.blob'):
            blob.write_bytes(b'corrupted')
        self.assertEqual(cook(**self.options)['cache_hits'], 0)
        (self.base/'cooked/sound.wav').write_bytes(b'changed output')
        self.assertEqual(cook(**self.options)['cache_hits'], 3)
        self.assertEqual((self.base/'cooked/sound.wav').read_bytes(), b'unchanged audio')

    def test_recipe_and_stale_removal(self):
        cook(**self.options)
        result = cook(**self.options, json_mode='copy', png_mode='copy')
        self.assertEqual(result['source_bytes'],result['bytes'])
        (self.assets/'sound.wav').unlink()
        manifest = json.loads(self.manifest.read_text()); manifest['roots'].remove('sound.wav')
        self.manifest.write_text(json.dumps(manifest))
        cook(**self.options)
        self.assertFalse((self.base/'cooked/sound.wav').exists())

    def test_foreign_output_and_overlap(self):
        self.options['output'].mkdir()
        (self.options['output']/'mine').write_text('keep')
        with self.assertRaisesRegex(ValueError, 'not owned'):
            cook(**self.options)
        self.assertEqual((self.options['output']/'mine').read_text(),'keep')
        self.options['output'] = self.assets/'output'
        with self.assertRaisesRegex(ValueError, 'Overlapping'):
            cook(**self.options)

    def test_invalid_graph_and_transaction(self):
        cook(**self.options)
        before = (self.base/'cooked/en.kinlang').read_bytes()
        (self.assets/'en.kinlang').write_bytes(b'bad json')
        with self.assertRaises(ValueError):
            cook(**self.options)
        self.assertEqual((self.base/'cooked/en.kinlang').read_bytes(), before)
        (self.assets/'en.kinlang').unlink()
        with self.assertRaisesRegex(ValueError,'dependency graph'):
            cook(**self.options)

    def test_lock_and_generated_path_collision(self):
        lock=self.base/'cooked.kin-cook-lock'
        lock.write_text('locked')
        with self.assertRaises(FileExistsError):
            cook(**self.options)
        self.assertTrue(lock.exists())
        lock.unlink()
        self.options['report']=self.base/'cooked.kin-cook-owner'
        with self.assertRaisesRegex(ValueError,'Overlapping'):
            cook(**self.options)

    def test_json_and_png_semantics(self):
        for invalid in (b'{"a":1,"a":2}', b'{"a":NaN}', b'{bad}'):
            with self.assertRaises(ValueError):
                minify_json(invalid)
        source = png(chunk(b'tEXt', b'Author\0Kin'))
        cooked = recompress_png(source)
        self.assertLess(len(cooked),len(source))
        self.assertIn(chunk(b'tEXt',b'Author\0Kin'),cooked)
        def pixels(data):
            offset=8; compressed=b''
            while offset<len(data):
                length,kind=struct.unpack_from('>I4s',data,offset)
                if kind==b'IDAT': compressed+=data[offset+8:offset+8+length]
                offset+=length+12
            return zlib.decompress(compressed)
        self.assertEqual(pixels(source),pixels(cooked))
        for extra in (chunk(b'acTL',struct.pack('>II',1,0)),chunk(b'vpAg',b'unknown')):
            image=png(extra)
            self.assertEqual(recompress_png(image),image)
        with self.assertRaises(ValueError): recompress_png(source[:-1])
        with self.assertRaises(ValueError): recompress_png(source[:-5]+b'wrong')

    def test_cmake_package(self):
        project=self.base/'project'; project.mkdir()
        (project/'main.cpp').write_text('int main(){return 0;}\n')
        (project/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.22)
project(cook_check LANGUAGES CXX)
list(APPEND CMAKE_MODULE_PATH "{ROOT}/cmake")
include(kin_release)
include(kin_game)
function(kin_enable_release_optimizations target)
endfunction()
function(kin_enable_release_link_optimizations target)
endfunction()
add_executable(check main.cpp)
kin_configure_game_assets(check demo SOURCE_DIR "{self.assets}" MANIFEST "{self.manifest}")
kin_package_game(check)
''')
        build=self.base/'build'
        def run(*args):
            subprocess.run(args,check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        run('cmake','-S',str(project),'-B',str(build),'-DCMAKE_BUILD_TYPE=Release','-DKIN_ENABLE_ASSET_COOKING=ON')
        run('cmake','--build',str(build),'--target','check_package')
        report=next((build/'packages').glob('*.cook.json'))
        result=json.loads(report.read_text())
        import hashlib
        archive=next((build/'packages').glob('*.zip'))
        self.assertEqual(result['package_sha256'],hashlib.sha256(archive.read_bytes()).hexdigest())
        stage=next(p for p in (build/'packages').iterdir() if p.is_dir())
        self.assertEqual((stage/'assets/demo/en.kinlang').read_bytes(),minify_json((self.assets/'en.kinlang').read_bytes()))
        # Direct package script also refreshes data without rebuilding code.
        (self.assets/'sound.wav').write_bytes(b'edited after build')
        run('cmake','-P',str(build/'package-check-Release.cmake'))
        self.assertEqual((stage/'assets/demo/sound.wav').read_bytes(),b'edited after build')
        size=json.loads(next((build/'packages').glob('*.size.json')).read_text())
        self.assertTrue(size['optimizations']['asset_cooking'])
        # Turning cooking off restores raw installation and removes stale sidecars.
        run('cmake','-S',str(project),'-B',str(build),'-DKIN_ENABLE_ASSET_COOKING=OFF')
        run('cmake','--build',str(build),'--target','check_package')
        self.assertFalse(report.exists())
        self.assertEqual((stage/'assets/demo/en.kinlang').read_bytes(),(self.assets/'en.kinlang').read_bytes())
        size=json.loads(next((build/'packages').glob('*.size.json')).read_text())
        self.assertFalse(size['optimizations']['asset_cooking'])


if __name__ == '__main__':
    unittest.main()
