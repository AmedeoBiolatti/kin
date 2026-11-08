# See README.md for scope, prerequisites and disposable-worktree instructions.
import argparse, json, os, re, statistics, subprocess, time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
a = parser.parse_args()
a.source, a.build, a.out = a.source.resolve(), a.build.resolve(), a.out.resolve()
expected = "baa5621032100adeb2fd48d84eb3170456ae5640"
head = subprocess.check_output(['git', '-C', str(a.source), 'rev-parse', 'HEAD'], text=True).strip()
if head != expected:
    parser.error('Use the disposable measured checkpoint described in README.md')
if subprocess.check_output(['git', '-C', str(a.source), 'status', '--porcelain', '--untracked-files=no'], text=True).strip():
    parser.error('Source worktree must have no tracked changes')
a.out.mkdir(parents=True, exist_ok=True)
main = a.source / 'games/hex_demo/src/main.cpp'
original, original_stat = main.read_bytes(), main.stat()
affinity = sorted(os.sched_getaffinity(0))[:4]
env = dict(os.environ, SDL_VIDEO_DRIVER='dummy', SDL_AUDIO_DRIVER='dummy', CCACHE_DISABLE='1')

# Never change compiler inputs during an owned configure/build/compile.
def guard():
    found = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit() or int(p.name) == os.getpid():
            continue
        try:
            args = (p / 'cmdline').read_bytes().split(b'\0')
            program = args[0].rsplit(b'/', 1)[-1]
            if not (b'clang' in program or program in [b'cmake', b'ninja', b'c++', b'g++', b'cc1plus']):
                continue
            cwd = os.readlink(p / 'cwd')
            if any(str(root).encode() in x for root in [a.source, a.build] for x in args) or cwd.startswith(str(a.build)):
                found.append(int(p.name))
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass
    if found:
        raise RuntimeError(f'Owned build processes are still running: {found}')

includes = re.findall(r'^#include <([^>]+)>', (a.source/'experimental/cpp_modules/kin_runtime.cppm').read_text(), re.M)
hook = a.out/'matched-pch.cmake'
hook.write_text('function(kin_module_bench_pch_control)\n'
                '  if(KIN_MODULE_BENCH_CASE STREQUAL "matched")\n'
                '    target_precompile_headers(hex_demo PRIVATE\n' +
                ''.join('      <'+h+'>\n' for h in includes) +
                '    )\n'
                '  elseif(KIN_MODULE_BENCH_CASE STREQUAL "modules-reduced")\n'
                '    target_compile_options(kin_runtime_module PRIVATE -fmodules-reduced-bmi)\n'
                '  elseif(KIN_MODULE_BENCH_CASE STREQUAL "modules-no-pch")\n'
                '    set_target_properties(hex_demo PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)\n'
                '  endif()\nendfunction()\n'
                'cmake_language(DEFER CALL kin_module_bench_pch_control)\n')

results = []

def run(cmd, name, measured=False):
    log, stats = a.out/(name+'.log'), a.out/(name+'.time.json')
    actual = ['taskset','-c',','.join(map(str,affinity)),*cmd]
    start_load, start = os.getloadavg(), time.perf_counter()
    with log.open('w') as f:
        r = subprocess.run(['/usr/bin/time','-f','{"user_s":%U,"system_s":%S,"max_process_rss_kib":%M}',
                            '-o',str(stats),*actual], cwd=a.source, env=env, stdout=f, stderr=subprocess.STDOUT)
    elapsed = time.perf_counter()-start
    if r.returncode:
        print(log.read_text()[-5000:],flush=True)
        raise RuntimeError(f'{name} failed: {r.returncode}')
    data = dict(name=name,wall_s=elapsed,load_before=start_load,load_after=os.getloadavg(),
                command=cmd,**json.loads(stats.read_text()))
    text = log.read_text()
    data['cpp_compiles'] = len(re.findall(r'Building CXX object',text))
    data['scans'] = len(re.findall(r' for CXX dependencies',text))
    data['links'] = len(re.findall(r'Linking CXX',text))
    if measured:
        results.append(data)
        with (a.out/'samples.jsonl').open('a') as f: f.write(json.dumps(data)+'\n')
        print(json.dumps({k:data[k] for k in ['name','wall_s','user_s','system_s','max_process_rss_kib','cpp_compiles','scans','links']}),flush=True)
    return data

