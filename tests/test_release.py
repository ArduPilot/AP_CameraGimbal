#!/usr/bin/env python3
"""Exercise release layout and failed-build recovery without cross compilers."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_release as release


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='apcam-release-test-')
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        shutil.copytree(ROOT / 'packaging/release', self.repo / 'packaging/release')
        def git(*args):
            return subprocess.check_output(['git', *args], cwd=self.repo, stderr=subprocess.DEVNULL)
        git('init')
        git('add', 'packaging')
        git('-c', 'user.name=Test', '-c', 'user.email=test@example.com', 'commit', '-m', 'Fixture')
        self.revision = git('rev-parse', 'HEAD').decode().strip()
        self.output = self.repo / 'release'
        self.make = self.repo / 'fake-make'
        self.make.write_text('''#!/usr/bin/env python3
from pathlib import Path
import sys
args = dict(arg.split('=', 1) for arg in sys.argv[2:])
out = Path(next(value for key, value in args.items() if key.endswith('_OUT')))
out.write_bytes(b'firmware-' + sys.argv[1].encode())
out.with_name(out.name + '.sha256').write_text('old sidecar format')
if Path('fail').exists():
    sys.exit(1)
''')
        self.make.chmod(0o755)

    def build(self, targets):
        release.build_release(self.repo, self.output, 'v1.0', targets, str(self.make))

    def test_layout_and_checksums(self):
        self.build(list(release.TARGETS))
        expected = {
            'A8': 'SIYI_4K_MINI_UpgradeSD.bin',
            'MT11': f'MT11_FW_ArduPilot_v1.0_{self.revision[:6]}.bin',
            'ZR10': 'ZR10_UpgradeSD.bin',
            'Z1-Mini': f'Z1Mini_AP_native_v1.0_{self.revision[:6]}.gcu',
        }
        for name, firmware in expected.items():
            folder = self.output / 'v1.0' / name
            self.assertEqual({p.name for p in folder.iterdir()},
                             {firmware, 'README.md', 'BUILD_INFO.json', 'SHA256SUMS'})
            for line in (folder / 'SHA256SUMS').read_text().splitlines():
                digest, filename = line.split('  ')
                self.assertEqual(digest, hashlib.sha256((folder / filename).read_bytes()).hexdigest())
            info = json.loads((folder / 'BUILD_INFO.json').read_text())
            self.assertEqual(info['revision'], self.revision)
            self.assertFalse(info['dirty'])
            self.assertEqual(info['package'], firmware)
            self.assertIn(firmware, (folder / 'README.md').read_text())
            self.assertIn(self.revision, (folder / 'README.md').read_text())
        self.assertEqual(set(p.name for p in (self.output / 'v1.0').iterdir()), set(expected))

    def test_failed_rebuild_preserves_complete_package(self):
        self.build(['A8'])
        folder = self.output / 'v1.0/A8'
        original = {p.name: p.read_bytes() for p in folder.iterdir()}
        template = self.repo / 'packaging/release/A8.md'
        template.write_text(template.read_text() + '\nUpdated instructions\n')
        (self.repo / 'fail').touch()
        with self.assertRaises(subprocess.CalledProcessError):
            self.build(['A8'])
        self.assertEqual(original, {p.name: p.read_bytes() for p in folder.iterdir()})
        self.assertEqual([p.name for p in folder.parent.iterdir()], ['A8'])
        (self.repo / 'fail').unlink()
        self.build(['A8'])
        self.assertIn('Updated instructions', (folder / 'README.md').read_text())
        self.assertTrue(json.loads((folder / 'BUILD_INFO.json').read_text())['dirty'])
        self.assertEqual([p.name for p in folder.parent.iterdir()], ['A8'])

    def test_target_aliases_and_version_validation(self):
        self.assertEqual(release.target_name('a8'), 'A8')
        self.assertEqual(release.target_name('z1mini'), 'Z1-Mini')
        with self.assertRaises(argparse.ArgumentTypeError):
            release.target_name('unknown')
        with self.assertRaises(ValueError):
            release.build_release(self.repo, self.output, 'post-refactor', ['A8'], str(self.make))
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
