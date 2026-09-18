#!/usr/bin/env python3
"""Independent calibration vectors and consistency of the exported build data."""
import configparser
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from sitl.target_properties import TARGETS, transform

assert set(TARGETS) == {'mt11', 'a8', 'zr10', 'z1mini'}
recording_defaults = {
    'mt11': '3840x2160',
    'a8': '3840x2160',
    'zr10': '2560x1440',
    'z1mini': '1920x1080',
}
resolutions = ('1280x720', '1920x1080', '3840x2160', '2560x1440')
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
        # Fresh firmware installs and SITL use these INI templates; missing
        # settings use the compiled defaults exported into TARGETS instead.
        template = ROOT / ('camera_app/camera.ini' if name == 'mt11'
                           else f'packaging/{name}/camera.ini')
        config = configparser.ConfigParser()
        config.read(template)
        for role, section in (('main', 'stream.main'), ('sub', 'stream.sub'),
                              ('recording', 'recording')):
            compiled = resolutions[target['default_' + role + '_resolution']]
            assert config[section]['resolution'] == compiled, (name, section)
        assert config['recording']['resolution'] == recording_defaults[name], name
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
