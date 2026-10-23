#!/usr/bin/env python3
"""Collect already accepted desktop artifacts for a versioned GitHub release."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def check_tag(tag, revision):
    version = re.search(r'project\(kin VERSION ([0-9]+\.[0-9]+\.[0-9]+)\b',
                        (ROOT/'CMakeLists.txt').read_text(encoding='utf-8')).group(1)
    if tag != 'v'+version:
        raise ValueError(f'Release tag must match project version v{version}')
    target = subprocess.check_output(['git', 'rev-parse', f'refs/tags/{tag}^{{commit}}'],
                                     cwd=ROOT, text=True).strip()
    if target != revision:
        raise ValueError('Release tag points at a different commit')
    return version


def collect(incoming, output, version, revision):
    # Keep source artifacts immutable. Build a separate flat upload directory
    # only after every platform and fresh-runtime proof has been checked.
    files = {}
    packages = {}

    def add(path, name=None):
        name = name or path.name
        if name in files:
            raise ValueError('Duplicate release filename: '+name)
        files[name] = path

    def proof(path, package_sha, backend):
        data = read(path)
        identity = data['release']
        if (data['schema'] != 'kin.release-acceptance/1'
                or data['package_sha256'] != package_sha
                or data['backend'] != backend
                or data['automated_gate_passed'] is not True
                or data['smoke_only'] is not False
                or data['read_only_enforced'] is not True
                or data['settings_roundtrip'] is not True
                or identity['source_revision'] != revision
                or identity['source_dirty'] != 'false'
                or identity['version'] != version):
            raise ValueError('Invalid release acceptance: '+str(path))
        runs = data['runs']
        if (len(runs) != 6 or any(run['status'] != 'ok' for run in runs)
                or {run['locale'] for run in runs} != {'en', 'fr', 'ja', 'ar'}):
            raise ValueError('Incomplete acceptance scenarios: '+str(path))

    for platform in ('Linux', 'Windows'):
        folder = incoming/('desktop-v1-'+platform)
        candidates = [p for p in (folder/'packages').glob('signal_siege-*.zip')
                      if not p.name.endswith('-symbols.zip')]
        if len(candidates) != 1:
            raise ValueError('Expected one player ZIP for '+platform)
        package = candidates[0]
        sha = digest(package)
        for kind in ('release', 'components', 'assets', 'cook', 'size'):
            sidecar = package.with_suffix('.'+kind+'.json')
            metadata = read(sidecar)
            if metadata['package_sha256'] != sha:
                raise ValueError('Sidecar package mismatch: '+str(sidecar))
            add(sidecar)
        identity = read(package.with_suffix('.release.json'))
        if (identity['source_revision'] != revision or identity['source_dirty'] != 'false'
                or identity['version'] != version or identity['system'] != platform
                or identity['target'] != 'signal_siege' or identity['contract'] != 'desktop-v1'):
            raise ValueError('Wrong release identity: '+str(package))
        symbols = package.with_name(package.stem+'-symbols.zip')
        with zipfile.ZipFile(symbols) as archive:
            manifests = [n for n in archive.namelist() if n.endswith('/manifest.json')]
            if len(manifests) != 1:
                raise ValueError('Expected one debug-symbol manifest')
            if json.loads(archive.read(manifests[0]))['package_sha256'] != sha:
                raise ValueError('Debug symbols belong to another package')
        add(package)
        add(symbols)
        build_proof = folder/'acceptance/acceptance.json'
        proof(build_proof, sha, 'software')
        add(build_proof, package.stem+'.acceptance-build.json')
        if platform == 'Linux':
            audit = folder/'packages/release-audit.json'
            data = read(audit)
            if data['passed'] is not True or data['package_sha256'] != sha:
                raise ValueError('Compiler audit does not match the player ZIP')
            add(audit, package.stem+'.compiler-audit.json')
            for backend in ('software', 'gpu'):
                path = incoming/f'desktop-v1-clean-Linux-{backend}/acceptance.json'
                proof(path, sha, backend)
                add(path, package.stem+f'.acceptance-clean-{backend}.json')
        else:
            path = incoming/'desktop-v1-fresh-Windows/acceptance.json'
            proof(path, sha, 'software')
            add(path, package.stem+'.acceptance-fresh.json')
        packages[platform] = {'file': package.name, 'sha256': sha}

    if output.exists() and any(output.iterdir()):
        raise ValueError('Release output must be empty')
    output.mkdir(parents=True, exist_ok=True)
    for name, path in files.items():
        shutil.copyfile(path, output/name)
    manifest = {'schema': 'kin.published-release/1', 'version': version,
                'source_revision': revision, 'packages': packages,
                'files': {name: digest(output/name) for name in sorted(files)}}
    (output/'release-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    paths = sorted(output.iterdir())
    (output/'SHA256SUMS').write_text(''.join(f'{digest(p)}  {p.name}\n' for p in paths), encoding='utf-8')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-tag', required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--incoming', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        version = check_tag(args.check_tag, args.revision)
        if args.incoming is not None or args.output is not None:
            if args.incoming is None or args.output is None:
                raise ValueError('--incoming and --output must be supplied together')
            collect(args.incoming, args.output, version, args.revision)
        print('Validated release '+args.check_tag+' at '+args.revision)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        parser.exit(1, f'Release preparation failed: {error}\n')


if __name__ == '__main__':
    main()
