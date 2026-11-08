# See README.md for prerequisites, measurement scope and source mutations.
import argparse,json,os,re,shlex,statistics,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
a.source,a.build,a.out=a.source.resolve(),a.build.resolve(),a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
if any(a.out.iterdir()):p.error('Choose a new empty output directory')
commands=json.loads((a.build/'compile_commands.json').read_text());affinity=sorted(os.sched_getaffinity(0))[:4]
env=dict(os.environ,CCACHE_DISABLE='1')
files=['engine/src/renderer/renderer2d.cpp','engine/src/ui2/widgets_editor.cpp','engine/src/platform/services_catalog.cpp']
variants={'baseline':[],'filesystem':['filesystem'],'filesystem-chrono':['filesystem','chrono']};results=[];preparation=[];pch_commands={};source_commands={}
def run(cmd,name,measured=False):
 log=a.out/(name+'.log');stats=a.out/(name+'.time.json');start=time.perf_counter()
 with log.open('w') as f:r=subprocess.run(['/usr/bin/time','-f','{"user_s":%U,"system_s":%S,"max_process_rss_kib":%M}','-o',str(stats),'taskset','-c',','.join(map(str,affinity)),*cmd],cwd=a.build,env=env,stdout=f,stderr=subprocess.STDOUT)
 wall=time.perf_counter()-start
 if r.returncode:raise RuntimeError(name+' failed: '+log.read_text()[-3000:])
 d=dict(name=name,wall_s=wall,**json.loads(stats.read_text()))
 if measured:results.append(d);print(json.dumps(d),flush=True)
 return d
for file in files:
 c=next(c for c in commands if c['file']==str(a.source/file));cmd=shlex.split(c['command']);target=c['output'].split('.dir/')[0]+'.dir'
 pch=next(c for c in commands if c['output'].startswith(target+'/') and c['output'].endswith('.pch'))
 for variant,headers in variants.items():
  key=(target,variant)
  if key not in pch_commands:
   d=a.out/variant/Path(target).name;d.mkdir(parents=True,exist_ok=True);h=d/'cmake_pch.hxx';cpp=d/'cmake_pch.hxx.cxx';obj=d/'cmake_pch.hxx.pch'
   old_h=Path(pch['file']).with_suffix('')
   text=old_h.read_text();text=re.sub(r'^#include <(?:filesystem|chrono)>\n','',text,flags=re.M);text=text.replace('#include <vector>\n','#include <vector>\n'+''.join('#include <'+x+'>\n' for x in headers),1);h.write_text(text);cpp.write_bytes(Path(pch['file']).read_bytes())
   repl={str(old_h):str(h),pch['file']:str(cpp),pch['output']:str(obj)}
   pc=[repl.get(x,x) for x in shlex.split(pch['command'])];pc[pc.index('-o')+1]=str(obj)
   assert Path(pc[pc.index('-o')+1]).is_relative_to(a.out)
   assert '-MF' not in pc, 'Use the documented Ninja build without external dependency-file flags'
   pch_commands[key]=(pc,h,obj)
  pc,h,obj=pch_commands[key]
  sc=[str(obj) if x.endswith('/cmake_pch.hxx.pch') else str(h) if x.endswith('/cmake_pch.hxx') else x for x in cmd]
  out=a.out/variant/(Path(file).stem+'.o');sc[sc.index('-o')+1]=str(out);sc+=['-Werror=invalid-pch'];assert Path(sc[sc.index('-o')+1]).is_relative_to(a.out);assert '-MF' not in sc;source_commands[(variant,file)]=sc
for (target,variant),(cmd,h,obj) in pch_commands.items():preparation.append(dict(target=target,variant=variant,**run(cmd,variant+'-'+Path(target).name+'-prepare')))
orders=[['baseline','filesystem','filesystem-chrono'],['filesystem','filesystem-chrono','baseline'],['filesystem-chrono','baseline','filesystem'],['baseline','filesystem-chrono','filesystem'],['filesystem-chrono','filesystem','baseline'],['filesystem','baseline','filesystem-chrono']]
for round_id,order in enumerate(orders):
 for variant in order:
  for file in files:
   run(source_commands[(variant,file)],f'{variant}-{Path(file).stem}-r{round_id}',True)
summary={v:{Path(f).stem:{'wall_s':statistics.median(x['wall_s'] for x in results if x['name'].startswith(v+'-'+Path(f).stem+'-')),'cpu_s':statistics.median(x['user_s']+x['system_s'] for x in results if x['name'].startswith(v+'-'+Path(f).stem+'-'))} for f in files} for v in variants}
(a.out/'summary.json').write_text(json.dumps(dict(affinity=affinity,variants=variants,orders=orders,scope='real engine source to object; no final link; PCHs built separately with target compiler flags; no source or build inputs modified',preparation=preparation,summary=summary,results=results),indent=2)+'\n');print(json.dumps(summary,indent=2),flush=True)
