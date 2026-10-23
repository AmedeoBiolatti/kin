#!/usr/bin/env python3
"""CPU-only recipe checks; native mutations must fail before any GPU initialization."""
import argparse
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

import generate as yard


class RecipeTests(unittest.TestCase):
    def test_geometry_and_tangents(self):
        for mesh in yard.meshes():
            tangents = mesh.tangents()
            for i in range(0, len(mesh.indices), 3):
                a, b, c = [mesh.vertices[j] for j in mesh.indices[i:i + 3]]
                face = yard.cross(yard.add(b[0], yard.mul(a[0], -1)),
                                  yard.add(c[0], yard.mul(a[0], -1)))
                self.assertGreater(yard.dot(face, yard.add(a[1], yard.add(b[1], c[1]))), 0)
            for i, (_, normal, _, bone) in enumerate(mesh.vertices):
                t = tangents[i * 4:i * 4 + 3]
                self.assertAlmostEqual(yard.dot(t, t), 1, places=5)
                self.assertAlmostEqual(yard.dot(t, normal), 0, places=5)
                self.assertTrue(0 <= bone < 6)

    def test_tiers_deterministic_bounded_and_native_loadable(self):
        for tier in yard.TIERS:
            path = ROOT / (tier + '.yrd')
            info = yard.generate(path, tier)
            self.assertEqual(info, yard.generate(path, tier))
            self.assertLess(info['bytes'], 3 * 1048576)
            self.assertLess(info['source_mesh_triangles'], 5000)
            self.assertEqual(info['moving_material_groups'], info['materials'])
            p = subprocess.run([BINARY, '--recipe', path, '--validate-recipe'], capture_output=True, text=True, timeout=10)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertNotIn('gpu backend', p.stdout + p.stderr)

    def offsets(self, data):
        offset = 64
        mesh_offsets = []
        for _ in range(10):
            nv, ni, nj = struct.unpack_from('<3I', data, offset)
            vertices = offset + 12
            indices = vertices + nv * 48
            bones = indices + ni * 4
            weights = bones + nv * 16
            mesh_offsets.append((vertices, indices, bones, weights, nv))
            offset = bones + (nv * 32 if nj else 0)
        materials = struct.unpack_from('<I', data, 16)[0]
        offset += (materials + 2) * 16
        for _ in range(5):
            side = struct.unpack_from('<I', data, offset)[0]
            offset += 4 + side * side * 4
        offset += 8 + 128 * 64 * 12 + 1440 * 24
        props = struct.unpack_from('<I', data, 12)[0]
        actors, smoke, world = struct.unpack_from('<3I', data, 20)
        return mesh_offsets, offset + world * 76, offset + (world + props + actors + smoke) * 76

    def test_distributed_updates_and_baked_skin(self):
        path = ROOT / 'motion.yrd'
        yard.generate(path, 'medium')
        data = path.read_bytes()
        _, prop_offset, pivots_offset = self.offsets(data)
        cells = set()
        for i in range(8000):
            o = prop_offset + i * 76
            if struct.unpack_from('<I', data, o + 8)[0]:
                x, _, z = struct.unpack_from('<3f', data, o + 12 + 48)
                cells.add((int((x + 40) / 20), int((z + 40) / 20)))
        self.assertEqual(len(cells), 16, 'motion must touch the entire yard')
        for frame in [1, 17, 30, 67, 119]:
            for bone, pivot in enumerate(yard.PIVOTS):
                offset = pivots_offset + 72 + (frame * 6 + bone) * 128
                palette = struct.unpack_from('<16f', data, offset)
                pose = struct.unpack_from('<16f', data, offset + 64)
                # Applying the palette must leave the joint pivot fixed, and
                # equal pose * inverse(rest) for an arbitrary off-pivot point.
                for axis in range(3):
                    self.assertAlmostEqual(sum(palette[j * 4 + axis] * pivot[j] for j in range(3)) + palette[12 + axis], pivot[axis], places=6)
                    point = (.7, .3, -.4)
                    native = sum(palette[j * 4 + axis] * point[j] for j in range(3)) + palette[12 + axis]
                    godot = sum(pose[j * 4 + axis] * (point[j] - pivot[j]) for j in range(3)) + pose[12 + axis]
                    self.assertAlmostEqual(native, godot, places=6)
        arm = pivots_offset + 72 + (30 * 6 + 2) * 128
        self.assertGreater(abs(struct.unpack_from('<f', data, arm + 4)[0]), .4,
                           'animated pose must differ substantially from bind pose')

    def test_diagnostic_option_limits_before_gpu(self):
        path = ROOT / 'option-limits.yrd'
        yard.generate(path, 'small')
        for option, value in [('--warmup', '-1'), ('--warmup', '301'),
                              ('--point-lights', '-2'), ('--point-lights', '17'),
                              ('--receive-shadows', '2')]:
            p = subprocess.run([BINARY, '--recipe', path, '--metrics', ROOT / 'unused.csv',
                                option, value], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(p.returncode, 0, option)
            self.assertIn('yard:', p.stderr)
            self.assertNotIn('gpu backend', p.stdout + p.stderr)

    def test_native_rejects_corruption(self):
        path = ROOT / 'valid.yrd'
        yard.generate(path, 'small')
        data = path.read_bytes()
        meshes, props, _ = self.offsets(data)
        mutations = [(12, '<I', 20001), (64, '<I', 10001),
                     (meshes[0][0], '<f', math.nan),
                     (meshes[0][1], '<I', 999999),
                     (meshes[8][2], '<I', 6),
                     (meshes[8][3], '<4f', (0, 0, 0, 0)),
                     (props + 12 + 60, '<f', 0)]
        invalid = []
        for offset, fmt, value in mutations:
            d = bytearray(data)
            struct.pack_into(fmt, d, offset, *(value if isinstance(value, tuple) else (value,)))
            invalid.append(d)
        invalid.extend([data[:-1], data + b'trailing'])
        for i, corrupted in enumerate(invalid):
            bad = ROOT / f'bad-{i}.yrd'
            bad.write_bytes(corrupted)
            p = subprocess.run([BINARY, '--recipe', bad, '--validate-recipe'], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(p.returncode, 0, f'mutation {i} accepted')
            self.assertIn('yard:', p.stderr)
            self.assertNotIn('gpu backend', p.stdout + p.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--root', required=True, type=Path, help='Linux disk scratch directory')
    args = parser.parse_args()
    BINARY = str(args.binary.resolve())
    args.root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='yard-tests-', dir=args.root) as temp:
        ROOT = Path(temp)
        unittest.main(argv=['test_recipe.py'])
