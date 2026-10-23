#!/usr/bin/env python3
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from accept_desktop_release import extract, abi, readonly, snapshot
from check_bundled_fonts import check
from content_pack import extract_content_pack


class ReleaseAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)

    def test_zip_paths_and_duplicates(self):
        for name in ('../escape','/absolute','game/../escape','game/C:/bad','game\\bad'):
            archive=self.root/'bad.zip'
            with zipfile.ZipFile(archive,'w') as zipped:
                item=zipfile.ZipInfo()
                item.filename=name  # Preserve malformed backslashes on Windows too.
                zipped.writestr(item,b'bad')
            with self.subTest(name=name), self.assertRaises(ValueError):
                extract(archive,self.root/'out')
        archive=self.root/'case.zip'
        with zipfile.ZipFile(archive,'w') as zipped:
            zipped.writestr('game/A',b'a');zipped.writestr('game/a',b'b')
        with self.assertRaises(ValueError): extract(archive,self.root/'out')

    def test_zip_rejects_windows_normalization(self):
        archive=self.root/'backslash.zip'
        with zipfile.ZipFile(archive,'w') as zipped:
            item=zipfile.ZipInfo()
            item.filename='game\\bad'
            zipped.writestr(item,b'bad')
        with patch('zipfile.os.sep', '\\'):
            with self.assertRaises(ValueError): extract(archive,self.root/'out')

    def test_content_pack_checksums_and_paths(self):
        import struct
        import zlib
        payload = b'content'
        pack = self.root/'game.kinpak'
        def write(name, checksum=None):
            name = name.encode('utf-8')
            index = struct.pack('<QQII', 32, len(payload), zlib.crc32(payload), len(name)) + name
            header = struct.pack('<8sIIQII', b'KINPAK\x1a\n', 1, 1, 32 + len(payload), len(index),
                                 zlib.crc32(index) if checksum is None else checksum)
            pack.write_bytes(header + payload + index)
        write('lang/ja.kinlang')
        extract_content_pack(pack, self.root/'content')
        self.assertEqual((self.root/'content/lang/ja.kinlang').read_bytes(), payload)
        write('../escape')
        with self.assertRaisesRegex(ValueError, 'Unsafe'):
            extract_content_pack(pack, self.root/'bad')
        write('good', 0)
        with self.assertRaisesRegex(ValueError, 'checksum'):
            extract_content_pack(pack, self.root/'bad')

    def test_abi_limits_use_numeric_versions(self):
        (self.root/'game').write_bytes(b'\x7fELF')
        with patch('accept_desktop_release.shutil.which',return_value='/usr/bin/readelf'),patch('accept_desktop_release.subprocess.check_output') as read:
            read.side_effect=['Advanced Micro Devices X86-64','Name: GLIBC_2.9 Name: GLIBC_2.38 Name: GLIBCXX_3.4.32']
            self.assertEqual(abi(self.root,'2.39','3.4.33')['GLIBC'],'2.38')
            read.side_effect=['Advanced Micro Devices X86-64','Name: GLIBC_2.40']
            with self.assertRaises(ValueError): abi(self.root,'2.39','3.4.33')
            read.side_effect=['ARM','']
            with self.assertRaises(ValueError): abi(self.root,'2.39','3.4.33')

    def test_readonly_roundtrip(self):
        import os
        if os.name=='nt' or os.geteuid()==0: self.skipTest('Native ACLs are covered by release CI acceptance')
        folder=self.root/'game';folder.mkdir();(folder/'signal_siege').write_bytes(b'game')
        before=snapshot(folder)
        readonly(folder,True)
        try:
            with self.assertRaises(PermissionError): (folder/'write').write_text('bad', encoding='utf-8')
        finally: readonly(folder,False)
        self.assertEqual(snapshot(folder),before)

    def test_fonts_hashes_and_new_locales(self):
        source=ROOT/'examples/content/lang';root=self.root/'assets';shutil.copytree(source,root)
        check(root)
        (root/'new.kinlang').write_text(json.dumps({'text':'\U0001f600'}), encoding='utf-8')
        with self.assertRaisesRegex(ValueError,'regenerate'): check(root)
        (root/'new.kinlang').unlink();(root/'fonts/latin.ttf').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError,'provenance'): check(root)

    def test_toolchain_contract(self):
        for system,arch,compiler,version,msvc,number,success in [
            ('Linux','x86_64','GNU','13.4',False,0,True),
            ('Linux','x86_64','GNU','15.2',False,0,False),
            ('Linux','aarch64','GNU','13.4',False,0,False),
            ('Windows','AMD64','MSVC','19.44',True,1944,True),
            ('Windows','AMD64','MSVC','19.30',True,1930,True),
            ('Windows','AMD64','MSVC','19.29',True,1929,False),
            ('Windows','AMD64','MSVC','19.50',True,1950,False),
            ('Windows','AMD64','Clang','18',False,0,False)]:
            script=self.root/'contract.cmake'
            script.write_text(f'''cmake_minimum_required(VERSION 3.22)
set(KIN_RELEASE_CONTRACT ON)
set(CMAKE_SIZEOF_VOID_P 8)
set(CMAKE_SYSTEM_NAME {system})
set(CMAKE_SYSTEM_PROCESSOR {arch})
set(CMAKE_CXX_COMPILER_ID {compiler})
set(CMAKE_CXX_COMPILER_VERSION {version})
set(MSVC {'TRUE' if msvc else 'FALSE'})
set(MSVC_VERSION {number})
include("{ROOT.as_posix()}/cmake/kin_release_contract.cmake")
''', encoding='utf-8')
            result=subprocess.run(['cmake','-P',str(script)],capture_output=True,text=True)
            self.assertEqual(result.returncode==0,success,result.stdout+result.stderr)

    def test_font_check_uses_explicit_utf8(self):
        result = subprocess.run(
            [sys.executable, '-X', 'warn_default_encoding', '-W',
             'error::EncodingWarning', str(ROOT/'tools/check_bundled_fonts.py')],
            capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__=='__main__': unittest.main()
