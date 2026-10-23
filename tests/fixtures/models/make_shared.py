"""Tiny glTF fixtures sharing an external image, with distinct sampling semantics."""
import base64
import copy
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'tools/models'))
from glb import Glb, quad, png

root = Path(__file__).resolve().parent
model = Glb()
material = model.material('shared', unlit=True)
model.node('quad', mesh=model.mesh('quad', *quad(2, 2), material=material))
model.json['images'] = [{'uri': 'shared%20pixel.png'}]
model.json['textures'] = [{'source': 0, 'sampler': 0}]
model.json['samplers'] = [{'magFilter': 9728}]
model.json['buffers'] = [{'byteLength': len(model.bin), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(model.bin).decode()}]
model.json['extensionsUsed'] = sorted(model.extensions)
model.json['materials'][0]['pbrMetallicRoughness']['baseColorTexture'] = {'index': 0}
(root / 'shared pixel.png').write_bytes(png(2, 2, [128, 128, 255, 255] * 4))
for name in ['a', 'b', 'filtered']:
    doc = copy.deepcopy(model.json)
    if name == 'a':
        doc['materials'][0]['normalTexture'] = {'index': 0}
        doc['materials'][0]['pbrMetallicRoughness']['metallicRoughnessTexture'] = {'index': 0}
        doc['materials'][0]['occlusionTexture'] = {'index': 0}
    elif name == 'b':
        doc['images'][0]['uri'] = './shared%20pixel.png'
    else:
        doc['samplers'][0]['magFilter'] = 9729
    (root / f'shared-{name}.gltf').write_text(json.dumps(doc, indent=2) + '\n')
