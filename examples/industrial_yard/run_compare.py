#!/usr/bin/env python3
"""Serial, RAM-guarded industrial-yard comparison. No asset downloads or installs."""
import argparse
import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


guard = module('yard_guard', HERE.parent / 'blockfield/run_render_compare.py')
generator = module('yard_generator', HERE / 'generate.py')


def environment(root):
    env = dict(os.environ, TMPDIR=str(root / 'tmp'), KIN_RENDER_BACKEND='gpu',
               KIN_REQUIRE_GPU_TESTS='1')
    return env


def checked_run(command, label, root, env, limit):
    result = guard.bounded_run(command, label, root, env=env, rss_limit=limit)
    # Godot can return success even if export encountered a script error.
    log = (root / (label + '.log')).read_text()
    if 'ERROR:' in log or 'SCRIPT ERROR' in log:
        raise RuntimeError(f'{label}: engine reported an error: {log[-3000:]}')
    return result


def prepare(root, godot, env, build):
    if build:
        checked_run(['cmake', '--build', REPO / 'build', '--target', 'industrial_yard', '-j1'],
                    'build-yard', root, env, 2048)
    project = root / 'godot-project'
    project.mkdir(exist_ok=True)
    for source in (HERE / 'godot').iterdir():
        if source.is_file():
            shutil.copy2(source, project / source.name)
    template = (godot / 'linux_release.x86_64').resolve()
    (project / 'export_presets.cfg').write_text(
        '[preset.0]\nname="Linux"\nplatform="Linux"\nrunnable=true\n'
        'export_filter="all_resources"\ninclude_filter=""\nexclude_filter=""\n'
        '[preset.0.options]\ncustom_template/release=' + json.dumps(str(template)) + '\n'
        'binary_format/architecture="x86_64"\nbinary_format/embed_pck=false\n')
    editors = list(godot.glob('Godot_v*-stable_linux.x86_64'))
    if len(editors) != 1 or not template.is_file():
        raise RuntimeError('Expected one Godot Linux editor and linux_release.x86_64 template')
    checked_run([editors[0], '--headless', '--path', project, '--editor', '--export-pack',
                 'Linux', root / 'industrial-yard-godot.pck'], 'export-godot', root, env, 1024)
    binary = root / 'industrial-yard-godot'
    if not binary.exists():
        # Copy only when hardlinks are unavailable (e.g. another filesystem).
        try:
            os.link(template, binary)
        except OSError:
            shutil.copy2(template, binary)


def run(root, env, engine, case, frames, label, screenshot=False):
    recipe = root / (case['tier'] + '.yrd')
    common = ['--recipe', str(recipe), '--measured', str(frames), '--metrics', str(root / (label + '.csv'))]
    for key, value in case.items():
        if key != 'tier':
            common += ['--' + key, str(value)]
    report = root / (label + '.json')
    if engine == 'kin':
        command = [REPO / 'build/bin/industrial_yard', *common, '--report=' + str(report), '--profile-cpu']
        if screenshot:
            command.append('--screenshot=' + str(root / (label + '.png')))
    else:
        command = [root / 'industrial-yard-godot', '--display-driver', 'x11', '--rendering-driver',
                   'vulkan', '--rendering-method', 'forward_plus', '--render-thread', 'safe',
                   '--audio-driver', 'Dummy', '--disable-vsync', '--', *common, '--report', str(report)]
        if screenshot:
            command += ['--screenshot', str(root / (label + '.png'))]
    process = checked_run(['taskset', '-c', '0-15', *command], label, root, env, 512 if engine == 'kin' else 1024)
    state = json.loads(report.read_text())
    if engine == 'kin':
        assert state['status'] == 'ok'
        state = state['scenes'][0]['state']
    else:
        assert state['release'] and state['renderer'] == 'forward_plus'
        assert state['skin_validations'] == 2 and state['skin_validation_error'] < .0001
        assert state['motion_validation_error'] < .00001
    rows = list(csv.DictReader((root / (label + '.csv')).open()))
    assert len(rows) == frames, (label, len(rows))
    offset = case.get('route-offset', 0)
    assert state['route_frame'] == (frames + offset) % 1440
    for index, row in enumerate(rows, 1):
        assert int(row['frame']) == 300 + index
        assert int(row['route_frame']) == (index + offset) % 1440
        assert all(float(row[k]) > 0 for k in ['frame_interval_ms', 'draws', 'triangles'])
        if engine == 'kin':
            assert 0 <= int(row['skinned_copies']) <= state['actors']
            assert int(row['skin_matrices']) == 6 * state['actors']
            assert int(row['local_shadow_maps']) == (state['spot_lights'] if state['shadows'] else 0)
            assert int(row['local_shadow_bytes']) <= state['spot_lights'] * 1024 * 1024 * 4
    if state['shadows']:
        assert max(int(r['local_shadow_draws']) for r in rows) > 0
    else:
        assert all(int(r['local_shadow_draws']) == 0 for r in rows)
    summary = {'name': label, 'engine': engine, 'case': case, 'frames': frames,
               'peak_rss_mib': process['peak_rss_mib']}
    for key in rows[0]:
        values = [float(row[key]) for row in rows]
        summary[key] = {stat: guard.percentile(values, percent)
                        for stat, percent in [('p50', 50), ('p95', 95), ('p99', 99)]}
        summary[key]['p50'] = statistics.median(values)
        summary[key]['mean'] = statistics.mean(values)
    print(label, 'frame p50/p95:', summary['frame_interval_ms']['p50'],
          summary['frame_interval_ms']['p95'], flush=True)
    return state, rows, summary


