#!/usr/bin/env python3
"""Independent calibration vectors and consistency of the exported build data."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from sitl.target_properties import TARGETS, transform

assert set(TARGETS) == {'mt11', 'a8', 'zr10', 'z1mini'}
with tempfile.TemporaryDirectory(prefix='apcam-target-test-') as temp:
    for name, target in TARGETS.items():
        binary = str(Path(temp) / name)
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-std=c11',
                        '-DAPCAM_TARGET=' + target['target_id'],
                        '-I' + str(ROOT/'include'), '-I' + str(ROOT/'camera_app/include'),
                        str(ROOT/'tests/test_targets.c'), '-lm', '-o', binary], check=True)
        subprocess.run([binary], check=True)
        for role in ('main', 'sub', 'recording'):
            assert target[role + '_resolutions'] & (1 << target['default_' + role + '_resolution'])
        for lens in range(1, int(target['num_lenses']) + 1):
            assert 0 < target[f'lens{lens}_fov_h'] < 180
        for channel in ('feedback', 'private_feedback', 'angle_command', 'rate_command'):
            for inverted in (False, True):
                prefix = 'gimbal_' + channel + ('_inverted' if inverted else '_upright')
                matrix = target[prefix + '_matrix']
                for i in range(3):
                    for j in range(3):
                        assert sum(matrix[3*i+k] * matrix[3*j+k] for k in range(3)) == (i == j)
                for rates in (False, True):
                    vector = (12, -34, 56)
                    converted = transform(target, channel, inverted, vector, rates=rates)
                    recovered = transform(target, channel, inverted, converted, rates=rates, inverse=True)
                    assert list(vector) == recovered
print('PASS four-target compiler exports, profile defaults, orthogonal mappings and known calibration vectors')
