#!/usr/bin/env python3
"""Bake bounded, engine-neutral industrial yard geometry and animation (YRD1)."""
import argparse
import hashlib
import math
from pathlib import Path
import struct

TIERS = {'small': (2000, 16, 32, 128, 2, 8, 2),
         'medium': (8000, 64, 128, 512, 10, 16, 4),
         'heavy': (20000, 128, 256, 2000, 25, 16, 8)}
ROUTE, POSES, JOINTS = 1440, 120, 6
PIVOTS = [(0, 1.1, 0), (0, 1.9, 0), (-.55, 1.55, 0), (.55, 1.55, 0), (-.22, .85, 0), (.22, .85, 0)]

def hashed(n):
    n = (n ^ (n >> 16)) * 0x7feb352d & 0xffffffff
    n = (n ^ (n >> 15)) * 0x846ca68b & 0xffffffff
    return n ^ (n >> 16)

def rand(n, lane=0):
    return (hashed((n * 17 + lane * 0x9e3779b9) & 0xffffffff) & 0xffffff) / 16777216

def add(a, b): return tuple(x+y for x,y in zip(a,b))
def mul(a, k): return tuple(x*k for x in a)
def dot(a, b): return sum(x*y for x,y in zip(a,b))
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def norm(a): return mul(a, 1/max(math.sqrt(dot(a,a)), 1e-20))

def transform(p=(0,0,0), scale=(1,1,1), yaw=0):
    c, s = math.cos(yaw), math.sin(yaw)
    return [c*scale[0],0,-s*scale[0],0, 0,scale[1],0,0,
            s*scale[2],0,c*scale[2],0, *p,1]

class Mesh:
    def __init__(self, skin=False): self.vertices=[]; self.indices=[]; self.skin=skin
    def vertex(self, p, n, uv, joint=0):
        self.vertices.append((p,n,uv,joint)); return len(self.vertices)-1
    def quad(self, points, n, joint=0):
        first=len(self.vertices)
        for p,uv in zip(points,[(0,0),(1,0),(1,1),(0,1)]):self.vertex(p,n,uv,joint)
        self.indices.extend(first+i for i in [0,1,2,0,2,3])
    def box(self, size=(1,1,1), centre=(0,0,0), joint=0):
        x,y,z=(v*.5 for v in size)
        for c,u,v,n in [((x,0,0),(0,0,-z),(0,y,0),(1,0,0)),((-x,0,0),(0,0,z),(0,y,0),(-1,0,0)),
                         ((0,y,0),(x,0,0),(0,0,-z),(0,1,0)),((0,-y,0),(x,0,0),(0,0,z),(0,-1,0)),
                         ((0,0,z),(x,0,0),(0,y,0),(0,0,1)),((0,0,-z),(-x,0,0),(0,y,0),(0,0,-1))]:
            self.quad([add(centre,add(c,add(mul(u,a),mul(v,b)))) for a,b in [(-1,-1),(1,-1),(1,1),(-1,1)]],n,joint)
    def cylinder(self, radius=.5, height=1, centre=(0,0,0), segments=24, joint=0):
        first=len(self.vertices)
        for y in [-height*.5,height*.5]:
            for i in range(segments+1):
                a=i*math.tau/segments;n=(math.cos(a),0,math.sin(a))
                self.vertex(add(centre,(n[0]*radius,y,n[2]*radius)),n,(i/segments,(y/height+.5)),joint)
        for i in range(segments):
            a=first+i;b=a+1;d=a+segments+1;c=d+1
            self.indices.extend([a,c,b,a,d,c])
        for sign in [-1,1]:
            c=self.vertex(add(centre,(0,sign*height*.5,0)),(0,sign,0),(.5,.5),joint)
            ring=[]
            for i in range(segments):
                a=i*math.tau/segments
                ring.append(self.vertex(add(centre,(radius*math.cos(a),sign*height*.5,radius*math.sin(a))),(0,sign,0),(.5+.5*math.cos(a),.5+.5*math.sin(a)),joint))
            for i in range(segments):
                a,b=ring[i],ring[(i+1)%segments];self.indices.extend([c,b,a] if sign>0 else [c,a,b])
    def torus(self, major=.5, minor=.13, centre=(0,0,0), fraction=1, rings=32, sides=12):
        first=len(self.vertices)
        for i in range(rings+1):
            a=i*math.tau*fraction/rings
            for j in range(sides+1):
                b=j*math.tau/sides;n=(math.cos(a)*math.cos(b),math.sin(b),math.sin(a)*math.cos(b))
                p=((major+minor*math.cos(b))*math.cos(a),minor*math.sin(b),(major+minor*math.cos(b))*math.sin(a))
                self.vertex(add(centre,p),n,(i/rings,j/sides))
        for i in range(rings):
            for j in range(sides):
                a=first+i*(sides+1)+j;b=a+sides+1
                self.indices.extend([a,a+1,b+1,a,b+1,b])
    def tangents(self):
        ts=[[0.,0.,0.] for _ in self.vertices];bs=[[0.,0.,0.] for _ in self.vertices]
        for i in range(0,len(self.indices),3):
            ids=self.indices[i:i+3];a,b,c=[self.vertices[k] for k in ids]
            e1=add(b[0],mul(a[0],-1));e2=add(c[0],mul(a[0],-1));u1,v1=add(b[2],mul(a[2],-1));u2,v2=add(c[2],mul(a[2],-1))
            d=u1*v2-u2*v1
            if abs(d)<1e-10:continue
            t=mul(add(mul(e1,v2),mul(e2,-v1)),1/d);bt=mul(add(mul(e2,u1),mul(e1,-u2)),1/d)
            for k in ids:ts[k]=add(ts[k],t);bs[k]=add(bs[k],bt)
        out=[]
        for i,(_,n,_,_) in enumerate(self.vertices):
            t=norm(add(ts[i],mul(n,-dot(n,ts[i]))))
            if dot(t,t)<.5:t=norm(cross((0,1,0) if abs(n[1])<.9 else (1,0,0),n))
            out.extend((*t,-1 if dot(cross(n,t),bs[i])<0 else 1))
        return out