def verify_pair(kin, godot):
    a, ar, _ = kin
    b, br, _ = godot
    keys = ['tier', 'props', 'actors', 'smoke', 'materials', 'structures', 'glass', 'motion', 'animation',
            'shadows', 'taa', 'ao', 'contact', 'point_lights', 'spot_lights', 'shadow_resolution',
            'shadow_near', 'shadow_depth_bits', 'render_width', 'render_height', 'route_frame',
            'moving_props', 'moving_batches', 'source_triangles', 'scene_triangles', 'recipe_bytes']
    for key in keys:
        assert a[key] == b[key], (key, a[key], b[key])
    for left, right in zip(ar, br):
        for key in ['moving_x_sum', 'moving_y_sum', 'moving_z_sum']:
            assert abs(float(left[key]) - float(right[key])) < .00002, (key, left, right)


def provenance(root, env, args):
    paths = [REPO / 'build/bin/industrial_yard', root / 'industrial-yard-godot',
             root / 'industrial-yard-godot.pck', REPO / 'build/CMakeCache.txt',
             REPO / 'engine/CMakeLists.txt', REPO / 'engine/src/renderer/gpu/gpu_renderer3d.cpp',
             REPO / 'engine/src/render3d/scene3d.cpp']
    paths += [p for p in HERE.rglob('*') if p.is_file() and p.suffix in ['.py', '.cpp', '.hpp', '.gd', '.godot', '.tscn']]
    result = {'command': sys.argv, 'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip(),
              'files': [{'path': str(p), 'bytes': p.stat().st_size,
                         'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in paths],
              'kin_environment': {k: v for k, v in env.items() if k.startswith('KIN_')},
              'cpu': subprocess.check_output(['lscpu'], text=True),
              'gpu': subprocess.check_output(['nvidia-smi', '--query-gpu=name,driver_version,temperature.gpu,power.draw', '--format=csv'], text=True),
              'thermal': {p.name: {'type': (p / 'type').read_text().strip(), 'millidegrees': (p / 'temp').read_text().strip()}
                          for p in Path('/sys/class/thermal').glob('thermal_zone*') if (p / 'temp').exists()},
              'other_processes': subprocess.check_output(['ps', '-eo', 'pid,etime,comm,pcpu,rss', '--sort=-pcpu'], text=True).splitlines()[:15],
              'limits': {'kin_rss_mib': 512, 'godot_rss_mib': 1024, 'build_rss_mib': 2048},
              'note': 'Shared desktop, unlocked clocks, RSS excludes dedicated VRAM. Metadata is a startup snapshot, not a continuous thermal/load profile.'}
    (root / (args.tier + '-' + args.suite + '-provenance.json')).write_text(json.dumps(result, indent=2) + '\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, required=True, help='Linux disk scratch directory, never RAM-backed /tmp')
    p.add_argument('--godot', type=Path, required=True, help='Existing editor + release template directory')
    p.add_argument('--build', action='store_true')
    p.add_argument('--prepare', action='store_true')
    p.add_argument('--tier', choices=generator.TIERS, default='medium')
    p.add_argument('--frames', type=int, default=1440)
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--suite', choices=['full', 'baseline', 'preflight', 'ablations'], default='full')
    args = p.parse_args()
    if not 1 <= args.frames <= 6000 or not 1 <= args.repeats <= 5:
        p.error('Use 1..6000 measured frames and 1..5 repeats')
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    (root / 'tmp').mkdir(exist_ok=True)
    env = environment(root)
    if args.prepare or args.build:
        prepare(root, args.godot.resolve(), env, args.build)
    provenance(root, env, args)
    recipe = root / (args.tier + '.yrd')
    info = generator.generate(recipe, args.tier)
    (root / (args.tier + '-recipe.json')).write_text(json.dumps(info, indent=2) + '\n')
    cases = []
    if args.suite in ['full', 'baseline']:
        cases += [('s1', {'resolution-scale': 1}), ('s2', {'resolution-scale': 2})]
    if args.suite in ['full', 'ablations']:
        cases += [('no-skin', {'actors': 0}), ('frozen-poses', {'animation': 0}), ('no-motion', {'motion': 0}),
                  ('no-shadow', {'local-shadows': 0}), ('no-alpha', {'smoke': 0, 'glass': 0}),
                  ('ao-taa', {'ao': 1, 'taa': 1})]
    if args.suite == 'preflight':
        cases = [('outside', {'route-offset': 0}), ('inside', {'route-offset': 540})]
    summaries = []
    for name, options in cases:
        case = {'tier': args.tier, **options}
        for repeat in range(args.repeats):
            pair = {}
            # Alternate order to reduce warm-order bias. Sequential GPU applications only.
            for engine in (['kin', 'godot'] if repeat % 2 == 0 else ['godot', 'kin']):
                label = f'{engine}-{args.tier}-{name}-r{repeat + 1}'
                pair[engine] = run(root, env, engine, case, args.frames, label, args.suite == 'preflight')
                summaries.append(pair[engine][2])
            verify_pair(pair['kin'], pair['godot'])
            assert hashlib.sha256(recipe.read_bytes()).hexdigest() == info['sha256']
            (root / (args.tier + '-' + args.suite + '-runs.json')).write_text(json.dumps(summaries, indent=2) + '\n')
    print('All route, population, motion, skin and feature checks passed.', flush=True)


if __name__ == '__main__':
    main()
