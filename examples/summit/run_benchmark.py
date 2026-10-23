#!/usr/bin/env python3
"""Serial RAM-bounded, real-time 1440p Summit traversal comparison. No downloads."""
import argparse
import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
spec = importlib.util.spec_from_file_location('summit_guard', HERE.parent / 'blockfield/run_render_compare.py')
guard = importlib.util.module_from_spec(spec); spec.loader.exec_module(guard)


def monitor(path, owner):
    known = {owner}
    def ours(pid):
        chain = []
        for _ in range(32):
            if pid in known: known.update(chain); return True
            if pid <= 1: return False
            chain.append(pid)
            try:
                status = Path(f'/proc/{pid}/status').read_text()
                pid = int(next(x.split()[1] for x in status.splitlines() if x.startswith('PPid:')))
            except (OSError,StopIteration): return False
        return False
    with path.open('w') as output:
        while True:
            start = time.monotonic()
            gpu = subprocess.run(['nvidia-smi','pmon','-c','1'],capture_output=True,text=True,timeout=5).stdout
            cpu = subprocess.run(['ps','-eo','pid=,comm=,pcpu='],capture_output=True,text=True,timeout=5).stdout
            conflicts = []
            for line in gpu.splitlines():
                p = line.split()
                if p and not line.startswith('#') and len(p)>=4 and p[1].isdigit() and p[-1]!='gnome-shell' and not ours(int(p[1])):
                    conflicts.append({'source':'gpu','line':line})
            for line in cpu.splitlines():
                p = line.split()
                if len(p)==3 and p[1] in ['atlas_dump','portrait_mesh','cc1plus','glslc'] and not ours(int(p[0])):
                    conflicts.append({'source':'cpu','line':line})
            output.write(json.dumps({'mono':start,'wall':time.time(),'conflicts':conflicts,'gpu':gpu})+'\n'); output.flush()
            time.sleep(max(.01,.5-(time.monotonic()-start)))


def percentile(values, p):
    ordered = sorted(values)
    x = (len(ordered)-1)*p/100
    low = int(x); high = min(low+1,len(ordered)-1)
    return ordered[low]+(ordered[high]-ordered[low])*(x-low)


