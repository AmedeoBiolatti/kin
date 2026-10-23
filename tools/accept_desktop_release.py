#!/usr/bin/env python3
"""Accept an exact Signal Siege release ZIP after relocation (stdlib + Pillow)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import tempfile
import zipfile

from content_pack import extract_content_pack


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(root):
    return {p.relative_to(root).as_posix():sha(p) for p in sorted(root.rglob('*')) if p.is_file()}


def extract(archive, destination):
    names=set()
    with zipfile.ZipFile(archive) as zipped:
        for item in zipped.infolist():
            # ZipInfo normalizes backslashes on Windows and truncates NULs.
            # Validate the original archive spelling before those transformations.
            name=item.orig_filename
            if name != item.filename or '\x00' in name:
                raise ValueError('Unsafe ZIP path: '+repr(name))
            parts=name.rstrip('/').split('/')
            if any(p in ('','.','..') or ':' in p or '\\' in p for p in parts):
                raise ValueError('Unsafe ZIP path: '+item.filename)
            if stat.S_ISLNK(item.external_attr >> 16): raise ValueError('ZIP symlinks are unsupported')
            key=item.filename.casefold()
            if key in names: raise ValueError('Duplicate ZIP path')
            names.add(key)
        zipped.extractall(destination)
    roots=list(destination.iterdir())
    if len(roots)!=1 or not roots[0].is_dir(): raise ValueError('Expected one package root')
    return roots[0]


def abi(root, maximum_glibc, maximum_glibcxx, enforce=True):
    measured={'GLIBC':(), 'GLIBCXX':()}
    for path in root.rglob('*'):
        if not path.is_file(): continue
        with path.open('rb') as stream: magic=stream.read(4)
        if magic!=b'\x7fELF': continue
        if not shutil.which('readelf'): raise ValueError('ELF acceptance requires readelf (binutils)')
        header=subprocess.check_output(['readelf','-h',str(path)],text=True)
        if 'Advanced Micro Devices X86-64' not in header: raise ValueError('Non-x86-64 ELF')
        versions=subprocess.check_output(['readelf','--version-info',str(path)],text=True)
        for name,version in re.findall(r'\b(GLIBCXX|GLIBC)_([0-9]+(?:\.[0-9]+)+)',versions):
            measured[name]=max(measured[name],tuple(map(int,version.split('.'))))
    for key,limit in [('GLIBC',maximum_glibc),('GLIBCXX',maximum_glibcxx)]:
        if enforce and measured[key]>tuple(map(int,limit.split('.'))):
            raise ValueError(f'{key} requirement exceeds desktop-v1: {measured[key]} > {limit}')
    return {key:'.'.join(map(str,value)) for key,value in measured.items()}


def readonly(root, enable):
    if os.name=='nt':
        user=subprocess.check_output(['whoami'],text=True).strip()
        rights='RX' if enable else 'F'
        subprocess.run(['icacls',str(root),'/inheritance:r','/grant:r',f'{user}:(OI)(CI)({rights})','/T','/Q'],
                       check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    else:
        for path in sorted(root.rglob('*'),reverse=True):
            path.chmod((0o555 if path.is_dir() or path.name=='signal_siege' else 0o444) if enable else (0o755 if path.is_dir() or path.name=='signal_siege' else 0o644))
        root.chmod(0o555 if enable else 0o755)


def accept(package, output, backend='software', expected_revision=None, require_clean=False, require_readonly=False, video_driver=None, smoke_only=False):
    from PIL import Image
    archive=package.resolve();output=output.resolve();output.mkdir(parents=True,exist_ok=True)
    package_sha=sha(archive)
    sidecars={}
    for suffix in ('release','components','assets','cook','size'):
        path=archive.with_suffix('.'+suffix+'.json')
        value=json.loads(path.read_text(encoding='utf-8'))
        if value.get('package_sha256')!=package_sha: raise ValueError('Sidecar ZIP mismatch: '+str(path))
        sidecars[suffix]=value
    symbols=archive.with_name(archive.stem+'-symbols.zip')
    with zipfile.ZipFile(symbols) as symbol_zip:
        manifests=[name for name in symbol_zip.namelist() if name.endswith('/manifest.json')]
        if len(manifests)!=1:
            raise ValueError('Expected one symbol manifest')
        symbol_metadata=json.loads(symbol_zip.read(manifests[0]))
        if symbol_metadata.get('package_sha256')!=package_sha:
            raise ValueError('Debug symbols do not match the player artifact')
        symbol_root=manifests[0].rsplit('/',1)[0]+'/'
        for entry in symbol_metadata['files']:
            if entry['debug_path'] and hashlib.sha256(symbol_zip.read(symbol_root+entry['debug_path'])).hexdigest()!=entry['debug_sha']:
                raise ValueError('Debug file hash mismatch')
    audit_file=archive.parent/'release-audit.json'
    if audit_file.exists():
        audit_data=json.loads(audit_file.read_text(encoding='utf-8'))
        if not audit_data.get('passed') or audit_data.get('package_sha256')!=package_sha:
            raise ValueError('Compiler command audit does not match player artifact')
    release=sidecars['release']
    if release['target']!='signal_siege' or release['contract']!='desktop-v1': raise ValueError('Not a Signal Siege desktop-v1 release')
    if expected_revision and release['source_revision']!=expected_revision: raise ValueError('Wrong source revision')
    if require_clean and release['source_dirty']!='false': raise ValueError('Release was configured from a dirty or unknown source tree')
    runs=[]
    with tempfile.TemporaryDirectory(prefix='kin release acceptance ') as temp:
        temp=Path(temp);root=extract(archive,temp/'installation with spaces');unrelated=temp/'unrelated working directory';unrelated.mkdir()
        try:
            expected={}
            for line in (root/'SHA256SUMS').read_text(encoding='utf-8').splitlines():
                digest,relative=line.split('  ',1)
                path=Path(relative)
                if path.is_absolute() or '..' in path.parts or '\\' in relative or ':' in relative: raise ValueError('Unsafe checksum path')
                if relative in expected: raise ValueError('Duplicate checksum entry')
                expected[relative]=digest
            before=snapshot(root)
            if expected!={name:digest for name,digest in before.items() if name!='SHA256SUMS'}: raise ValueError('Player file inventory/checksums mismatch')
            for entry in symbol_metadata['files']:
                if expected.get(entry['path'])!=entry['binary_sha']:
                    raise ValueError('Symbol manifest binary hash mismatch')
            internal=json.loads((root/'release.json').read_text(encoding='utf-8'))
            if internal!={key:value for key,value in release.items() if key!='package_sha256'}: raise ValueError('Release identity mismatch')
            if (root/'signal_siege.kinpak').exists():
                assets=extract_content_pack(root/'signal_siege.kinpak',temp/'verified content')
                font_root=assets/'lang/fonts'
            else:
                assets=root/'signal_siege'
                font_root=assets/'lang/fonts'
            font_meta=json.loads((font_root/'provenance.json').read_text(encoding='utf-8'))
            for entry in font_meta['files']:
                if sha(font_root/entry['path'])!=entry['sha256']: raise ValueError('Bundled font hash mismatch')
            for entry in sidecars['cook']['files']:
                if sha(assets/entry['path'])!=entry['sha256']: raise ValueError('Cooked asset mismatch')
            binary=root/('signal_siege.exe' if os.name=='nt' else 'signal_siege')
            if os.name!='nt': binary.chmod(0o755)
            policy=json.loads((root/'PLATFORM.json').read_text(encoding='utf-8'))
            trusted_policy=Path(__file__).resolve().parents[1]/'release/desktop-v1.json'
            if policy!=json.loads(trusted_policy.read_text(encoding='utf-8')):
                raise ValueError('Player platform policy differs from the acceptance policy')
            if policy['id']!='desktop-v1' or sha(root/'PLATFORM.json')!=release['contract_sha256']:
                raise ValueError('Platform policy mismatch')
            if os.name!='nt':
                abi_result=abi(root,policy['linux']['maximum_glibc'],policy['linux']['maximum_glibcxx'],enforce=not smoke_only)
            else:
                for name in (() if release['static_runtime'] else ('vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll')):
                    if not (root/name).is_file():
                        raise ValueError('Missing app-local MSVC runtime: '+name)
                for pe in [binary,*root.rglob('*.dll')]:
                    data=pe.read_bytes();offset=struct.unpack_from('<I',data,0x3c)[0]
                    if data[offset:offset+4]!=b'PE\0\0' or struct.unpack_from('<H',data,offset+4)[0]!=0x8664:
                        raise ValueError('Non-x64 PE executable or library')
                abi_result={'PE':'x64'}
            environment=dict(os.environ)
            for key in ('LD_LIBRARY_PATH','DYLD_LIBRARY_PATH','SDL_DYNAMIC_API'):
                environment.pop(key,None)
            environment.update(SDL_VIDEO_DRIVER=video_driver or ('x11' if backend=='gpu' and os.name!='nt' else 'windows' if backend=='gpu' else 'dummy'),
                SDL_AUDIO_DRIVER='dummy',KIN_RENDER_BACKEND='gpu' if backend=='gpu' else 'sdl',KIN_LOG_FORMAT='text',
                XDG_DATA_HOME=str(temp/'player-data'),APPDATA=str(temp/'player-data'),HOME=str(temp/'player-home'),USERPROFILE=str(temp/'player-home'))
            if os.name=='nt': environment['PATH']=os.pathsep.join([str(Path(os.environ['SystemRoot'])/'System32'),os.environ['SystemRoot']])
            readonly(root,True)
            enforced=False
            try:
                probe=root/'write-permission-probe'
                probe.write_text('test', encoding='utf-8');probe.unlink()
            except PermissionError: enforced=True
            if require_readonly and not enforced: raise ValueError('Read-only installation is not enforced for this user (root bypasses Unix permissions)')
            info=subprocess.run([str(binary),'--game-info'],cwd=unrelated,env=environment,
                                capture_output=True,text=True,encoding='utf-8',check=True,timeout=30)
            if '- version: '+release['version'] not in info.stdout:
                raise ValueError('Runtime game version differs from release identity')
            cases=[('arena','en',False,False,12,300),('stress','en',False,True,12,1200)]
            cases += [('grid-'+locale,locale,True,False,3,300) for locale in ('en','fr','ja','ar')]
            for case,locale,grid,stress,frames,enemies in cases:
                capture=output/(case+'.png')
                command=[str(binary),'--headless','--render','--frames='+str(frames),'--seed=7','--report=-',
                         '--screenshot',str(capture),'--capture-frame',str(frames),'--fail-on-missing-text','--locale='+locale]
                if grid: command.append('--power-grid')
                if stress: command.append('--stress')
                completed=subprocess.run(command,cwd=unrelated,env=environment,capture_output=True,text=True,encoding='utf-8',check=True,timeout=180)
                report=json.loads(completed.stdout)
                expected_backend='SDL_GPU' if backend=='gpu' else 'software'
                if report['status']!='ok' or report['localization']['locale']!=locale or report['localization']['missing_text']:
                    raise ValueError('Localization/game run failed')
                state=report['scenes'][0]['state']
                if state['enemies']!=enemies or state['power_grid']!=grid:
                    raise ValueError('Wrong gameplay scenario')
                if 'backend='+expected_backend not in completed.stderr: raise ValueError('Wrong renderer')
                if 'bundled font chain loaded' not in completed.stderr: raise ValueError('Bundled fonts were not used')
                with Image.open(capture) as image:
                    pixels=image.convert('RGBA').tobytes()
                    if len(set(pixels))<3: raise ValueError('Empty capture')
                    pixel_sha=hashlib.sha256(pixels).hexdigest()
                (output/(case+'.json')).write_text(json.dumps(report,indent=2)+'\n', encoding='utf-8')
                runs.append({'case':case,'locale':locale,'backend':expected_backend,'state':state,'pixel_sha256':pixel_sha,'status':'ok'})
            for attempt in range(2):
                subprocess.run([str(binary),'--check-user-data'],cwd=unrelated,env=environment,capture_output=True,text=True,encoding='utf-8',check=True,timeout=30)
            settings=temp/'player-data/kin/signal_siege/settings.json'
            settings_data=json.loads(settings.read_text(encoding='utf-8'))
            if settings_data['payload']['locale']!='ja' or settings_data['game_version']!=release['version']: raise ValueError('User settings did not persist')
            if snapshot(root)!=before: raise ValueError('Game modified its installation')
        finally:
            readonly(root,False)
    result={'schema':'kin.release-acceptance/1','package_sha256':package_sha,'release':release,'backend':backend,
            'abi':abi_result,'automated_gate_passed':not smoke_only,'smoke_only':smoke_only,'read_only_enforced':enforced,'settings_roundtrip':True,'runs':runs,
            'qualification':'Relocated package smoke test. OS baseline and real hardware qualification depend on the test host.'}
    (output/'acceptance.json').write_text(json.dumps(result,indent=2)+'\n', encoding='utf-8')
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--backend',choices=('software','gpu'),default='software')
    parser.add_argument('--smoke-only',action='store_true',help='Development smoke test; measures ABI without qualifying this artifact for release')
    parser.add_argument('--video-driver');parser.add_argument('--expected-revision');parser.add_argument('--require-clean',action='store_true');parser.add_argument('--require-readonly',action='store_true')
    try:
        result=accept(**vars(parser.parse_args()))
        print(('Accepted ' if result['automated_gate_passed'] else 'Smoke test only (NOT release-qualified): ')+result['package_sha256']+'; six scenarios, four locales, '+result['backend']+', settings round trip')
    except (OSError,ValueError,KeyError,subprocess.SubprocessError,zipfile.BadZipFile) as error:
        parser.exit(1,f'Release acceptance failed: {error}\n')


if __name__=='__main__': main()
