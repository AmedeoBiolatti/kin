"""Run inside Blender: fit Kenney's survivor mesh and authored clips to Summit's canonical rig.

blender -b --factory-startup -t 1 --python tools/assets/prepare_summit_character.py -- SOURCE_DIR OUTPUT.glb
The source directory contains Model/, Animations/ and Skins/ from the pinned survivor pack.
"""
import bpy
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys
from mathutils import Matrix, Vector, Quaternion

source_dir, output = map(Path, sys.argv[sys.argv.index('--') + 1:])
repo = Path(__file__).resolve().parents[2]
rig_source = repo / 'examples/sponza_walk/character.hpp'
text = rig_source.read_text().split('const Def defs[] = {', 1)[1].split('};', 1)[0]
defs = [(name, int(parent), Vector([float(v.strip().rstrip('f')) for v in at.split(',')]))
        for name, parent, at in re.findall(r'\{"([^"]+)",\s*(-?\d+),\s*\{([^}]+)\}\}', text)]
assert len(defs) == 25
names = [d[0] for d in defs]
world = []
for name, parent, local in defs:
    world.append(local + (world[parent] if parent >= 0 else Vector()))
source_names = {'pelvis': 'Hips', 'spine': 'Spine', 'chest': 'Chest', 'neck': 'Neck', 'head': 'Head'}
for side, prefix in [('L', 'Left'), ('R', 'Right')]:
    for target, source in [('hip', 'UpLeg'), ('knee', 'Leg'), ('ankle', 'Foot'), ('toe', 'Toes'),
                           ('shoulder', 'Arm'), ('elbow', 'ForeArm'), ('hand', 'Hand')]:
        source_names[target + '_' + side] = prefix + source

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=str(source_dir / 'Model/characterMedium.fbx'))
mesh = next(o for o in bpy.data.objects if o.type == 'MESH')
rig = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
# Blender Z-up to glTF Y-up, with the character facing +Z.
basis = Matrix.Rotation(-math.pi / 2, 4, 'X')
rest = {n: basis @ rig.matrix_world @ rig.data.bones[n].matrix_local for n in source_names.values()}
inverse_names = {v: k for k, v in source_names.items()}

def mapped(name):
    while name:
        if name in inverse_names:
            return inverse_names[name]
        bone = rig.data.bones.get(name)
        name = bone.parent.name if bone and bone.parent else None
    return 'pelvis'

ends = {n: world[names.index(child)] for n, child in [
    ('pelvis','spine'),('spine','chest'),('chest','neck'),('neck','head'),
    ('hip_L','knee_L'),('knee_L','ankle_L'),('ankle_L','toe_L'),
    ('hip_R','knee_R'),('knee_R','ankle_R'),('ankle_R','toe_R'),
    ('shoulder_L','elbow_L'),('elbow_L','hand_L'),('shoulder_R','elbow_R'),('elbow_R','hand_R')]}
fit = {}
for target, src in source_names.items():
    head = world[names.index(target)]
    end = ends.get(target, head + Vector((0, .22, 0) if target == 'head' else
                     ((.12 if target.endswith('_L') else -.12), 0, 0) if target.startswith('hand') else (0, 0, .1)))
    if target == 'chest': end = Vector((0,1.60,0))
    bone = rig.data.bones[src]
    a = basis @ (rig.matrix_world @ bone.head_local)
    b = basis @ (rig.matrix_world @ bone.tail_local)
    direction = end - head
    rotation = (b-a).rotation_difference(direction)
    ratio = direction.length / (b-a).length
    if target.startswith('hand'): ratio = .4
    radial = .5 if target in {'pelvis','spine','chest','neck'} or target.startswith(('hip','knee','shoulder','elbow')) else ratio
    if target == 'head': radial = .30
    fit[target] = (a, head, rotation, ratio, (b-a).normalized(), radial)