build_cmd = ['cmake','--build',str(a.build),'--target','hex_demo','--parallel','4']

orders = [
    ['baseline','modules','matched'], ['modules','matched','baseline'], ['matched','baseline','modules'],
    ['baseline','matched','modules'], ['matched','modules','baseline'], ['modules','baseline','matched'],
]
cpp = a.source/'engine/src/pathfinding/pathfinding.cpp'
header = a.source/'engine/include/kin/pathfinding/pathfinding.hpp'
backups = {p:(p.read_bytes(),p.stat()) for p in [main,cpp,header]}

def configure(case,name):
    guard()
    run(['cmake','-S',str(a.source),'-B',str(a.build),
         '-DKIN_EXPERIMENTAL_CPP_MODULES='+('ON' if case.startswith('modules') else 'OFF'),
         '-DKIN_MODULE_BENCH_CASE='+case,'-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES='+str(hook)],name+'-configure')
    run(build_cmd,name+'-prime')

def cold():
    guard()
    for parent in [a.build/'games/hex_demo/CMakeFiles/hex_demo.dir',a.build/'games/hex_demo/CMakeFiles/kin_runtime_module.dir']:
        if parent.exists():
            for p in parent.rglob('*'):
                if p.is_file() and (p.suffix in ['.o','.pch','.pcm','.ddi'] or p.name.endswith('.modmap')):
                    p.unlink()
    for p in [a.build/'bin/hex_demo',a.build/'lib/libkin_runtime_module.a']:
        p.unlink(missing_ok=True)

try:
    guard()
    for round_id,order in enumerate(orders):
        for case in order:
            tag=f'{case}-r{round_id}'
            configure(case,tag)
            for sample in range(2):
                guard(); main.write_bytes(original+f'\n// C++ module paired probe {round_id}-{sample}\n'.encode())
                run(build_cmd,f'{case}-game-r{round_id}-s{sample}',True)
                run(build_cmd,f'{case}-noop-r{round_id}-s{sample}',True)
            if round_id<3:
                cold(); run(build_cmd,f'{case}-cold-r{round_id}',True)
            else:
                guard(); cpp.write_bytes(backups[cpp][0]+f'\n// C++ module native probe {round_id}\n'.encode())
                run(build_cmd,f'{case}-native-r{round_id}',True)
                guard(); cpp.write_bytes(backups[cpp][0]); run(build_cmd,tag+'-native-restore')
                guard(); header.write_bytes(backups[header][0]+f'\n// C++ module API header probe {round_id}\n'.encode())
                run(build_cmd,f'{case}-header-r{round_id}',True)
                guard(); header.write_bytes(backups[header][0]); run(build_cmd,tag+'-header-restore')
            run(['ctest','--test-dir',str(a.build),'-C','Release','-R','^hex-demo$','--output-on-failure'],tag+'-smoke')
    summary={case:{kind:statistics.median(r['wall_s'] for r in results if r['name'].startswith(case+'-'+kind+'-'))
                   for kind in ['game','noop','cold','native','header']} for case in ['baseline','modules','matched']}
    (a.out/'summary.json').write_text(json.dumps(dict(affinity=affinity,orders=orders,source=str(a.source),build=str(a.build),summary=summary,results=results),indent=2)+'\n')
    print(json.dumps(summary,indent=2),flush=True)
finally:
    guard()
    for p,(data,stat) in backups.items():
        p.write_bytes(data); os.utime(p,ns=(stat.st_atime_ns,stat.st_mtime_ns))
    # Header timestamps were restored: invalidate API PCH/BMI artifacts explicitly.
    cold()
    run(build_cmd,'final-restored-build')
