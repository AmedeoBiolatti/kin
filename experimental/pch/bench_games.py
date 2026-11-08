# See README.md for prerequisites, measurement scope and source mutations.
import argparse,json,os,re,statistics,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
a.source,a.build,a.out=a.source.resolve(),a.build.resolve(),a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
if any(a.out.iterdir()):p.error('Choose a new empty output directory')
cache=(a.build/'CMakeCache.txt').read_text()
match=re.search(r'^CMAKE_PROJECT_TOP_LEVEL_INCLUDES:[^=]+=(.*)$',cache,re.M)
if match and match.group(1).strip():p.error('Use a dedicated build with no other top-level CMake hook')
if subprocess.check_output(['git','-C',str(a.source),'status','--porcelain','--untracked-files=no'],text=True).strip():p.error('Use a clean disposable source worktree')
games=['charts_demo','text_edit_demo','shapes_demo','lighting_demo','ecs_parallel_bench','isometric_demo'];backups={}
for g in games:
 f=a.source/'games'/g/'src/main.cpp';backups[f]=(f.read_bytes(),f.stat())
affinity=sorted(os.sched_getaffinity(0))[:4];env=dict(os.environ,SDL_VIDEO_DRIVER='dummy',SDL_AUDIO_DRIVER='dummy',KIN_RENDER_BACKEND='sdl',CCACHE_DISABLE='1');results=[]
standard='algorithm array cmath cstdint functional memory optional span string string_view unordered_map utility vector'.split()
api={g:sorted({m.group(1) for f in (a.source/'games'/g/'src').rglob('*') if f.suffix in ['.cpp','.hpp'] for m in re.finditer(r'^\s*#include <(kin/[^>]+)>',f.read_text(),re.M)}) for g in games}
hook=a.out/'game-pch-control.cmake';lines=['function(kin_game_pch_control)']
for g in games:
 lines+=['  set_property(TARGET '+g+' PROPERTY PRECOMPILE_HEADERS "")','  target_precompile_headers('+g+' PRIVATE '+' '.join('<'+h+'>' for h in standard+['kin/runtime/scene_app.hpp'])+')','  if(KIN_BROAD_PCH_BENCH STREQUAL "targeted")','    target_precompile_headers('+g+' PRIVATE '+' '.join('<'+h+'>' for h in api[g])+')','  endif()']
lines+=['endfunction()','cmake_language(DEFER CALL kin_game_pch_control)'];hook.write_text('\n'.join(lines)+'\n')
def guard():
 found=[]
 for f in Path('/proc').iterdir():
  if not f.name.isdigit() or int(f.name)==os.getpid():continue
  try:
   args=(f/'cmdline').read_bytes().split(b'\0');program=args[0].rsplit(b'/',1)[-1]
   if not (b'clang' in program or program in [b'cmake',b'ninja',b'c++',b'g++',b'cc1plus']):continue
   cwd=os.readlink(f/'cwd')
   if any(str(r).encode() in x for r in [a.source,a.build] for x in args) or cwd.startswith(str(a.build)):found.append(f.name)
  except (FileNotFoundError,PermissionError,ProcessLookupError):pass
 if found:raise RuntimeError('Owned build processes are running: '+str(found))
def run(cmd,name,measured=False):
 log=a.out/(name+'.log');stats=a.out/(name+'.time.json');before=os.getloadavg();start=time.perf_counter()
 with log.open('w') as f:r=subprocess.run(['/usr/bin/time','-f','{"user_s":%U,"system_s":%S,"max_process_rss_kib":%M}','-o',str(stats),'taskset','-c',','.join(map(str,affinity)),*cmd],cwd=a.source,env=env,stdout=f,stderr=subprocess.STDOUT)
 wall=time.perf_counter()-start
 if r.returncode:raise RuntimeError(name+' failed: '+log.read_text()[-3000:])
 text=log.read_text();d=dict(name=name,wall_s=wall,load_before=before,load_after=os.getloadavg(),cpp_compiles=len(re.findall('Building CXX object',text)),links=len(re.findall('Linking CXX',text)),**json.loads(stats.read_text()))
 if measured:
  results.append(d);(a.out/'samples.jsonl').open('a').write(json.dumps(d)+'\n');print(json.dumps(d),flush=True)
 return d
def build(targets,name,measured=False):return run(['cmake','--build',str(a.build),'--target',*targets,'--parallel','4'],name,measured)
def cold(g):
 guard();parent=a.build/'games'/g/'CMakeFiles'/(g+'.dir')
 for f in parent.rglob('*'):
  if f.is_file() and f.suffix in ['.o','.pch']:f.unlink()
 (a.build/'bin'/g).unlink(missing_ok=True)
try:
 guard()
 for r in range(6):
  for case in (['baseline','targeted'] if r%2==0 else ['targeted','baseline']):
   tag=f'{case}-r{r}';guard()
   run(['cmake','-S',str(a.source),'-B',str(a.build),'-DKIN_BROAD_PCH_BENCH='+case,'-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES='+str(hook)],tag+'-configure')
   build(games,tag+'-prime')
   for g in (games if r%2==0 else list(reversed(games))):
    f=a.source/'games'/g/'src/main.cpp';guard();f.write_bytes(backups[f][0]+f'\n// Broader PCH probe {r}\n'.encode())
    build([g],f'{case}-{g}-warm-r{r}',True);build([g],f'{case}-{g}-noop-r{r}',True)
    if r in [0,2,4]:cold(g);build([g],f'{case}-{g}-cold-r{r}',True)
   run(['ctest','--test-dir',str(a.build),'-C','Release','-R','^(charts-demo|text-edit-demo|shapes-demo|lighting-demo|ecs-parallel-bench)$','--output-on-failure'],tag+'-smoke')
   run([str(a.build/'bin/isometric_demo'),'--smoke'],tag+'-iso-smoke')
 summary={c:{g:{k:statistics.median(x['wall_s'] for x in results if x['name'].startswith(c+'-'+g+'-'+k+'-')) for k in ['warm','noop','cold']} for g in games} for c in ['baseline','targeted']}
 (a.out/'summary.json').write_text(json.dumps(dict(affinity=affinity,api=api,standard=standard,summary=summary,results=results),indent=2)+'\n');print(json.dumps(summary,indent=2),flush=True)
finally:
 guard()
 for f,(data,st) in backups.items():f.write_bytes(data);os.utime(f,ns=(st.st_atime_ns,st.st_mtime_ns))
 run(['cmake','-S',str(a.source),'-B',str(a.build),'-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=','-U','KIN_BROAD_PCH_BENCH'],'final-configure');build(games,'final-build')
