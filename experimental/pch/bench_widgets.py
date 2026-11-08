# Native Linux Clang/Ninja. See README.md; use an exclusive disposable worktree.
from pathlib import Path
import argparse,json,os,re,statistics,subprocess,time
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.source,a.build,a.out=a.source.resolve(),a.build.resolve(),a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
if any(a.out.iterdir()):p.error('Choose a new empty output directory')
cache=(a.build/'CMakeCache.txt').read_text()
initial=re.search(r'^KIN_ENABLE_WIDGET_PCH:[^=]+=(.*)$',cache,re.M).group(1).strip()
if 'KIN_ENABLE_PCH:BOOL=ON' not in cache:p.error('Enable KIN_ENABLE_PCH in the dedicated build')
if subprocess.check_output(['git','-C',str(a.source),'status','--porcelain','--untracked-files=no'],text=True).strip():p.error('Use a clean disposable source worktree')
cpps=sorted((a.source/'engine/src/ui2').glob('widgets_*.cpp'));private=a.source/'engine/src/ui2/widget_chunk_preamble.hpp';public=a.source/'engine/include/kin/ui2/widgets.hpp';backups={f:(f.read_bytes(),f.stat()) for f in cpps+[private,public]}
affinity=sorted(os.sched_getaffinity(0))[:4];env=dict(os.environ,SDL_VIDEO_DRIVER='dummy',SDL_AUDIO_DRIVER='dummy',KIN_RENDER_BACKEND='sdl',CCACHE_DISABLE='1');results=[]
def guard():
 found=[]
 for f in Path('/proc').iterdir():
  if not f.name.isdigit() or int(f.name)==os.getpid():continue
  try:
   args=(f/'cmdline').read_bytes().split(b'\0');name=args[0].rsplit(b'/',1)[-1]
   if not (b'clang' in name or name in [b'cmake',b'ninja',b'c++',b'g++',b'cc1plus']):continue
   cwd=os.readlink(f/'cwd')
   if any(str(r).encode() in x for r in [a.source,a.build] for x in args) or cwd.startswith(str(a.build)):found.append(f.name)
  except (FileNotFoundError,PermissionError,ProcessLookupError):pass
 if found:raise RuntimeError('Owned build is running: '+str(found))
def run(cmd,name,measured=False):
 log=a.out/(name+'.log');stats=a.out/(name+'.time.json');start=time.perf_counter();before=os.getloadavg()
 with log.open('w') as f:r=subprocess.run(['/usr/bin/time','-f','{"user_s":%U,"system_s":%S,"max_process_rss_kib":%M}','-o',str(stats),'taskset','-c',','.join(map(str,affinity)),*cmd],cwd=a.source,env=env,stdout=f,stderr=subprocess.STDOUT)
 elapsed=time.perf_counter()-start
 if r.returncode:raise RuntimeError(name+' failed: '+log.read_text()[-4000:])
 text=log.read_text();d=dict(name=name,wall_s=elapsed,load_before=before,load_after=os.getloadavg(),compiles=len(re.findall('Building CXX object',text)),archives=len(re.findall('Linking CXX static library',text)),links=len(re.findall('Linking CXX executable',text)),**json.loads(stats.read_text()))
 if measured:
  results.append(d)
  with (a.out/'samples.jsonl').open('a') as f:f.write(json.dumps(d)+'\n')
  print(json.dumps(d),flush=True)
 return d
def build(name,measured=False):return run(['cmake','--build',str(a.build),'--target','hex_demo','--parallel','4'],name,measured)
def cold(case):
 guard();directory=a.build/'engine/CMakeFiles'/('kin_engine.dir' if case=='baseline' else 'kin_ui2_widgets.dir')
 for f in directory.rglob('*'):
  if f.is_file() and ((f.name.startswith('widgets_') and f.suffix=='.o') or (case=='widget' and f.suffix=='.pch')):f.unlink()
try:
 guard()
 for r in range(4):
  for case in (['baseline','widget'] if r%2==0 else ['widget','baseline']):
   tag=f'{case}-r{r}';guard();run(['cmake','-S',str(a.source),'-B',str(a.build),'-DKIN_ENABLE_WIDGET_PCH='+('ON' if case=='widget' else 'OFF')],tag+'-configure');build(tag+'-prime')
   for stem in ['widgets_editor','widgets_core','widgets_inputs']:
    f=a.source/'engine/src/ui2'/(stem+'.cpp');guard();f.write_bytes(backups[f][0]+f'\n// Widget PCH source probe {r}\n'.encode());build(f'{case}-{stem}-r{r}',True)
   build(case+f'-noop-r{r}',True)
   cold(case);build(case+f'-cold-r{r}',True)
   guard();private.write_bytes(backups[private][0]+f'\n// Widget PCH private header probe {r}\n'.encode());build(case+f'-private-r{r}',True)
   guard();private.write_bytes(backups[private][0]);build(tag+'-private-restore')
   if r<3:
    guard();public.write_bytes(backups[public][0]+f'\n// Widget PCH public header probe {r}\n'.encode());build(case+f'-public-r{r}',True)
    guard();public.write_bytes(backups[public][0]);build(tag+'-public-restore')
   run(['ctest','--test-dir',str(a.build),'-C','Release','-R','^hex-demo$','--output-on-failure'],tag+'-smoke')
 kinds=['widgets_editor','widgets_core','widgets_inputs','noop','cold','private','public'];summary={c:{k:statistics.median(x['wall_s'] for x in results if x['name'].startswith(c+'-'+k+'-')) for k in kinds} for c in ['baseline','widget']}
 (a.out/'summary.json').write_text(json.dumps(dict(affinity=affinity,scope={'warm':'one real widget source edit through Hex executable link','cold':'six widget objects and private PCH if present; common engine PCH, other engine objects and dependencies already built','private':'private preamble comment edit through Hex link','public':'public widgets API header comment edit through Hex link; declarations unchanged'},summary=summary,results=results),indent=2)+'\n');print(json.dumps(summary,indent=2),flush=True)
finally:
 guard()
 for f,(data,st) in backups.items():f.write_bytes(data);os.utime(f,ns=(st.st_atime_ns,st.st_mtime_ns))
 run(['cmake','-S',str(a.source),'-B',str(a.build),'-DKIN_ENABLE_WIDGET_PCH='+initial],'final-configure');cold('widget');build('final-restored-build')
