"""Check widget PCH scope and compiler settings against an ordinary engine file."""
import argparse
import json
from pathlib import Path
import re
import shlex

parser = argparse.ArgumentParser()
parser.add_argument('--commands', type=Path, required=True)
args = parser.parse_args()
entries = json.loads(args.commands.read_text())
names = ['widgets_core', 'widgets_inputs', 'widgets_menus', 'widgets_collections', 'widgets_editor', 'widgets_text_edit']
def configuration(entry):
    output = entry.get('output', '').replace('\\', '/')
    match = re.search(r'\.dir/(Debug|Release|RelWithDebInfo|MinSizeRel)/', output)
    return match.group(1) if match else ''
def normalized(entry):
    tokens = shlex.split(entry['command'])
    result, i = [], 0
    while i < len(tokens):
        token = tokens[i]
        if token in ['-o', '-MF', '-MT', '-MQ', '/Fo', '/Fd']:
            i += 2
            continue
        if token.startswith(('/Fo', '/Fd')):
            i += 1
            continue
        if token.replace('\\', '/') == entry['file'].replace('\\', '/'):
            i += 1
            continue
        if 'cmake_pch' in token:
            prefix = token[:3] if token.startswith(('/Fp', '/FI', '/Yu')) else ''
            token = prefix + '<PCH>'
        result.append(token)
        i += 1
    return result
references = {configuration(e): e for e in entries if e['file'].replace('\\', '/').endswith('/ui2/widget_internal.cpp')}
assert references, 'Missing ordinary engine reference source'
for config, reference in references.items():
    expected = normalized(reference)
    for name in names:
        matches = [e for e in entries if e['file'].replace('\\', '/').endswith('/ui2/' + name + '.cpp') and configuration(e) == config]
        assert len(matches) == 1, (config, name, len(matches))
        entry = matches[0]
        assert 'kin_ui2_widgets.dir' in entry['command'], entry['command']
        actual = normalized(entry)
        if actual != expected:
            import difflib
            raise AssertionError('\n'.join(difflib.unified_diff(expected, actual, fromfile='engine flags', tofile=name)))
for target in ['kin_engine', 'kin_ui2_widgets']:
    producers = [e for e in entries if target + '.dir/' in e['file'].replace('\\', '/') and e['file'].endswith('cmake_pch.hxx.cxx')]
    assert producers, 'Missing PCH producer: ' + target
    for producer in producers:
        header = Path(producer['file']).with_suffix('').read_text()
        if target == 'kin_engine':
            assert 'widget_chunk_preamble.hpp' not in header and 'kin/ui2/' not in header
        else:
            assert 'widget_chunk_preamble.hpp' in header
print('Six widget sources preserve engine compiler flags; private PCH scope verified.')
