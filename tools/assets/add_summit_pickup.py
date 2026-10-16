#!/usr/bin/env python3
"""Append Kin's small Pickup clip to the fitted CC0 survivor, without Blender or mesh copies."""
import hashlib
import json
import math
from pathlib import Path
import struct

REPO = Path(__file__).resolve().parents[2]

def add_pickup(path):
    blob = path.read_bytes()
    size = struct.unpack_from('<I', blob, 12)[0]
    gltf = json.loads(blob[20:20+size])
    existing = next((a for a in gltf['animations'] if a['name'] == 'Pickup'),None)
    binary = bytearray(blob[28+size:])
    # Remove only GLB alignment padding, retaining the complete original buffer.
    del binary[gltf['buffers'][0]['byteLength']:]
    if existing:
        assert existing is gltf['animations'][-1] and existing.get('extras',{}).get('kin_authored')=='left reach v1', 'Unexpected Pickup clip; refusing to overwrite'
        first_accessor=min(min(s['input'],s['output']) for s in existing['samplers'])
        first_view=min(a['bufferView'] for a in gltf['accessors'][first_accessor:])
        first_byte=min(v.get('byteOffset',0) for v in gltf['bufferViews'][first_view:])
        assert all(s['input']<first_accessor and s['output']<first_accessor for a in gltf['animations'][:-1] for s in a['samplers'])
        del gltf['animations'][-1]; del gltf['accessors'][first_accessor:]; del gltf['bufferViews'][first_view:]; del binary[first_byte:]
    def read(index):
        a = gltf['accessors'][index]; v = gltf['bufferViews'][a['bufferView']]
        width = {'SCALAR':1,'VEC4':4}[a['type']]
        assert a['componentType'] == 5126
        return struct.unpack_from('<'+'f'*width, binary, v.get('byteOffset',0)+a.get('byteOffset',0))
    idle = next(a for a in gltf['animations'] if a['name'] == 'Idle')
    rest = {c['target']['node']:read(idle['samplers'][c['sampler']]['output'])
            for c in idle['channels'] if c['target']['path'] == 'rotation'}
    def accessor(rows, kind):
        while len(binary)%4: binary.append(0)
        offset=len(binary); width=len(rows[0])
        binary.extend(struct.pack('<'+'f'*(len(rows)*width),*(x for r in rows for x in r)))
        view=len(gltf['bufferViews']);gltf['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':len(rows)*width*4})
        index=len(gltf['accessors']);a={'bufferView':view,'componentType':5126,'count':len(rows),'type':kind}
        if kind=='SCALAR':a.update(min=[rows[0][0]],max=[rows[-1][0]])
        gltf['accessors'].append(a);return index
    def axis(axis,degrees):
        v=[0.,0.,0.,math.cos(math.radians(degrees)/2)];v[axis]=math.sin(math.radians(degrees)/2);return v
    def mul(a,b):
        x,y,z,w=a;X,Y,Z,W=b
        return (w*X+x*W+y*Z-z*Y,w*Y-x*Z+y*W+z*X,w*Z+x*Y-y*X+z*W,w*W-x*X-y*Y-z*Z)
    # An in-place left-handed reach. Legs keep their normal locomotion under the
    # spine mask; the right hand continues to carry the lantern.
    reach={'spine':axis(0,35),'chest':axis(0,18),'neck':axis(0,-15),
           'shoulder_L':mul(axis(1,-55),axis(2,-65)), 'elbow_L':axis(1,-25),'hand_L':axis(0,-10)}
    times=accessor([(t,) for t in [0,.10,.16,.25,.40]],'SCALAR')
    animation={'name':'Pickup','samplers':[],'channels':[], 'extras':{'kin_authored':'left reach v1'}}
    for node,q in rest.items():
        name=gltf['nodes'][node]['name']; target=reach.get(name,q)
        # Torso/neck offsets apply relative to the fitted idle; arm reach uses
        # canonical rest axes rather than retargeted source arm rotations.
        if name in ['spine','chest','neck']:target=mul(q,target)
        middle=tuple((x+y)/2 for x,y in zip(q,target));norm=math.sqrt(sum(x*x for x in middle));middle=tuple(x/norm for x in middle)
        values=accessor([q,middle,target,target,q],'VEC4')
        sampler=len(animation['samplers']);animation['samplers'].append({'input':times,'output':values,'interpolation':'LINEAR'})
        animation['channels'].append({'sampler':sampler,'target':{'node':node,'path':'rotation'}})
    gltf['animations'].append(animation);gltf['buffers'][0]['byteLength']=len(binary)
    encoded=json.dumps(gltf,separators=(',',':')).encode();encoded+=b' '*((-len(encoded))%4);binary+=b'\0'*((-len(binary))%4)
    output=struct.pack('<III',0x46546c67,2,28+len(encoded)+len(binary))+struct.pack('<II',len(encoded),0x4e4f534a)+encoded+struct.pack('<II',len(binary),0x004e4942)+binary
    path.write_bytes(output)

if __name__=='__main__':
    path=REPO/'examples/summit/content/kenney-character/survivor.glb'
    add_pickup(path)
    manifest_path=path.with_name('manifest.json')
    if manifest_path.exists():
        manifest=json.loads(manifest_path.read_text())
        suffix=' Kin adds an authored in-place Pickup reach; original mesh, texture and existing clips are unchanged.'
        if suffix not in manifest['modifications']:manifest['modifications']+=suffix
        manifest['pickup_tool_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        manifest['files'][path.name]={'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
