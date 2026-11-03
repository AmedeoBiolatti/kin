"""Regenerate this demo's small procedural source images; standard Python only."""
from pathlib import Path
import math
import json
import struct
import zlib

root = Path(__file__).resolve().parent

def png(path, size, pixel):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    width, height = size
    raw = b''.join(b'\0' + bytes(v for x in range(width) for v in pixel(x, y)) for y in range(height))
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

png(root / 'pulse.png', (8, 8), lambda x,y: (255,225,112,255 if (x-3.5)**2+(y-3.5)**2<13 else 0))
png(root / 'drone.png', (16, 16), lambda x,y: (245,103+int(y*4),105,255 if abs(x-7.5)+abs(y-7.5)<7.5 else 0))
images = root.parent / 'content' / 'images'
png(images / 'floor.png', (32,32), lambda x,y: ((68,91,112,255) if (x//8+y//8)%2 else (83,108,128,255)))
png(images / 'normal.png', (32,32), lambda x,y: (int(128+45*math.sin(x*math.pi/4)),int(128+45*math.sin(y*math.pi/4)),240,255))
png(images / 'specular.png', (32,32), lambda x,y: (190,190,190,255))
png(images / 'hero.png', (32,32), lambda x,y: (112,232,211,255) if 6<x<25 and 3<y<29 else (50,97,112,255))

def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n')

def vec(x,y):
    return {'x': x, 'y': y}

vertices = []
for bone, points in [(0,[(-12,-15),(12,-15),(12,15),(-12,15)]), (1,[(0,-4),(18,-4),(18,4),(0,4)])]:
    for i,(x,y) in enumerate(points):
        vertices.append({'position':vec(x+(8 if bone else 0),y), 'uv':vec(1 if i in (1,2) else 0,1 if i>=2 else 0),
                         'influences':[{'bone':bone,'weight':1}]})
clips = {'idle': {'duration':1,'tracks':[]}}
for name,angle in [('left',-8),('right',8),('up',-4),('down',4)]:
    clips[name] = {'duration':.6, 'tracks':[{'bone':0,'keys':[
        {'time':0,'rotation':angle}, {'time':.3,'position':vec(0,-3),'rotation':-angle}, {'time':.6,'rotation':angle}]}]}
clips['fire'] = {'duration':.3,'tracks':[{'bone':1,'keys':[
    {'time':0,'position':vec(8,0)}, {'time':.1,'position':vec(8,0),'rotation':-45}, {'time':.3,'position':vec(8,0)}]}]}
write_json(root.parent/'content'/'hero.kinskel', {
    'schema':'kin.skeleton2d/1', 'texture_asset':'images/hero.png',
    'bones':[{'name':'body'},{'name':'arm','parent':0,'position':vec(8,0),'length':18}],
    'vertices':vertices, 'indices':[0,1,2,0,2,3,4,5,6,4,6,7],
    'attachments':[{'name':'muzzle','bone':1,'position':vec(18,0)}], 'clips':clips,
    'blend_spaces':{'movement':{'points':[{'position':vec(x,y),'clip':name} for x,y,name in
        [(0,0,'idle'),(-1,0,'left'),(0,-1,'up'),(1,0,'right'),(0,1,'down')]],
        'triangles':[[0,1,2],[0,2,3],[0,3,4],[0,4,1]]}}})
for name,color in [('amber',{'r':255,'g':178,'b':100,'a':255}),('azure',{'r':90,'g':190,'b':255,'a':255})]:
    entities = [
        {'id':'lighting','components':{'SceneLighting':{'forward_sprites':True,'ambient':{'r':70,'g':74,'b':88,'a':255}}}},
        {'id':'floor','components':{'Transform2D':{'pos':vec(24,72)},'NormalSpriteRenderer2D':{
            'albedo_asset':'images/floor.png','normal_asset':'images/normal.png','specular_asset':'images/specular.png',
            'pivot':vec(0,0),'size':vec(656,488),'specular_strength':.25}}},
        {'id':'lamp','components':{'Transform2D':{'pos':vec(240,240)},'LightComponent':{
            'radius':340,'height':100,'color':color,'intensity':2,'shadows':True,'shadow_softness':3}}},
        {'id':'sun','components':{'DirectionalLight2D':{'direction':vec(.2,-.5),'intensity':.3}}}
    ]
    for y,height in [(208,160),(424,176)]:
        entities.append({'id':'wall'+str(y),'components':{'Transform2D':{'pos':vec(360,y)},
            'RectRenderer':{'offset':vec(-8,-height/2),'size':vec(16,height),'color':{'r':53,'g':75,'b':96,'a':255}},
            'LightOccluder':{'size':vec(16,height)}}})
    write_json(root.parent/'content'/'rooms'/(name+'.kinscene'), {'schema':'kin.scene/1','name':name.title(),'entities':entities})
