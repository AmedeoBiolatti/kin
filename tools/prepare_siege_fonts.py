#!/usr/bin/env python3
"""Explicit offline font import; requires FontTools 4.60.1, never runs during builds."""
import argparse
import hashlib
import json
import unicodedata
from pathlib import Path
import fontTools
from fontTools import subset
from fontTools.ttLib import TTFont

ROOT=Path(__file__).resolve().parents[1]


def strings(value):
    if isinstance(value,str): yield value
    elif isinstance(value,dict):
        for key,item in value.items():
            yield key; yield from strings(item)
    elif isinstance(value,list):
        for item in value: yield from strings(item)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('latin','arabic','japanese','core-license','cjk-license'):
        parser.add_argument('--'+name,required=True,type=Path)
    args=parser.parse_args()
    if fontTools.__version__!='4.60.1': raise ValueError('Use FontTools 4.60.1')
    output=ROOT/'examples/content/lang/fonts';output.mkdir(exist_ok=True)
    texts=''.join(s for path in sorted((ROOT/'examples/content/lang').glob('*.kinlang')) for s in strings(json.loads(path.read_text(encoding='utf-8'))))
    points={ord(c) for c in texts}|set(range(32,256))|{0x2026,0x2190,0x2191,0x2192,0x2193,0x2212,0x25b6,0x25c0}
    points={p for p in points if p>=32 and unicodedata.category(chr(p)) != "Cc"}
    records=[];coverage=set()
    for key,family in [('latin','Kin Siege Sans'),('arabic','Kin Siege Arabic'),('japanese','Kin Siege Japanese')]:
        source=getattr(args,key)
        font=TTFont(source,fontNumber=0,recalcTimestamp=False)
        original_name=font['name'].getDebugName(1)
        copyright=font['name'].getDebugName(0)
        options=subset.Options();options.recalc_timestamp=False;options.layout_features=['*'];options.name_IDs=['*'];options.name_languages=['*']
        selected=points|(set(range(0x600,0x700)) if key=='arabic' else set())
        worker=subset.Subsetter(options=options);worker.populate(unicodes=selected);worker.subset(font)
        for record in list(font['name'].names):
            names={1:family,2:'Regular',3:family+' subset v1',4:family+' Regular',6:family.replace(' ','')+'-Regular',16:family,17:'Regular'}
            if record.nameID in names:
                font['name'].setName(names[record.nameID],record.nameID,record.platformID,record.platEncID,record.langID)
        if 'CFF ' in font:
            top=font['CFF '].cff.topDictIndex[0];top.FullName=family+' Regular';top.FamilyName=family
            font['CFF '].cff.fontNames=[family.replace(' ','')+'-Regular']
        name=f'{key}.otf' if 'CFF ' in font else f'{key}.ttf'
        font.save(output/name,reorderTables=True)
        coverage.update(font.getBestCmap())
        records.append({'path':name,'source_name':original_name,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
                        'copyright':copyright,'sha256':hashlib.sha256((output/name).read_bytes()).hexdigest(),'bytes':(output/name).stat().st_size})
    missing=points-coverage
    if missing: raise ValueError('Font chain lacks '+str(sorted(missing)))
    for key,source in [('core',args.core_license),('cjk',args.cjk_license)]:
        text=source.read_text(encoding='utf-8');label='License: OFL-1.1\n' if key=='core' else 'License: SIL-1.1\n';start=text.rindex(label);text=text[start+len(label):].split('\nLicense: ',1)[0]
        license='\n'.join('' if line==' .' else line[1:] if line.startswith(' ') else line for line in text.splitlines())
        copyrights='\n'.join(r['copyright'] or '' for r in records if (r['path'].startswith('japanese'))==(key=='cjk'))
        (output/f'{key}-OFL.txt').write_text(copyrights+'\n\nSIL OPEN FONT LICENSE Version 1.1 - 26 February 2007\n\n'+license+'\n', encoding='utf-8')
    (output/'provenance.json').write_text(json.dumps({'schema':'kin.bundled-fonts/1','fonttools':fontTools.__version__,
        'sources':['https://github.com/notofonts/noto-fonts','https://github.com/notofonts/noto-cjk'],
        'recipe':'Explicit subsets, face 0; Latin-1 plus locale text and UI punctuation; Arabic shaping closure; renamed families; no build-time system font import.',
        'files':records,'required_codepoints':sorted(points)},indent=2,ensure_ascii=False)+'\n', encoding='utf-8')
    print([(r['path'],r['bytes']) for r in records])


if __name__=='__main__': main()
