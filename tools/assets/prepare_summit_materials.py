#!/usr/bin/env python3
"""Prepare two pinned CC0 ambientCG 1K materials. Tool dependency: Pillow."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import urllib.request
import zipfile
from PIL import Image

PACKS = {'Rock030': '2fd0bc2d214e2487c19062ed0798a85dd135b41cfc12bbaed42d5acae238f989',
         'Ground037': '4e69db2b7251b1736fa70fe6fe4ab80d64bd220117dd5dd4c7caaead51c2574e'}
OUT = Path(__file__).resolve().parents[2] / 'examples/summit/content/ambientcg'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archives', type=Path, help='Folder containing Rock030.zip and Ground037.zip')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    manifest = {'author': 'ambientCG / Lennart Demes', 'license': 'CC0-1.0',
                'license_url': 'https://docs.ambientcg.com/license/', 'materials': {}}
    for asset, sha in PACKS.items():
        url = f'https://ambientcg.com/get?file={asset}_1K-JPG.zip'
        if args.archives:
            blob = (args.archives / (asset + '.zip')).read_bytes()
        else:
            with urllib.request.urlopen(url, timeout=30) as response:
                blob = response.read(24 * 1024 * 1024 + 1)
        if len(blob) > 24 * 1024 * 1024 or hashlib.sha256(blob).hexdigest() != sha:
            raise SystemExit(f'{asset}: unexpected archive checksum/size')
        files = {}
        def write(name, data):
            (OUT / name).write_bytes(data)
            files[name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            prefix = asset + '_1K-JPG_'
            for channel in ['Color', 'NormalGL']:
                data = archive.read(prefix + channel + '.jpg')
                with Image.open(io.BytesIO(data)) as image:
                    assert image.size == (1024, 1024)
                write(asset + '-' + channel + '.jpg', data)
            ao = Image.open(archive.open(prefix + 'AmbientOcclusion.jpg')).convert('L')
            roughness = Image.open(archive.open(prefix + 'Roughness.jpg')).convert('L')
            assert ao.size == roughness.size == (1024, 1024)
            orm = Image.merge('RGB', (ao, roughness, Image.new('L', ao.size, 0)))
            encoded = io.BytesIO(); orm.save(encoded, format='PNG')
            write(asset + '-ORM.png', encoded.getvalue())
        manifest['materials'][asset] = {'source': 'https://ambientcg.com/view?id=' + asset,
            'download': url, 'archive_sha256': sha, 'files': files,
            'modifications': 'Original colour and OpenGL normal JPEGs; AO and roughness packed into R/G of ORM, metallic B=0'}
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (OUT / 'License.txt').write_text('Rock030 and Ground037 by ambientCG / Lennart Demes.\nCC0-1.0: https://creativecommons.org/publicdomain/zero/1.0/\nLicense: https://docs.ambientcg.com/license/\nSources: https://ambientcg.com/view?id=Rock030 and https://ambientcg.com/view?id=Ground037\n')
    print('Prepared six 1024x1024 maps; 32 MiB estimated decoded RGBA8 texture residency including mipmaps.')


if __name__ == '__main__':
    main()