# The source has an extra chest segment and clavicles. Fit those vertices at their own anchors even though their
# skin weights collapse onto the canonical chest; treating all of them as the lower chest buries the head in the shirt.
for src, head, end in [('UpperChest',Vector((0,1.60,0)),Vector((0,1.70,0))),
                       ('LeftShoulder',Vector((.07,1.68,0)),Vector((.2,1.65,0))),
                       ('RightShoulder',Vector((-.07,1.68,0)),Vector((-.2,1.65,0)))]:
    bone = rig.data.bones[src]
    a = basis @ (rig.matrix_world @ bone.head_local)
    b = basis @ (rig.matrix_world @ bone.tail_local)
    fit[src] = (a,head,(b-a).rotation_difference(end-head),(end-head).length/(b-a).length,(b-a).normalized(),.5)


positions, uvs, joints, weights, normals = [], [], [], [], []
mesh.data.calc_loop_triangles()
uv = mesh.data.uv_layers.active.data
for triangle in mesh.data.loop_triangles:
    points = []
    for loop_index in triangle.loops:
        vertex = mesh.data.vertices[mesh.data.loops[loop_index].vertex_index]
        original = basis @ (mesh.matrix_world @ vertex.co)
        influences = {}
        for group in vertex.groups:
            target = mapped(mesh.vertex_groups[group.group].name)
            influences[target] = influences.get(target, 0) + group.weight
        influence = sorted(influences.items(), key=lambda x: -x[1])[:4] or [('pelvis', 1)]
        total = sum(w for _,w in influence)
        influence = [(n,w/total) for n,w in influence]
        point = Vector()
        original_total = sum(g.weight for g in vertex.groups) or 1
        for group in vertex.groups:
            group_name = mesh.vertex_groups[group.group].name
            a, head, rotation, ratio, axis, radial = fit.get(group_name, fit[mapped(group_name)])
            offset = original-a
            parallel = axis * offset.dot(axis)
            fitted = head + rotation @ (parallel*ratio + (offset-parallel)*radial)
            point += fitted * (group.weight/original_total)
        points.append(point)
        positions.append(tuple(point))
        uvs.append((uv[loop_index].uv.x, 1-uv[loop_index].uv.y))
        joints.append(tuple([names.index(n) for n,w in influence]+[0]*(4-len(influence))))
        weights.append(tuple([w for n,w in influence]+[0]*(4-len(influence))))
    normal = (points[1]-points[0]).cross(points[2]-points[0]).normalized()
    normals.extend([tuple(normal)]*3)
assert all(math.isfinite(v) for p in positions for v in p)

# Sample the original authored actions; transfer world-space rotation changes onto the canonical rest axes. FK preserves
# Summit's existing limb lengths, hand IK and ragdoll binding. Only the pelvis bob is retained; horizontal motion stays physics-owned.
clips = {}
for filename, title in [('idle','Idle'), ('run','Run'), ('jump','Jump')]:
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=str(source_dir / ('Animations/' + filename + '.fbx')))
    animated = next(o for o in set(bpy.data.objects)-before if o.type == 'ARMATURE')
    action = next(a for a in bpy.data.actions if a.name.endswith('|' + title))
    animated.animation_data_create(); animated.animation_data.action = action
    first, last = map(int, action.frame_range)
    rotations = [[] for _ in defs]; translations = []
    for frame in range(first, last+1):
        bpy.context.scene.frame_set(frame)
        bpy.context.view_layer.update()
        delta = [Quaternion() for _ in defs]
        for i,(name,parent,local) in enumerate(defs):
            if name in source_names:
                src = source_names[name]
                current = basis @ animated.matrix_world @ animated.pose.bones[src].matrix
                delta[i] = current.to_quaternion() @ rest[src].to_quaternion().inverted()
            elif parent >= 0:
                delta[i] = delta[parent].copy()
            local_q = (delta[parent].inverted() @ delta[i]) if parent >= 0 else delta[i]
            local_q.normalize()
            rotations[i].append((local_q.x,local_q.y,local_q.z,local_q.w))
        hips = basis @ animated.matrix_world @ animated.pose.bones['Hips'].matrix
        bob = (hips.translation.y-rest['Hips'].translation.y) * .6
        translations.append((0,1+max(-.12,min(.12,bob)),0))
    clips[title] = {'times': [(f-first)/30 for f in range(first,last+1)], 'rotations': rotations, 'pelvis': translations}
