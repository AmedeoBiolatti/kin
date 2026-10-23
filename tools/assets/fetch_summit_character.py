#!/usr/bin/env python3
"""Fetch the pinned CC0 survivor sources and prepare a glTF with an installed Blender. No editor install."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

SOURCE='https://kenney.nl/assets/animated-characters-survivors'
URL='https://kenney.nl/media/pages/assets/animated-characters-survivors/27b16052a7-1774772958/kenney_animated-characters-survivors.zip'
SHA='fdadced07a0454c9b7f0b46507be6144a072b4d03b4ffa37f225893c76c62845'
REPO=Path(__file__).resolve().parents[2]

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--archive',type=Path);p.add_argument('--blender',required=True);p.add_argument('--scratch',type=Path,required=True,help='Disk-backed working directory, not a RAM filesystem');a=p.parse_args()
 if a.archive:data=a.archive.read_bytes()
 else:
  with urllib.request.urlopen(URL,timeout=30) as r:data=r.read(8*1024*1024+1)
 if len(data)>8*1024*1024 or hashlib.sha256(data).hexdigest()!=SHA:raise SystemExit('Survivor archive checksum/size changed')
 a.scratch.mkdir(parents=True,exist_ok=True)
 out=REPO/'examples/summit/content/kenney-character';out.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(dir=a.scratch) as directory:
  root=Path(directory)
  with zipfile.ZipFile(io.BytesIO(data)) as archive:
   for name in ['Model/characterMedium.fbx','Animations/idle.fbx','Animations/run.fbx','Animations/jump.fbx','Skins/survivorMaleB.png','License.txt']:
    file=root/name;file.parent.mkdir(parents=True,exist_ok=True);file.write_bytes(archive.read(name))
   (out/'License.txt').write_bytes(archive.read('License.txt'))
  subprocess.run([a.blender,'-b','--factory-startup','-t','1','--python',str(REPO/'tools/assets/prepare_summit_character.py'),'--',str(root),str(out/'survivor.glb')],check=True)
 subprocess.run([sys.executable,str(REPO/'tools/assets/add_summit_pickup.py')],check=True)
 blob=(out/'survivor.glb').read_bytes()
 manifest={'author':'Kenney','source':SOURCE,'download':URL,'archive_sha256':SHA,'license':'CC0-1.0',
  'modifications':'Survivor Male B mesh fitted to Summit rig, weights collapsed to 25 joints; authored idle/run/jump rotations retargeted. Walk slows run; Fall holds the jump mid-pose. Flat normals recalculated. Texture embedded unchanged. Kin adds an authored in-place Pickup reach; original mesh, texture and existing clips are unchanged.',
  'rig_source_sha256':hashlib.sha256((REPO/'examples/sponza_walk/character.hpp').read_bytes()).hexdigest(),
  'pickup_tool_sha256':hashlib.sha256((REPO/'tools/assets/add_summit_pickup.py').read_bytes()).hexdigest(),
  'tool_sha256':hashlib.sha256((REPO/'tools/assets/prepare_summit_character.py').read_bytes()).hexdigest(),
  'blender_version':subprocess.check_output([a.blender,'--version'],text=True).splitlines()[0],
  'files':{'survivor.glb':{'bytes':len(blob),'sha256':hashlib.sha256(blob).hexdigest()}}}
 (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 print('Prepared fitted survivor:',len(blob),'bytes')

if __name__=='__main__':main()
