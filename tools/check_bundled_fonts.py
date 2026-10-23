#!/usr/bin/env python3
"""Check committed font hashes and supported locale coverage without FontTools."""
import hashlib
import json
from pathlib import Path
import unicodedata

ROOT=Path(__file__).resolve().parents[1]


def strings(value):
    if isinstance(value,str): yield value
    elif isinstance(value,dict):
        for key,item in value.items(): yield key; yield from strings(item)
    elif isinstance(value,list):
        for item in value: yield from strings(item)


def check(root):
    meta=json.loads((root/'fonts/provenance.json').read_text(encoding='utf-8'))
    covered=set(meta['required_codepoints'])
    for path in root.glob('*.kinlang'):
        for text in strings(json.loads(path.read_text(encoding='utf-8'))):
            for char in text:
                if ord(char)>=32 and unicodedata.category(char)!='Cc' and ord(char) not in covered:
                    raise ValueError(f'{path}: new character U+{ord(char):04X}; regenerate bundled fonts')
    for entry in meta['files']:
        data=(root/'fonts'/entry['path']).read_bytes()
        if hashlib.sha256(data).hexdigest()!=entry['sha256'] or len(data)!=entry['bytes']:
            raise ValueError('Font does not match provenance: '+entry['path'])
    for name in ('core-OFL.txt','cjk-OFL.txt'):
        text=(root/'fonts'/name).read_text(encoding='utf-8')
        if 'SIL OPEN FONT LICENSE' not in text or 'PERMISSION & CONDITIONS' not in text or 'DISCLAIMER' not in text:
            raise ValueError('Incomplete font license: '+name)
    return meta


if __name__=='__main__':
    result=check(ROOT/'examples/content/lang')
    print(f"Bundled fonts verified: {sum(f['bytes'] for f in result['files'])} bytes; all locale characters covered")