clips['Walk'] = dict(clips['Run'], times=[t*2 for t in clips['Run']['times']])
mid = len(clips['Jump']['times'])//2
clips['Fall'] = {'times':[0,1], 'rotations':[[r[mid],r[mid]] for r in clips['Jump']['rotations']], 'pelvis':[(0,1,0)]*2}

binary = bytearray(); views=[]; accessors=[]
def view(blob):
    while len(binary)%4: binary.append(0)
    i=len(views);views.append({'buffer':0,'byteOffset':len(binary),'byteLength':len(blob)});binary.extend(blob);return i

def accessor(rows, kind, component=5126, bounds=False):
    count=len(rows);components={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}[kind]
    fmt={5126:'f',5123:'H',5121:'B'}[component]
    blob=struct.pack('<'+fmt*(count*components),*(v for row in rows for v in row))
    a={'bufferView':view(blob),'componentType':component,'count':count,'type':kind}
    if bounds:
        a['min']=[min(r[k] for r in rows) for k in range(components)];a['max']=[max(r[k] for r in rows) for k in range(components)]
    accessors.append(a);return len(accessors)-1

attributes={n:accessor(rows,kind,component,bounds) for n,rows,kind,component,bounds in [
 ('POSITION',positions,'VEC3',5126,True),('NORMAL',normals,'VEC3',5126,False),('TEXCOORD_0',uvs,'VEC2',5126,False),
 ('JOINTS_0',joints,'VEC4',5121,False),('WEIGHTS_0',weights,'VEC4',5126,False)]}
indices=accessor([(i,) for i in range(len(positions))],'SCALAR',5123)
ibm=accessor([tuple(v for col in Matrix.Translation(-p).transposed() for v in col) for p in world],'MAT4')
image=view((source_dir/'Skins/survivorMaleB.png').read_bytes())
nodes=[]
for i,(name,parent,local) in enumerate(defs):
    node={'name':name,'translation':list(local)}
    children=[j for j,d in enumerate(defs) if d[1]==i]
    if children:node['children']=children
    nodes.append(node)
nodes.append({'name':'Survivor','mesh':0,'skin':0})
animations=[]
for name,clip in clips.items():
    times=accessor([(t,) for t in clip['times']],'SCALAR',bounds=True)
    samplers=[];channels=[]
    def channel(joint,path,rows,kind):
        sampler=len(samplers);samplers.append({'input':times,'output':accessor(rows,kind),'interpolation':'LINEAR'})
        channels.append({'sampler':sampler,'target':{'node':joint,'path':path}})
    for i,rows in enumerate(clip['rotations']):channel(i,'rotation',rows,'VEC4')
    channel(names.index('pelvis'),'translation',clip['pelvis'],'VEC3')
    animations.append({'name':name,'samplers':samplers,'channels':channels})
gltf={'asset':{'version':'2.0','generator':'Kin Summit CC0 survivor fitter'},'scene':0,'scenes':[{'nodes':[0,len(defs)]}],
 'nodes':nodes,'meshes':[{'primitives':[{'attributes':attributes,'indices':indices,'material':0}]}],
 'skins':[{'name':'SummitCanonical','joints':list(range(len(defs))),'skeleton':0,'inverseBindMatrices':ibm}],
 'materials':[{'name':'Kenney survivorMaleB','pbrMetallicRoughness':{'baseColorTexture':{'index':0},'metallicFactor':0,'roughnessFactor':.85}}],
 'images':[{'bufferView':image,'mimeType':'image/png'}],'textures':[{'source':0,'sampler':0}],
 'samplers':[{'magFilter':9728,'minFilter':9728}],'animations':animations,'buffers':[{'byteLength':len(binary)}],
 'bufferViews':views,'accessors':accessors}
encoded=json.dumps(gltf,separators=(',',':')).encode();encoded+=b' '*((-len(encoded))%4);binary+=b'\0'*((-len(binary))%4)
blob=struct.pack('<III',0x46546c67,2,12+8+len(encoded)+8+len(binary))+struct.pack('<II',len(encoded),0x4e4f534a)+encoded+struct.pack('<II',len(binary),0x004e4942)+binary
output.parent.mkdir(parents=True,exist_ok=True);output.write_bytes(blob)
print('SURVIVOR',len(positions)//3,'triangles',len(defs),'joints',list(clips),len(blob),'bytes')