def meshes():
    out=[];m=Mesh();m.box();out.append(m)
    m=Mesh();m.cylinder(.44,1.15)
    for y in [-.48,0,.48]:m.torus(.44,.035,(0,y,0),rings=24,sides=8)
    out.append(m)
    m=Mesh();m.box((.9,.55,.75),(0,.15,0));m.cylinder(.27,.9,(0,.6,0))
    for x in [-.38,.38]:m.box((.12,.18,1.1),(x,-.25,0))
    m.torus(.28,.075,(.55,.2,0),fraction=.75,rings=24);out.append(m)
    m=Mesh();m.cylinder(.36,.95)
    for i in range(10):m.box((.9,.035,.9),(0,-.4+i*.09,0))
    m.box((1.1,.12,.85),(0,-.55,0));out.append(m)
    m=Mesh();m.torus(.55,.18,fraction=.25,rings=24,sides=16);m.cylinder(.23,.12,(.55,0,0));out.append(m)
    m=Mesh()
    for i in range(7):m.box((1.4,.08,.12),(0,.13,i*.18-.54))
    for x in [-.5,0,.5]:m.box((.12,.2,1.3),(x,0,0))
    out.append(m)
    m=Mesh();m.torus(.42,.08,rings=32,sides=12);m.cylinder(.13,.22)
    m.box((.9,.06,.07));m.box((.07,.06,.9));out.append(m)
    m=Mesh();m.box((.75,.8,.75))
    for y in [-.32,.32]:
        for x in [-.4,.4]:m.box((.1,.13,.95),(x,y,0))
        for z in [-.4,.4]:m.box((.95,.13,.1),(0,y,z))
    out.append(m)
    m=Mesh(True)
    for size,centre,j in [((.8,.9,.5),(0,1.3,0),0),((.65,.5,.55),(0,2.05,0),1),
                          ((.25,.9,.3),(-.65,1.15,0),2),((.25,.9,.3),(.65,1.15,0),3),
                          ((.28,.8,.4),(-.22,.4,0),4),((.28,.8,.4),(.22,.4,0),5)]:m.box(size,centre,j)
    for x,j in [(-.65,2),(.65,3)]:m.cylinder(.18,.24,(x,1.6,0),16,j)
    out.append(m)
    m=Mesh();m.quad([(-.5,0,0),(.5,0,0),(.5,1,0),(-.5,1,0)],(0,0,1));out.append(m)
    return out

KEYS=[((-32,5,28),(-6,2,0)),((-10,3,16),(0,3,-15)),((0,3,-14),(6,2,-25)),
      ((8,3,-27),(0,2,-7)),((0,3,-4),(23,3,14)),((26,16,22),(0,2,-10))]

def lerp(a,b,t):return tuple(x+(y-x)*t for x,y in zip(a,b))
def camera_route():
    result=[]
    for i in range(ROUTE):
        p=i*len(KEYS)/ROUTE;k=int(p);t=p-k;t=t*t*(3-2*t)
        result.append((*lerp(KEYS[k][0],KEYS[(k+1)%len(KEYS)][0],t),*lerp(KEYS[k][1],KEYS[(k+1)%len(KEYS)][1],t)))
    return result

