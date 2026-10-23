"""Read a version-1 Kin content pack for release artifact verification."""
from pathlib import PurePosixPath
import struct
import zlib


def extract_content_pack(path, destination):
    data = path.read_bytes()
    if len(data) < 32:
        raise ValueError('Truncated content pack')
    magic, version, count, offset, size, checksum = struct.unpack_from('<8sIIQII', data)
    if magic != b'KINPAK\x1a\n' or version != 1:
        raise ValueError('Unsupported content pack')
    if offset < 32 or offset + size != len(data):
        raise ValueError('Content pack index out of range')
    index = data[offset:]
    if zlib.crc32(index) != checksum:
        raise ValueError('Content pack index checksum mismatch')
    entries = []
    cursor = 0
    previous = ''
    names = set()
    for _ in range(count):
        if cursor + 24 > len(index):
            raise ValueError('Truncated content pack index')
        start, length, crc, name_size = struct.unpack_from('<QQII', index, cursor)
        cursor += 24
        if cursor + name_size > len(index):
            raise ValueError('Truncated content pack path')
        name = index[cursor:cursor + name_size].decode('utf-8')
        cursor += name_size
        parts = name.split('/')
        if any(part in ('', '.', '..') or ':' in part or '\\' in part for part in parts):
            raise ValueError('Unsafe content pack path: ' + name)
        if name <= previous or name.casefold() in names:
            raise ValueError('Duplicate or unordered content pack path')
        previous = name
        names.add(name.casefold())
        if start < 32 or start + length > offset:
            raise ValueError('Content pack file out of range')
        content = data[start:start + length]
        if zlib.crc32(content) != crc:
            raise ValueError('Content pack file checksum mismatch: ' + name)
        entries.append((name, content))
    if cursor != len(index):
        raise ValueError('Trailing content pack index bytes')
    # Validate the whole index before creating any files.
    for name, content in entries:
        target = destination.joinpath(*PurePosixPath(name).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    return destination