def analyze(root, label, process, expected, width, height, samples):
    log=(root/(label+'.log')).read_text()
    for marker in ['external assets 14/14 models','imported PBR materials 2/2','authored survivor loaded']:
        if marker not in log: raise RuntimeError(f'{label}: required authored asset/material missing: {marker}')
    metadata = json.loads((root/(label+'.csv.json')).read_text())
    if not metadata['completed'] or metadata['falls'] or metadata['deaths'] or not metadata['sheltered']:
        raise RuntimeError(f'{label}: route failed')
    if (metadata['steps'],metadata['final_hash']) != expected:
        raise RuntimeError(f'{label}: real-time traversal did not match fixed-step CPU replay')
    if (metadata['width'],metadata['height'],metadata['samples']) != (width,height,samples):
        raise RuntimeError(f'{label}: requested render resolution/AA did not run')
    with (root/(label+'.csv')).open() as source: rows = list(csv.DictReader(source))
    active = [r for r in rows if r['measured']=='1' and r['phase'] in ['1','2']]
    warm = [r for r in rows if r['measured']=='0']
    if len(active)<1000 or metadata['rendered_frames']!=len(rows): raise RuntimeError(f'{label}: missing full-frame samples')
    # Include briefing in route coverage; traversal timings below intentionally
    # exclude it. The bot begins climbing at step 21, not at step 1.
    steps = [int(r['step']) for r in rows if r['measured']=='1']
    if min(steps)>10 or max(steps)<metadata['steps']-10 or any(b<a for a,b in zip(steps,steps[1:])):
        raise RuntimeError(f'{label}: incomplete/nonmonotonic route coverage')
    intervals = [float(r['frame_interval_ms']) for r in active]
    if any(t<=0 for t in intervals): raise RuntimeError(f'{label}: invalid frame intervals')
    seconds = sum(intervals)/1000
    if abs(seconds-metadata['steps']/60)>3:
        raise RuntimeError(f'{label}: traversal did not use real-time 60 Hz simulation ({seconds:.1f}s)')
    for r in active:
        if (int(r['width']),int(r['height'])) != (width,height) or int(r['draws'])<=0 or int(r['triangles'])<=0 or int(r['shadow_cascades'])!=4:
            raise RuntimeError(f'{label}: render/shadow work missing')
        # The asset has the canonical 25-joint skeleton, but draw_skinned keeps
        # only the first 20 matrices referenced by this mesh (no tail joints).
        if int(r['skin_matrices'])!=20 or int(r['skinned_copies'])!=1:
            raise RuntimeError(f'{label}: real character skinning missing')
    phases={}
    for leg in sorted(set(int(r['leg']) for r in active)):
        group=[r for r in active if int(r['leg'])==leg]
        timings=[float(r['frame_interval_ms']) for r in group]
        phases[str(leg)]={'samples':len(group),'p50_ms':percentile(timings,50),'p95_ms':percentile(timings,95),'p99_ms':percentile(timings,99)}
    result={'label':label,'scenery':metadata['scenery'],'samples':len(active),'seconds':seconds,'peak_rss_mib':process['peak_rss_mib'],
            'p50_ms':percentile(intervals,50),'p95_ms':percentile(intervals,95),'p99_ms':percentile(intervals,99),'max_ms':max(intervals),
            'over_16_67_ms':sum(t>1000/60 for t in intervals),'over_33_33_ms':sum(t>1000/30 for t in intervals),
            'next_hit_max_steps':sum(int(r['next_hit_max_steps']) for r in active),
            'startup_ms':metadata['startup_to_first_completed_frame_ms'],'asset_setup_ms':metadata['asset_setup_ms'],
            'warmup_max_ms':max(float(r['frame_interval_ms']) for r in warm),
            'scenery_instances':metadata['scenery_instances'],'scenery_instance_triangles':metadata['scenery_instance_triangles'],
            'prop_colliders':metadata['prop_colliders'],'instance_record_bytes':metadata['instance_record_bytes'],'legs':phases}
    for key in ['draws','triangles','shadow_draws','shadow_casters','instances','spatial_groups','scene_build_ms','cpu_submit_ms','instance_upload_bytes']:
        values=[float(r[key]) for r in active]
        result[key]={'p50':percentile(values,50),'p95':percentile(values,95),'max':max(values)}
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True,help='Fresh directory on disk, not tmpfs')
    parser.add_argument('--pairs',type=int,default=3)
    parser.add_argument('--width',type=int,default=2560);parser.add_argument('--height',type=int,default=1440)
    parser.add_argument('--samples',type=int,default=4);parser.add_argument('--warmup',type=float,default=2)
    args=parser.parse_args();root=args.output.resolve()
    if args.pairs<1 or args.width<1 or args.height<1 or args.samples not in [1,2,4,8] or not 0<=args.warmup<=30:
        parser.error('Use positive pairs/resolution, 1/2/4/8 samples, and a warmup between 0 and 30 seconds')
    if root.exists() and any(root.iterdir()): raise SystemExit('Choose a fresh output directory')
    root.mkdir(parents=True,exist_ok=True);(root/'tmp').mkdir()
    env=dict(os.environ,KIN_RENDER_BACKEND='gpu',TMPDIR=str(root/'tmp'))
    # Frame timestamps/draw counters do not need GPU timers or the global in-memory profiler.
    env.pop('KIN_LOG_FRAME_STATS',None)
    tracked=[REPO/'build/bin/summit',*HERE.glob('*.cpp'),*HERE.glob('*.hpp'),Path(__file__).resolve(),*HERE.glob('content/*/manifest.json')]
    manifest={'sha256':{str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked},
              'render_size':[args.width,args.height],'samples':args.samples,'pairs':args.pairs,
              'environment':{k:v for k,v in env.items() if k.startswith('KIN_') and any(w in k for w in ['RENDER','SHADOW','INSTANCE','BOUNDS','CULL','POINT','GPU','PREPARED','STAGING'])},
              'gpu':subprocess.check_output(['nvidia-smi','--query-gpu=name,driver_version','--format=csv'],text=True)}
    (root/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    expected={}
    for detail in ['compact','dense']:
        label='cpu-'+detail
        guard.bounded_run([REPO/'build/bin/summit','--script','shelter','--scenery',detail,'--seed','1'],label,root,env=env,rss_limit=256)
        log=(root/(label+'.log')).read_text();match=re.search(r'final state hash ([0-9a-f]+), step (\d+)',log)
        if not match: raise RuntimeError('CPU route report missing')
        expected[detail]=(int(match[2]),int(match[1],16))
    (root/'expected-cpu-routes.json').write_text(json.dumps(expected,indent=2)+'\n')
    monitor_process=subprocess.Popen([sys.executable,str(Path(__file__).resolve()),'--monitor',str(root/'environment.jsonl'),str(os.getpid())])
    runs=[];audit=[]
    def quiet():
        deadline=time.monotonic()+180
        while time.monotonic()<deadline:
            time.sleep(1)
            if not (root/'environment.jsonl').exists(): continue
            data=[json.loads(line) for line in (root/'environment.jsonl').read_text().splitlines() if line.endswith('}')]
            now=time.monotonic();recent=[sample for sample in data if now-sample['mono']<10]
            if recent and now-data[0]['mono']>=10 and all(not sample['conflicts'] for sample in recent):return
        raise RuntimeError('Competing GPU/build jobs did not finish; no profiling run started')
    try:
        quiet()
        pending=[(detail,repeat,0) for repeat in range(args.pairs)
                 for detail in (['compact','dense'] if repeat%2==0 else ['dense','compact'])]
        while pending:
            detail,repeat,attempt=pending.pop(0)
            label=f'{detail}-r{repeat+1}'+(f'-retry{attempt}' if attempt else '')
            quiet()
            began=time.monotonic()
            command=[REPO/'build/bin/summit','--benchmark',root/(label+'.csv'),'--script','shelter','--scenery',detail,
                     '--seed','1','--width',args.width,'--height',args.height,'--samples',args.samples,'--benchmark-warmup',args.warmup,'--max-steps','12000']
            query='timestamp,pstate,clocks.sm,clocks.mem,temperature.gpu,power.draw,utilization.gpu,clocks_event_reasons.sw_thermal_slowdown'
            gpu=subprocess.Popen(['nvidia-smi','--query-gpu='+query,'--format=csv','-lms','500','-f',str(root/(label+'-gpu.csv'))],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            print('Starting',label,flush=True)
            try:process=guard.bounded_run(command,label,root,env=env,rss_limit=512)
            finally:gpu.terminate();gpu.wait(timeout=5)
            ended=time.monotonic();time.sleep(.6)
            samples=[json.loads(line) for line in (root/'environment.jsonl').read_text().splitlines() if line.endswith('}')]
            conflicts=[s for s in samples if began-1<=s['mono']<=ended+.5 and s['conflicts']]
            audit.append({'label':label,'start':began,'end':ended,'accepted':not conflicts,'conflicts':conflicts})
            (root/'contention-audit.json').write_text(json.dumps(audit,indent=2)+'\n')
            if conflicts:
                print(f'{label}: discarded due to competing GPU/build jobs',flush=True)
                if attempt>=2: raise RuntimeError('Three contaminated attempts; rerun when competing jobs finish')
                pending.insert(0,(detail,repeat,attempt+1))
                continue
            result=analyze(root,label,process,expected[detail],args.width,args.height,args.samples);runs.append(result)
            (root/'runs.json').write_text(json.dumps(runs,indent=2)+'\n')
            print(f"{label}: p50 {result['p50_ms']:.3f}, p95 {result['p95_ms']:.3f}, p99 {result['p99_ms']:.3f} ms; max {result['max_ms']:.1f} ms",flush=True)
        for name,sha in manifest['sha256'].items():
            if hashlib.sha256((REPO/name).read_bytes()).hexdigest()!=sha: raise RuntimeError(f'Source/binary changed during measurement: {name}')
        summary={}
        for detail in ['compact','dense']:
            group=[r for r in runs if r['scenery']==detail]
            summary[detail]={key:statistics.median(r[key] for r in group) for key in ['p50_ms','p95_ms','p99_ms','max_ms','startup_ms','asset_setup_ms']}
            summary[detail].update({'peak_rss_mib':max(r['peak_rss_mib'] for r in group),'runs':len(group),
                                   'frames':sum(r['samples'] for r in group),'over_16_67_ms':sum(r['over_16_67_ms'] for r in group),
                                   'scenery_instances':group[0]['scenery_instances'],'scenery_instance_triangles':group[0]['scenery_instance_triangles'],
                                   'prop_colliders':group[0]['prop_colliders']})
        (root/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
        print(json.dumps(summary,indent=2),flush=True)
    finally:
        monitor_process.terminate();monitor_process.wait(timeout=5)


if __name__=='__main__':
    if len(sys.argv)>1 and sys.argv[1]=='--monitor':monitor(Path(sys.argv[2]),int(sys.argv[3]))
    else:main()