def lane(x,z):
    for i in range(len(KEYS)):
        a=KEYS[i][0];b=KEYS[(i+1)%len(KEYS)][0]
        if min(a[1],b[1])>8:continue
        dx,dz=b[0]-a[0],b[2]-a[2];t=max(0,min(1,((x-a[0])*dx+(z-a[2])*dz)/max(dx*dx+dz*dz,1e-6)))
        if (x-a[0]-dx*t)**2+(z-a[2]-dz*t)**2<3.0**2:return True
    return False

def generate(path,tier):
    props,materials,actors,smoke,percent,points,spots=TIERS[tier];assets=meshes();world=[]
    def obj(mesh,mat,p,scale=(1,1,1),yaw=0,flag=0):return (mesh,mat,flag,transform(p,scale,yaw))
    def box(mat,p,scale):world.append(obj(0,mat,p,scale))
    for z in range(10):
        for x in range(10):box(0,(-36+x*8,-.2,-36+z*8),(7.98,.4,7.98))
    for x in [-16,16]:box(2,(x,5,-22),(.5,10,28))
    box(2,(0,5,-36),(32,10,.5));box(1,(0,10,-22),(33,.4,29))
    for x in [-10,10]:box(2,(x,1.25,-8),(12,2.5,.4))
    box(1,(0,9,-8),(32,2,.5))
    for x in [-16,-12,-8,-4,4,8,12,16]:box(3,(x,5.2,-8),(.2,7.4,.6))
    for x in [-14,-10,-6,6,10,14]:world.append(obj(9,materials,(x,2.5,-7.75),(3.7,5.4,1)))
    for z in [-34,-26,-18,-10]:
        box(3,(0,9.3,z),(32,.4,.4))
        for x in [-15,15]:box(3,(x,4.6,z),(.35,9,.35))
    for x in [-34,-26,26,34]:
        for z in [-28,-12,8,24]:
            if not lane(x,z):
                world.append(obj(1,4,(x,2,z),(2.5,4,2.5)))
                box(3,(x,.3,z),(4,.6,4))
    for z in range(-36,40,4):
        for x in [-40,40]:box(3,(x,1.4,z),(.16,2.8,.16))
    for x in [-40,40]:
        for y in [.8,2.2]:box(3,(x,y,0),(.1,.1,76))
    for x in [-30,30]:
        for z in [-32,-16,0,16,32]:box(3,(x,6,z),(.25,12,.25))
        box(3,(x,12,0),(.3,.4,64))
    moving_count=props*percent//100
    # Guarantee every material group is edited, even in the 40-mover Small tier.
    moving=set(range(materials))
    moving.update(sorted(range(materials,props),key=lambda i:hashed(i+1234))[:moving_count-materials])
    objects=[];attempt=0
    while len(objects)<props:
        i=len(objects);x=-37+rand(attempt,1)*74;z=-34+rand(attempt,2)*70;attempt+=1
        if lane(x,z) or (abs(abs(x)-16)<1 and z<-8) or abs(z+8)<1.5 or abs(z+36)<1:continue
        mat=(i*17)%materials;kind=mat%8;size=.45+rand(i,3)*.8
        ground=-min(v[0][1] for v in assets[kind].vertices)*size+.03
        objects.append(obj(kind,mat,(x,ground,z),(size,size,size),rand(i,4)*math.tau,int(i in moving)))
    robots=[];attempt=0
    while len(robots)<actors:
        x=-30+rand(attempt+30000,1)*60;z=-30+rand(attempt+30000,2)*56;attempt+=1
        if lane(x,z) or (abs(abs(x)-16)<1 and z<-8) or abs(z+8)<1.5:continue
        robots.append(obj(8,5+len(robots)%(materials-5),(x,0,z),(1,1,1),rand(attempt,8)*math.tau))
    clouds=[]
    for i in range(smoke):
        zone=i%8;x=[-24,-12,12,24,-10,10,-30,30][zone]+(rand(i,10)-.5)*6
        z=[18,8,18,8,-24,-24,-12,-12][zone]+(rand(i,11)-.5)*6
        clouds.append(obj(9,materials+1,(x,.5+rand(i,12)*5,z),(2+rand(i,13)*3,2+rand(i,14)*3,1),rand(i,15)*math.tau))
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    with path.open('wb') as f:
        def u(*v):f.write(struct.pack('<'+'I'*len(v),*v))
        def fl(*v):f.write(struct.pack('<'+'f'*len(v),*v))
        u(0x31445259,1,list(TIERS).index(tier),props,materials,actors,smoke,len(world),len(assets),ROUTE,POSES,JOINTS,points,spots,moving_count,1)
        for mesh in assets:
            u(len(mesh.vertices),len(mesh.indices),JOINTS if mesh.skin else 0)
            for field in [0,1,2]:fl(*(v for row in mesh.vertices for v in row[field]))
            fl(*mesh.tangents());u(*mesh.indices)
            if mesh.skin:
                for row in mesh.vertices:u(row[3],0,0,0)
                for _ in mesh.vertices:fl(1,0,0,0)
        palette=[(135,145,155),(95,115,130),(105,115,122),(210,160,55),(70,125,130),(190,95,45),(110,120,135),(155,145,115)]
        for i in range(materials+2):
            col=palette[i%len(palette)];alpha=255;mode=0
            if i==materials:col=(80,170,210);alpha=55;mode=1
            if i==materials+1:col=(160,168,175);alpha=130;mode=2
            u(col[0]|col[1]<<8|col[2]<<16|alpha<<24,mode);fl(.15 if mode else .3+(i%7)*.09,0 if mode else (i%5)*.2)
        for kind in range(5):
            side=64 if kind==4 else 128;u(side);data=bytearray()
            for y in range(side):
                for x in range(side):
                    noise=hashed(x+y*side)%22
                    if kind==0:v=(170+noise,170+noise,170+noise,255)
                    elif kind==1:v=(230,140+(x%32)*2,255,255)
                    elif kind==2:v=(128+int(5*math.sin(x*.4)),128+int(5*math.cos(y*.4)),254,255)
                    elif kind==3:v=(230,130,30,255) if x%64<2 and y%64<12 else (0,0,0,255)
                    else:
                        d=((x/(side-1)-.5)**2+(y/(side-1)-.5)**2)*4;v=(220,225,230,round(max(0,1-d)**2*50))
                    data.extend(v)
            f.write(data)
        u(128,64)
        for y in range(64):
            for x in range(128):
                t=y/63;bright=.9 if 76<=x<90 and 20<=y<28 else 0
                fl(.035*(1-t)+.01*t+bright,.05*(1-t)+.015*t+bright*.65,.09*(1-t)+.025*t+bright*.35)
        for row in camera_route():fl(*row)
        for record in world+objects+robots+clouds:
            mesh,mat,flag,tr=record;u(mesh,mat,flag);fl(*tr)
        for p in PIVOTS:fl(*p)
        for frame in range(POSES):
            a=frame*math.tau/POSES
            for j,p in enumerate(PIVOTS):
                angle=0 if j==0 else (math.sin(a+(math.pi if j in [3,4] else 0))*(.08 if j==1 else .55))
                c,s=math.cos(angle),math.sin(angle)
                skin=[c,s,0,0,-s,c,0,0,0,0,1,0,p[0]-c*p[0]+s*p[1],p[1]-s*p[0]-c*p[1],0,1]
                pose=[c,s,0,0,-s,c,0,0,0,0,1,0,*p,1];fl(*skin,*pose)
        for frame in range(POSES):
            a=frame*math.tau/POSES;fl(*transform((.25*math.sin(a),.12*math.sin(a*2),.2*math.cos(a)),yaw=.3*math.sin(a)))
        positions=[(-24,13,20),(24,13,16),(-10,8,-25),(10,8,-25),(-28,12,-12),(28,12,-12),(-10,10,30),(10,10,30)]
        for i in range(points+spots):
            spot=i>=points;j=i-points if spot else i
            p=positions[j%8] if spot else (-30+(j%4)*20,5+(j%2),-26+(j//4)*18)
            target=(p[0]*.55,0,p[2]*.75);colour=(500,380,240) if spot else (95,65,35) if j%2 else (40,65,110)
            fl(*p,*target,*colour,60 if spot else 22,45);u(int(spot))
    source_triangles=sum(len(assets[r[0]].indices)//3 for r in world+objects+robots+clouds)
    info={'tier':tier,'props':props,'materials':materials,'actors':actors,'smoke':smoke,'moving_props':moving_count,'moving_material_groups':len({objects[i][1] for i in moving}),'static_structure_objects':len(world),'meshes':len(assets),'source_mesh_triangles':sum(len(m.indices)//3 for m in assets),'scene_triangles':source_triangles,'points':points,'shadowed_spots':spots,'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
    return info

if __name__=='__main__':
    import json
    p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--tier',choices=TIERS,default='medium');a=p.parse_args()
    print(json.dumps(generate(a.output,a.tier),indent=2))
