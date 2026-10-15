#!/usr/bin/env python3
"""Run bounded, sequential Kin/Godot release rendering comparisons on shared meshes.

Set up the standard Godot editor and Linux release template on the Linux disk first.
No third-party Python modules, editor UI, voxel addon, or terrain copy is required.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time


def available_mib():
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):
            return int(line.split()[1]) / 1024
    raise RuntimeError('Cannot read available memory')


def process_tree(pid):
    result = [pid]
    try:
        children = Path(f'/proc/{pid}/task/{pid}/children').read_text().split()
        for child in children:
            result.extend(process_tree(int(child)))
    except FileNotFoundError:
        pass
    return result


def bounded_run(command, label, root, env=None, rss_limit=1536):
    if available_mib() < 2048:
        raise RuntimeError('Less than 2 GiB RAM available; benchmark deferred')
    start = time.monotonic()
    peak = 0
    log_path = root / f'{label}.log'
    time_path = root / f'{label}.time'
    with log_path.open('w') as log:
        process = subprocess.Popen(['/usr/bin/time', '-f', '%M', '-o', str(time_path), *map(str, command)],
                                   stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
        try:
            while process.poll() is None:
                rss = 0
                for pid in process_tree(process.pid):
                    try:
                        for line in Path(f'/proc/{pid}/status').read_text().splitlines():
                            if line.startswith('VmRSS:'):
                                rss += int(line.split()[1]) / 1024
                    except FileNotFoundError:
                        pass
                peak = max(peak, rss)
                if rss > rss_limit or available_mib() < 1024 or time.monotonic() - start > 300:
                    raise RuntimeError(f'{label}: exceeded RAM or time limit ({rss:.0f} MiB RSS)')
                time.sleep(0.1)
        except BaseException:
            import signal
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            raise
    if time_path.exists():
        for line in time_path.read_text().splitlines():
            if line.isdigit():
                peak = max(peak, int(line) / 1024)
    if process.returncode:
        raise RuntimeError(f'{label} failed ({process.returncode}): {log_path.read_text()[-4000:]}')
    result = {'command': list(map(str, command)), 'peak_rss_mib': peak, 'elapsed_seconds': time.monotonic() - start}
    (root / f'{label}.process.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'{label}: {result["elapsed_seconds"]:.1f}s, peak RSS {peak:.1f} MiB', flush=True)
    return result


def percentile(values, percent):
    return sorted(values)[(len(values) - 1) * percent // 100]


def summarize(root, labels, frames):
    runs = []
    for label in labels:
        with (root / f'{label}.csv').open() as source:
            rows = list(csv.DictReader(source))
        if len(rows) != frames:
            raise RuntimeError(f'{label}: expected {frames} rows, got {len(rows)}')
        report = json.loads((root / f'{label}.json').read_text())
        if label.startswith('kin-'):
            report = report['scenes'][0]['state']
        if report.get('triangle_stress') and report.get('camera_layout') == 'top_down_disjoint_tiles':
            expected = report['scene_triangles']
            if any(int(row['triangles']) != expected for row in rows):
                raise RuntimeError(f'{label}: actual submitted triangles differ from {expected}')
        if label.startswith('kin-') and '-lighting-' in label:
            verify_kin_features(report, rows)
        if '-grove-' in label:
            if label.startswith('kin-'):
                verify_kin_features(report, rows)
            if report.get('camera_layout') != 'ground_route' or report.get('extra_texture_bytes') != 81920:
                raise RuntimeError(f'{label}: ground recipe/resources missing')
            if report['extra_mesh_triangles'] != report['foliage_instances'] * 6 + report['transparent_instances'] * 2:
                raise RuntimeError(f'{label}: wrong extra instance counts')
            if report['camera_route_frame'] != frames % 1440:
                raise RuntimeError(f'{label}: wrong route frame')
            for row in rows:
                if (int(row['foliage_requested']) != report['foliage_instances'] or
                    int(row['transparent_requested']) != report['transparent_instances'] or
                    int(row['route_frame']) != (int(row['frame']) - 300) % 1440):
                    raise RuntimeError(f'{label}: recipe counts/camera did not run')
        run = {'name': label, 'rows': len(rows)}
        for metric in rows[0]:
            if metric == 'frame':
                continue
            values = [float(r[metric]) for r in rows]
            if any(v <= 0 for v in values) and metric in ('draws', 'triangles', 'frame_interval_ms'):
                raise RuntimeError(f'{label}: invalid {metric}')
            run[metric] = {'p50': percentile(values, 50), 'p95': percentile(values, 95), 'p99': percentile(values, 99), 'mean': statistics.mean(values)}
        run['peak_rss_mib'] = json.loads((root / f'{label}.process.json').read_text())['peak_rss_mib']
        runs.append(run)
    (root / 'runs.json').write_text(json.dumps(runs, indent=2) + '\n')
    groups = {}
    for run in runs:
        key = run['name'].rsplit('-r', 1)[0]
        groups.setdefault(key, []).append(run)
    summary = {}
    for key, group in groups.items():
        summary[key] = {'repeats': len(group), 'peak_rss_mib': max(r['peak_rss_mib'] for r in group)}
        for metric in group[0]:
            if not isinstance(group[0][metric], dict):
                continue
            summary[key][metric] = {stat: statistics.median(r[metric][stat] for r in group) for stat in ('p50', 'p95', 'p99', 'mean')}
    (root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    return summary


def verify_kin_features(report, rows):
    for key, column in [('point_lights_requested', 'lights'), ('spot_lights_requested', 'spots')]:
        if any(int(r[column]) != report[key] for r in rows):
            raise RuntimeError(f'Requested {key} did not run')
    if any(int(r['samples']) != (1 if report['taa_requested'] else report['samples_requested']) for r in rows):
        raise RuntimeError('Requested AA samples did not run')
    for key, columns in [('shadows_requested', ['shadow_draws', 'shadow_cascades']),
                         ('local_shadows_requested', ['local_shadow_maps', 'local_shadow_draws']),
                         ('taa_requested', ['taa_dispatches']), ('ao_requested', ['ao_dispatches']),
                         ('contact_requested', ['contact_dispatches'])]:
        if report[key] and any(int(r[column]) <= 0 for r in rows for column in columns):
            raise RuntimeError(f'Requested {key} did not run')
    if report['material'] == 'pbr' and report['procedural_texture_bytes'] != 65536:
        raise RuntimeError('Material maps missing')


def lighting_cases(smoke=False):
    base = ['--hdr', '1']
    pbr = base + ['--material', 'pbr', '--environment', '1']
    lamps = pbr + ['--lights', '8', '--spots', '2']
    sun = lamps + ['--shadows', '1']
    shadows = sun + ['--local-shadows', '1']
    all_engines = ['kin', 'godot-mobile', 'godot-forward']
    cases = [('unlit', base, all_engines), ('lit', base + ['--material', 'lit'], all_engines),
             ('pbr-flat', pbr + ['--normal-map', '0'], all_engines), ('pbr-normal', pbr, all_engines),
             ('lights', lamps, all_engines), ('sun-shadow', sun, all_engines), ('local-shadow', shadows, all_engines),
             ('msaa4', shadows + ['--samples', '4'], all_engines),
             ('ssaa2', shadows + ['--resolution-scale', '2'], all_engines),
             ('taa', shadows + ['--taa', '1'], ['kin', 'godot-forward']),
             ('ao-contact', shadows + ['--taa', '1', '--ao', '1', '--contact', '1'], ['kin'])]
    return [c for c in cases if c[0] in ['lights', 'local-shadow', 'msaa4', 'taa', 'ao-contact']] if smoke else cases



def grove_cases(smoke=False):
    counts = [('route', 0, 0), ('foliage-10k', 10000, 0), ('foliage-50k', 50000, 0),
              ('foliage-100k', 100000, 0), ('transparent-512', 0, 512), ('transparent-5k', 0, 5000),
              ('combined', 10000, 512), ('combined-heavy', 100000, 5000)]
    if smoke:
        counts = [c for c in counts if c[0] in ['route', 'foliage-10k', 'transparent-512', 'combined']]
    return [(name, ['--preset', 'grove', '--foliage', str(f), '--transparent', str(t)], ['kin', 'godot-mobile', 'godot-forward'])
            for name, f, t in counts]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--kin', type=Path, required=True)
    parser.add_argument('--base', type=Path, default=Path.home() / 'kin-bench')
    parser.add_argument('--godot-dir', type=Path)
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--grove', action='store_true', help='Ground tour, shared alpha-cutout plants and transparent cards')
    parser.add_argument('--lighting', action='store_true', help='Incremental HDR/material/light/shadow/MSAA/TAA workloads')
    parser.add_argument('--triangles', action='store_true', help='Exact triangle multipliers with disjoint tiled instances, shared geometry and full-field framing')
    parser.add_argument('--scales', type=int, nargs='+', default=None)
    parser.add_argument('--frames', type=int)
    parser.add_argument('--repeats', type=int, default=3)
    args = parser.parse_args()
    if sum([args.lighting, args.triangles, args.grove]) > 1:
        parser.error('Choose --lighting, --triangles or --grove')
    if args.scales is None:
        args.scales = [1] if args.lighting or args.grove else [1, 5, 10, 20, 100]
    if args.frames is None:
        args.frames = 300 if args.triangles or args.lighting or args.grove else 900
    if not 1 <= args.frames <= 6000 or not 1 <= args.repeats <= 5 or any(n < 1 or n > 100 for n in args.scales):
        parser.error('Frames must be 1..6000 and repeats 1..5')
    if args.grove and args.scales != [1]:
        parser.error('Grove uses one terrain copy')
    base = args.base.resolve()
    godot_dir = (args.godot_dir or base / 'godot-4.7.2').resolve()
    editor = godot_dir / 'Godot_v4.7.2-stable_linux.x86_64'
    template = godot_dir / 'linux_release.x86_64'
    if not editor.is_file() or not template.is_file():
        parser.error(f'Expected standard Godot editor and Linux release template under {godot_dir}')
    root = base / (('smoke-grove' if args.smoke else 'grove') if args.grove else (('smoke-lighting' if args.smoke else 'lighting-materials') if args.lighting else (('smoke-triangles' if args.smoke else 'triangle-sweep') if args.triangles else ('smoke' if args.smoke else 'results'))))
    project = base / ('blockfield-godot-grove' if args.grove else 'blockfield-godot')
    root.mkdir(parents=True, exist_ok=True)
    project.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(base).free < 2 * 1024**3:
        raise RuntimeError('Need 2 GiB free disk space for snapshots/cache')
    for source in (Path(__file__).parent / 'godot').iterdir():
        if source.is_file():
            shutil.copy2(source, project / source.name)
    # Only the required Linux release binary; no global template installation.
    export_name = 'blockfield-godot-grove' if args.grove else 'blockfield-godot'
    (project / 'export_presets.cfg').write_text(f'''[preset.0]
name="Linux"
platform="Linux"
runnable=true
export_filter="all_resources"
include_filter=""
exclude_filter=""
export_path="{godot_dir}/{export_name}"

[preset.0.options]
custom_template/release="{template}"
binary_format/architecture="x86_64"
binary_format/embed_pck=false
''')
    bounded_run([editor, '--headless', '--path', project, '--editor', '--import', '--quit'], 'import', root)
    if 'ERROR' in (root / 'import.log').read_text():
        raise RuntimeError('Godot import reported script errors; see import.log')
    exported = godot_dir / export_name
    if exported.exists() and os.path.samefile(exported, template):
        # Never let an exporter overwrite its own custom template through a link.
        exported.unlink()
    bounded_run([editor, '--headless', '--path', project, '--export-release', 'Linux'], 'export', root)
    # The release executable is a byte-for-byte template copy. A hard link avoids
    # storing the same 70 MiB binary twice; the named .pck remains alongside it.
    with exported.open('rb') as left, template.open('rb') as right:
        same = hashlib.file_digest(left, 'sha256').digest() == hashlib.file_digest(right, 'sha256').digest()
    if same:
        exported.unlink()
        os.link(template, exported)
    if args.grove:
        cases = [(32, 75)]
    elif args.triangles or args.lighting:
        cases = [(2 if args.smoke else 32, 90)]
    else:
        cases = [(2, 110)] if args.smoke else [(24, 150), (32, 110), (32, 150)]
    snapshots = {}
    for distance, _ in cases:
        if distance in snapshots:
            continue
        snapshot = base / f'blockfield-d{distance}.bfc'
        manifest = base / 'results' / f'snapshot-d{distance}.json'
        reuse = False
        if (args.triangles or args.lighting or args.grove) and snapshot.is_file() and manifest.is_file():
            previous = json.loads(manifest.read_text())
            with snapshot.open('rb') as source:
                reuse = snapshot.stat().st_size == previous['bytes'] and hashlib.file_digest(source, 'sha256').hexdigest() == previous['sha256']
        if not reuse:
            bounded_run([args.kin, '--generate', snapshot, '--distance', distance], f'generate-d{distance}', root)
        with snapshot.open('rb') as source:
            digest = hashlib.file_digest(source, 'sha256').hexdigest()
        snapshots[distance] = snapshot
        (root / f'snapshot-d{distance}.json').write_text(json.dumps({'path': str(snapshot), 'bytes': snapshot.stat().st_size, 'sha256': digest}, indent=2) + '\n')
    dressing = base / 'blockfield-grove.bfd'
    if args.grove:
        bounded_run([args.kin, '--generate-dressing', dressing], 'generate-dressing', root)
        with dressing.open('rb') as source:
            digest = hashlib.file_digest(source, 'sha256').hexdigest()
        (root / 'dressing.json').write_text(json.dumps({'path': str(dressing), 'bytes': dressing.stat().st_size, 'sha256': digest}, indent=2) + '\n')
    frames = 60 if args.smoke else args.frames
    repeats = 1 if args.smoke else args.repeats
    labels = []
    if args.lighting or args.grove:
        workloads_all = grove_cases(args.smoke) if args.grove else lighting_cases(args.smoke)
        skipped = [{'case': name, 'engine': engine} for name, _, supported in workloads_all
                   for engine in ['kin', 'godot-mobile', 'godot-forward'] if engine not in supported]
        (root / 'unsupported.json').write_text(json.dumps(skipped, indent=2) + '\n')
        for repeat in range(1, repeats + 1):
            workloads = list(workloads_all)
            if repeat % 2 == 0:
                workloads.reverse()
            for name, options, supported in workloads:
                engines = supported if repeat % 2 else list(reversed(supported))
                for copies in args.scales:
                    for engine in engines:
                        label = f'{engine}-grove-{name}-x{copies}-r{repeat}' if args.grove else f'{engine}-lighting-{name}-x{copies}-r{repeat}'
                        labels.append(label)
                        common = ['--snapshot', str(snapshots[cases[0][0]]), '--metrics', str(root / f'{label}.csv'),
                                  '--measured', str(frames), '--copies', str(copies), '--report', str(root / f'{label}.json'), *options]
                        if args.grove:
                            common += ['--dressing', str(dressing)]
                        env = dict(os.environ)
                        if engine == 'kin':
                            env['KIN_RENDER_BACKEND'] = 'gpu'
                            command = [args.kin, *common]
                            if repeat == 1:
                                command += ['--screenshot=' + str(root / f'{label}.png')]
                        else:
                            renderer = 'mobile' if engine == 'godot-mobile' else 'forward_plus'
                            command = [exported, '--display-driver', 'x11', '--rendering-driver', 'vulkan', '--rendering-method', renderer,
                                       '--render-thread', 'safe', '--audio-driver', 'Dummy', '--disable-vsync', '--', *common]
                            if repeat == 1:
                                command += ['--screenshot', str(root / f'{label}.png')]
                        bounded_run(command, label, root, env=env)
                        if 'ERROR' in (root / f'{label}.log').read_text():
                            raise RuntimeError(f'Render errors in {label}')
        summary = summarize(root, labels, frames)
        for label, run in summary.items():
            print(f'{label}: p50 {run["frame_interval_ms"]["p50"]:.3f} ms / p95 {run["frame_interval_ms"]["p95"]:.3f} ms, {run["peak_rss_mib"]:.0f} MiB RSS')
        return
    levels = ([1, 5, 100] if args.smoke else args.scales) if args.triangles else [None]
    for repeat in range(1, repeats + 1):
        engines = ['kin', 'godot-mobile', 'godot-forward']
        if repeat % 2 == 0:
            engines.reverse()
        for distance, fov in (cases if repeat % 2 else list(reversed(cases))):
            for copies in (levels if repeat % 2 else list(reversed(levels))):
                for engine in engines:
                    multiplier = f'-x{copies}' if copies is not None else ''
                    label = f'{engine}-d{distance}-f{fov}{multiplier}-r{repeat}'
                    labels.append(label)
                    common = ['--snapshot', str(snapshots[distance]), '--metrics', str(root / f'{label}.csv'),
                              '--measured', str(frames), '--fov', str(fov), '--report', str(root / f'{label}.json')]
                    if copies is not None:
                        common += ['--copies', str(copies)]
                    env = dict(os.environ)
                    if engine == 'kin':
                        env['KIN_RENDER_BACKEND'] = 'gpu'
                        command = [args.kin, *common]
                        if repeat == 1:
                            command.append('--screenshot=' + str(root / f'{label}.png'))
                    else:
                        renderer = 'mobile' if engine == 'godot-mobile' else 'forward_plus'
                        command = [exported, '--display-driver', 'x11', '--rendering-driver', 'vulkan',
                                   '--rendering-method', renderer, '--render-thread', 'safe', '--audio-driver', 'Dummy', '--disable-vsync', '--', *common]
                        if repeat == 1:
                            command += ['--screenshot', str(root / f'{label}.png')]
                    bounded_run(command, label, root, env=env)
    summary = summarize(root, labels, frames)
    for label, run in summary.items():
        print(f'{label}: frame interval p50 {run["frame_interval_ms"]["p50"]:.3f} ms / p95 {run["frame_interval_ms"]["p95"]:.3f} ms, {run["draws"]["mean"]:.0f} draws / {run["triangles"]["mean"]:.0f} triangles, {run["peak_rss_mib"]:.0f} MiB RSS')


if __name__ == '__main__':
    main()
