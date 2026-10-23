#!/usr/bin/env python3
"""Portable, lossless asset cooking. No pruning or runtime format changes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import zlib

sys.dont_write_bytecode = True
from report_game_assets import analyze, unique_object


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, sort_keys=True, ensure_ascii=False, indent=2) + '\n').encode()


def atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=path.parent, prefix='.kin-cook-')
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


def minify_json(data):
    def invalid(value):
        raise ValueError(f'Non-finite JSON constant: {value}')
    json.loads(data.decode('utf-8'), object_pairs_hook=unique_object,
               parse_constant=invalid, parse_float=str, parse_int=str)
    output = bytearray()
    quoted = escaped = False
    for byte in data:
        if quoted or byte not in b' \t\r\n':
            output.append(byte)
        if quoted:
            if escaped:
                escaped = False
            elif byte == 92:
                escaped = True
            elif byte == 34:
                quoted = False
        elif byte == 34:
            quoted = True
    return bytes(output)


def recompress_png(data):
    if not data.startswith(b'\x89PNG\r\n\x1a\n'):
        raise ValueError('Invalid PNG signature')
    chunks = []
    offset = 8
    while offset < len(data):
        start = offset
        if offset + 12 > len(data):
            raise ValueError('Truncated PNG chunk')
        length, kind = struct.unpack_from('>I4s', data, offset)
        offset += length + 12
        if offset > len(data):
            raise ValueError('Truncated PNG data')
        payload = data[start + 8:offset - 4]
        crc, = struct.unpack_from('>I', data, offset - 4)
        if zlib.crc32(kind + payload) != crc:
            raise ValueError('PNG CRC mismatch')
        chunks.append((kind, payload, data[start:offset]))
    kinds = [c[0] for c in chunks]
    if (not kinds or kinds[0] != b'IHDR' or kinds[-1] != b'IEND'
            or kinds.count(b'IHDR') != 1 or kinds.count(b'IEND') != 1
            or b'IDAT' not in kinds or len(chunks[0][1]) != 13 or chunks[-1][1]):
        raise ValueError('Invalid PNG structure')
    indices = [i for i, kind in enumerate(kinds) if kind == b'IDAT']
    if indices != list(range(indices[0], indices[-1] + 1)):
        raise ValueError('Nonconsecutive PNG IDAT chunks')
    known = set(b'IHDR PLTE IDAT IEND cHRM gAMA iCCP sBIT sRGB bKGD hIST tRNS pHYs sPLT tIME iTXt tEXt zTXt eXIf'.split())
    if any(kind not in known for kind in kinds):
        return data  # Includes animated PNG: no interpretation of extension semantics.
    inflater = zlib.decompressobj()
    # Bound memory use; larger assets remain byte-identical passthroughs.
    filtered = inflater.decompress(b''.join(c[1] for c in chunks if c[0] == b'IDAT'), 64 * 1024 * 1024 + 1)
    if len(filtered) > 64 * 1024 * 1024 or inflater.unconsumed_tail:
        return data
    if not inflater.eof:
        raise ValueError('Incomplete PNG zlib stream')
    if inflater.unused_data:
        return data
    compressed = zlib.compress(filtered, 9)
    chunk = struct.pack('>I', len(compressed)) + b'IDAT' + compressed + struct.pack('>I', zlib.crc32(b'IDAT' + compressed))
    return data[:8] + b''.join(chunk if i == indices[0] else raw
        for i, (kind, _, raw) in enumerate(chunks) if kind != b'IDAT' or i == indices[0])


def cook(assets, manifest, output, cache, report, target='portable', json_mode='minify', png_mode='recompress'):
    assets, manifest, output, cache, report = [Path(p).absolute().resolve() for p in (assets, manifest, output, cache, report)]
    # Generated paths may never contain or sit inside input paths, nor each other.
    marker = output.with_name(output.name + '.kin-cook-owner')
    lock = output.with_name(output.name + '.kin-cook-lock')
    backup = output.with_name(output.name + '.kin-cook-backup')
    paths = [assets, output, cache, report, manifest, marker, lock, backup]
    for i, left in enumerate(paths):
        for right in paths[i + 1:]:
            if left == right or left in right.parents or right in left.parents:
                raise ValueError(f'Overlapping cooking paths: {left} and {right}')
    graph = analyze(assets, manifest)
    if not graph['valid']:
        raise ValueError('Invalid asset dependency graph: ' + str(graph['diagnostics']))
    output.parent.mkdir(parents=True, exist_ok=True)
    owner = b'kin.asset-cook-output/1\n'
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)
    temporary = None
    try:
        if ((marker.exists() and (not marker.is_file() or marker.read_bytes() != owner))
                or (output.exists() and not marker.exists())):
            raise ValueError('Refusing to replace an output directory not owned by the cooker')
        temporary = Path(tempfile.mkdtemp(prefix='.kin-cooked-', dir=output.parent))
        settings = {'schema': 'kin.asset-cook/1', 'target': target, 'json': json_mode,
                    'png': png_mode, 'cooker_sha256': digest(Path(__file__).read_bytes()),
                    'zlib': zlib.ZLIB_RUNTIME_VERSION, 'zlib_compile': zlib.ZLIB_VERSION}
        files = []
        for entry in graph['files']:
            path = entry['path']
            data = (assets / path).read_bytes()
            if digest(data) != entry['sha256']:
                raise ValueError(f'Asset changed while cooking: {path}')
            suffix = Path(path).suffix.lower()
            recipe = 'copy'
            if suffix in ('.kinlang', '.kinscene', '.kinprefab', '.kincomponents') and json_mode == 'minify':
                recipe = 'json-minify'
            elif suffix == '.png' and png_mode == 'recompress':
                recipe = 'png-recompress'
            key = digest(encoded({'source': entry['sha256'], 'recipe': recipe, 'settings': settings}))
            blob, meta = cache / (key + '.blob'), cache / (key + '.json')
            cooked = None
            try:
                candidate = blob.read_bytes()
                metadata = json.loads(meta.read_bytes())
                if metadata['sha256'] == digest(candidate) and metadata['bytes'] == len(candidate):
                    cooked = candidate
            except (OSError, ValueError, KeyError, TypeError):
                pass
            hit = cooked is not None
            if not hit:
                cooked = minify_json(data) if recipe == 'json-minify' else recompress_png(data) if recipe == 'png-recompress' else data
                if len(cooked) >= len(data):
                    cooked = data
                atomic(blob, cooked)
                atomic(meta, encoded({'sha256': digest(cooked), 'bytes': len(cooked)}))
            destination = temporary / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(cooked)
            files.append({'path': path, 'recipe': recipe, 'cache_hit': hit, 'source_bytes': len(data),
                          'bytes': len(cooked), 'source_sha256': entry['sha256'], 'sha256': digest(cooked)})
        # Catch removed/added/edited sources and graph/manifest changes during work.
        if analyze(assets, manifest) != graph:
            raise ValueError('Asset graph changed while cooking; retry')
        result = dict(settings, pruning_applied=False, files=files,
                      source_inventory_sha256=graph['inventory_sha256'], manifest_sha256=graph['manifest_sha256'],
                      source_bytes=sum(f['source_bytes'] for f in files), bytes=sum(f['bytes'] for f in files),
                      cache_hits=sum(f['cache_hit'] for f in files))
        if backup.exists():
            raise ValueError(f'Previous cooking backup requires recovery: {backup}')
        atomic(marker, owner)
        if output.exists():
            output.rename(backup)
        try:
            temporary.rename(output)
        except BaseException:
            if backup.exists():
                backup.rename(output)
            raise
        temporary = None
        if backup.exists():
            shutil.rmtree(backup)
        atomic(report, encoded(result))
        return result
    finally:
        if temporary is not None:
            shutil.rmtree(temporary)
        lock.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('assets', 'manifest', 'output', 'cache', 'report'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--target', default='portable')
    parser.add_argument('--json', dest='json_mode', choices=('minify', 'copy'), default='minify')
    parser.add_argument('--png', dest='png_mode', choices=('recompress', 'copy'), default='recompress')
    try:
        result = cook(**vars(parser.parse_args()))
        print(f"Cooked {len(result['files'])} assets: {result['source_bytes']} -> {result['bytes']} bytes; {result['cache_hits']} cache hits; no pruning")
    except (OSError, ValueError, zlib.error) as error:
        parser.exit(1, f'Asset cooking failed: {error}\n')


if __name__ == '__main__':
    main()
