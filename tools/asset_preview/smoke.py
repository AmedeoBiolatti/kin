"""Small Linux GPU regression for the asset preview. No downloaded assets."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import time
import traceback


def available_mib():
    return next(int(line.split()[1])/1024 for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:'))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--model',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if available_mib()<2048:
        raise RuntimeError('requires 2048 MiB available RAM before starting')
    output=args.output.resolve()/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    output.mkdir(parents=True)
    root=output/'assets';root.mkdir()
    model=root/'crate.glb';shutil.copyfile(args.model,model);good_model=model.read_bytes()
    binding=model.with_suffix('.kinmaterials')
    good_binding=json.dumps({'schema':'kin.materials3d/1','material':{'roughness':.7}})
    binding.write_text(good_binding)
    env=dict(os.environ,KIN_RENDER_BACKEND='gpu')
    authority=Path(env.get('XAUTHORITY',''))
    runtime=Path(env.get('XDG_RUNTIME_DIR','/nonexistent'))
    if authority.name.startswith('.mutter-Xwaylandauth.') and authority.parent==runtime:
        candidates=list(runtime.glob('.mutter-Xwaylandauth.*'))
        if candidates:env['XAUTHORITY']=str(max(candidates,key=lambda p:p.stat().st_mtime_ns))
    peak=0;started=time.monotonic();serial=0;transcript=[]
    with (output/'stderr.log').open('w') as log:
        process=subprocess.Popen([str(args.binary.resolve()),'--root',str(root),'--model','crate.glb',
                                  '--width=640','--height=480','--server'],cwd=output,env=env,
                                 stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log)
        def rpc(method,**params):
            nonlocal serial,peak
            serial+=1;request={'id':serial,'method':method,'params':params}
            process.stdin.write((json.dumps(request)+'\n').encode());process.stdin.flush()
            deadline=time.monotonic()+30
            while time.monotonic()<deadline:
                try:
                    rss=next(int(line.split()[1])/1024 for line in Path(f'/proc/{process.pid}/status').read_text().splitlines() if line.startswith('VmRSS:'))
                    peak=max(peak,rss)
                except (FileNotFoundError,StopIteration):pass
                if peak>1024 or available_mib()<1024:
                    raise RuntimeError('preview exceeded RSS/reserve budget')
                if select.select([process.stdout],[],[],.05)[0]:
                    line=process.stdout.readline()
                    if not line:
                        detail=(output/'stderr.log').read_text()
                        unavailable=('SDL_Init failed' in detail or 'asset preview requires the GPU backend' in detail)
                        if unavailable and os.environ.get('KIN_REQUIRE_GPU_TESTS','0') not in ['1','true']:
                            print('Asset preview GPU unavailable; set KIN_REQUIRE_GPU_TESTS=1 to require it')
                            raise SystemExit(77)
                        raise RuntimeError('preview exited before replying; see '+str(output/'stderr.log'))
                    reply=json.loads(line);transcript.append({'request':request,'response':reply})
                    if reply.get('id')!=serial or 'error' in reply:raise RuntimeError('invalid server response: '+str(reply))
                    return reply['result']
            raise RuntimeError('preview response timed out')
        def state():return rpc('scene.current')['state']
        def action(name):
            rpc('input.action',name='preview.'+name)
            result=rpc('sim.tick',count=2)
            assert not result.get('failed')
            return state()
        def shot(name):
            rpc('view.screenshot',path=name+'.png')
            return hashlib.sha256((output/(name+'.png')).read_bytes()).hexdigest()
        try:
            rpc('sim.tick',count=2)
            initial=state();assert initial['source_parts']>0 and initial['material_binding'] and initial['draws']>0
            assert (initial['width'],initial['height'])==(640,480)
            assert shot('lit')==shot('lit-repeat')
            images={}
            for mode in ['base','normals','uv','metallic','roughness','alpha','wireframe']:
                now=action('mode');assert now['mode']==mode and now['draws']>0
                images[mode]=shot(mode)
            assert len(set(images.values()))>=6,'inspection modes did not change pixels'
            assert action('mode')['mode']=='lit'
            assert action('part')['part']==0
            assert action('bounds')['bounds'];shot('bounds')
            assert action('projection')['orthographic'];shot('orthographic')
            now=action('freeze');assert now['frozen']
            rpc('sim.tick',count=30);assert state()['time']==now['time']
            assert action('hud')['hud'];shot('preview-hud')
            # Both geometry and binding failures must retain the active model.
            model.write_bytes(b'bad glb')
            failed=action('reload');assert failed['reloads']==1 and failed['reload_error'] and failed['draws']>0
            model.write_bytes(good_model);binding.write_text('{')
            failed=action('reload');assert failed['reloads']==1 and failed['reload_error'] and failed['draws']>0
            binding.write_text(good_binding)
            restored=action('reload');assert restored['reloads']==2 and not restored['reload_error']
            assert restored['source_parts']==initial['source_parts']
            rpc('server.shutdown');assert process.wait(timeout=5)==0
            # The driven server has no runtime overlay/profiling sinks. Exercise
            # the actual application sink with the ordinary headless runner.
            with (output/'profile.log').open('w') as profile_log:
                profile=subprocess.Popen([str(args.binary.resolve()),'--root',str(root),'--model','crate.glb',
                    '--width=640','--height=480','--headless','--frames=2','--profile-render','--profile',
                    '--profile-json='+str(output/'timings.json'),
                    '--report='+str(output/'profile.json')],cwd=output,env=env,stdout=profile_log,stderr=profile_log)
                try:
                    deadline=time.monotonic()+30
                    while profile.poll() is None:
                        try:
                            rss=next(int(line.split()[1])/1024 for line in Path(f'/proc/{profile.pid}/status').read_text().splitlines() if line.startswith('VmRSS:'))
                            peak=max(peak,rss)
                        except (FileNotFoundError,StopIteration):pass
                        if peak>1024 or available_mib()<1024 or time.monotonic()>deadline:
                            raise RuntimeError('headless profiling budget exceeded')
                        time.sleep(.05)
                    assert profile.returncode==0
                finally:
                    if profile.poll() is None:profile.kill();profile.wait()
            profile_report=json.loads((output/'profile.json').read_text())
            assert profile_report['status']=='ok' and profile_report['scenes'][-1]['state']['diagnostics_active']
            timings=json.loads((output/'timings.json').read_text())
            assert any(row['name']=='render3d.cpu' and row['calls']>0 for row in timings['summary'])
            assert 'render3d.cpu' in (output/'profile.log').read_text()
            report={'passed':True,'peak_preview_rss_mib':peak,'elapsed_seconds':time.monotonic()-started,'inspection_modes':list(images)}
            (output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
            print(json.dumps(report));print('Artifacts: '+str(output));return 0
        finally:
            (output/'transcript.json').write_text(json.dumps(transcript,indent=2)+'\n')
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=2)
                except subprocess.TimeoutExpired:process.kill();process.wait()


if __name__=='__main__':
    try:sys.exit(main())
    except Exception as error:
        print('Asset preview regression: '+str(error),file=sys.stderr);traceback.print_exc();sys.exit(1)
