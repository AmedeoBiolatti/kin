#!/usr/bin/env python3
"""Audit actual Ninja plans for optional component isolation (Linux)."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess


def audit(build, manifest_path=None):
    cache = {}
    for line in (build/'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
        if line and not line.startswith(('#','//')) and '=' in line:
            key,value=line.split('=',1); cache[key.split(':',1)[0]]=value
    if cache['CMAKE_GENERATOR'] != 'Ninja':
        raise ValueError('Component target audit requires single-config Ninja')
    ninja=cache['CMAKE_MAKE_PROGRAM']
    def run(*args):
        return subprocess.check_output([ninja,'-C',str(build),'-t',*args],text=True)
    targets={line.split(': ',1)[0] for line in run('targets','all').splitlines()}
    manifest_path=manifest_path or build/'generated/kin/components.json'
    manifest=json.loads(manifest_path.read_text(encoding='utf-8'))
    umbrella=run('commands','kin_component_tests')
    registered = json.loads(subprocess.check_output(
        ['ctest', '--test-dir', str(build), '-C', cache['CMAKE_BUILD_TYPE'], '--show-only=json-v1'],
        text=True))
    test_names = {test['name'] for test in registered['tests']}
    rows={}
    descriptions={
        'audio':(['kin_audio','kin_audio_ecs'],'kin_audio_component_tests',
                 ['libkin_engine.a','libflecs_static.a','libkin_audio_ecs.a','libkin_physics.a','libkin_scripting.a','libbox2d.a','liblua_static.a']),
        'physics':(['kin_physics','kin_physics_render','kin_physics_ecs'],'kin_physics_component_tests',
                   ['libkin_engine.a','libflecs_static.a','libSDL3.a','libkin_physics_ecs.a','libkin_physics_render.a','libkin_scripting.a','libkin_audio.a','liblua_static.a']),
        'scripting':(['kin_scripting','kin_scripting_scenes'],'kin_lua_script_tests',
                     ['libkin_engine.a','libflecs_static.a','libkin_scripting_scenes.a','libkin_physics.a','libkin_audio.a','libbox2d.a']),
    }
    for name,(owned,consumer,forbidden) in descriptions.items():
        enabled=manifest['components'][name]['enabled']
        if any((target in targets) != enabled for target in owned):
            raise ValueError(f'{name}: target availability disagrees with component manifest')
        declared_tests=manifest['components'][name]['tests']
        if any((test in test_names) != enabled for test in declared_tests):
            raise ValueError(f'{name}: test registration disagrees with component manifest')
        record={'enabled':enabled,'targets':owned if enabled else [],
                'tests':declared_tests if enabled else []}
        if enabled:
            commands=run('commands',consumer)
            links=[]
            for command in commands.splitlines():
                words=shlex.split(command)
                if '-o' in words and '-c' not in words and Path(words[words.index('-o')+1]).name==consumer:
                    links.append(command)
            if len(links)!=1:
                raise ValueError(f'{name}: expected exactly one executable link command')
            if any(library in links[0] for library in forbidden):
                raise ValueError(f'{name}: standalone executable links forbidden dependencies: {links[0]}')
            own_library=f'libkin_{name}.a'
            if own_library not in links[0]:
                raise ValueError(f'{name}: standalone executable does not link {own_library}')
            record['standalone_link']=links[0]
        else:
            sources={'audio':['/engine/src/audio/','/engine/src/ecs/audio.cpp'],
                     'physics':['/engine/src/physics/','/engine/src/ecs/physics.cpp'],
                     'scripting':['/engine/src/scripting/']}[name]
            if any(source in umbrella for source in sources):
                raise ValueError(f'{name}: disabled component sources remain in umbrella build plan')
            if consumer in targets:
                raise ValueError(f'{name}: disabled standalone test target remains')
        rows[name]=record
    return {'schema':'kin.component-target-audit/1','components':rows,'valid':True,
            'note':'Ninja build/link plan audit; runtime behavior is checked by component tests.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--manifest',type=Path)
    args=parser.parse_args()
    try:
        result=audit(args.build.resolve(), args.manifest)
    except (OSError,ValueError,KeyError,subprocess.SubprocessError) as error:
        parser.exit(1,f'Component target audit failed: {error}\n')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2)+'\n', encoding='utf-8')
    print('Component target isolation verified: '+', '.join(f"{key}={'ON' if value['enabled'] else 'OFF'}" for key,value in result['components'].items()))


if __name__=='__main__':
    main()
