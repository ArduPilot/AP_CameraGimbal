#!/usr/bin/env python3
"""Check source bootstrap ordering and recursive Make's exported roots."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BuildDependenciesTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='apcam-dependencies-')
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)
        subprocess.run(['git', 'init', '-q', str(self.repo)], check=True)
        subprocess.run(['git', '-c', 'user.name=Test', '-c', 'user.email=test@example.com',
                        'commit', '-q', '--allow-empty', '-m', 'Fixture'],
                       cwd=self.repo, check=True)
        shutil.copyfile(ROOT / 'Makefile', self.repo / 'Makefile')
        (self.repo / 'build').mkdir()
        (self.repo / 'build/environment.mk').write_text('# Test environment\n')
        # Stand in for downloaded sources; exercise the real Make entry points.
        tools = self.repo / 'tools'
        tools.mkdir()
        bootstrap = tools / 'bootstrap_dependencies.sh'
        bootstrap.write_text('''#!/bin/sh
set -eu
mkdir -p "$1/ss928-mpp" "$1/minimp4"
touch "$1/minimp4/minimp4.h"
echo bootstrap >> calls
''')
        bootstrap.chmod(0o755)
        for path in ['modules/mavlink/message_definitions/v1.0/all.xml',
                     'modules/mavlink/pymavlink/tools/mavgen.py']:
            file = self.repo / path
            file.parent.mkdir(parents=True, exist_ok=True)
            file.touch()
        for component in ['camera_app', 'web']:
            directory = self.repo / component
            directory.mkdir()
            (directory / 'Makefile').write_text('''all:
	@test -f "$(MINIMP4_ROOT)/minimp4.h"
	@echo compile >> ../calls
''')
        # Like build_release.py, this child receives exported dependency roots.
        (self.repo / 'recursive.mk').write_text('''include Makefile
.PHONY: recursive
recursive:
	$(MAKE) all
''')

    def make(self, *args, extra_env=None, expect_success=True):
        env = os.environ.copy()
        for key in ['SS928_MPP_ROOT', 'MINIMP4_ROOT', 'DEPS_ROOT',
                    'MAKEFLAGS', 'MFLAGS', 'MAKELEVEL']:
            env.pop(key, None)
        env.update(extra_env or {})
        result = subprocess.run(['make', '-j4', '-f', 'recursive.mk', *args,
                                 'MT11_VERSION=v1.0', 'MT11_GIT_HASH=123456',
                                 'ZR10_BUILD_HASH=123456', 'Z1MINI_BUILD_HASH=123456'],
                                cwd=self.repo, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if not expect_success:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            return result.stdout
        self.assertEqual(result.returncode, 0, result.stdout)
        calls = self.repo / 'calls'
        return calls.read_text().splitlines() if calls.exists() else []

    def test_plain_make_bootstraps_before_compiling(self):
        self.assertEqual(self.make(), ['bootstrap', 'compile', 'compile'])

    def test_hardware_builds_require_environment_before_any_work(self):
        (self.repo / 'build/environment.mk').unlink()
        for goal in [None, 'all', 'release', 'mt11_package', 'a8_package',
                     'zr10_firmware', 'z1mini_native_package', 'recursive']:
            with self.subTest(goal=goal):
                output = self.make(*([goal] if goal else []), expect_success=False)
                self.assertIn('python3 tools/install_build_environment.py', output)
                self.assertFalse((self.repo / 'calls').exists())

    def test_source_setup_does_not_require_hardware_environment(self):
        (self.repo / 'build/environment.mk').unlink()
        self.assertEqual(self.make('build-dependencies'), ['bootstrap'])

    def test_recursive_make_bootstraps_inherited_default_paths(self):
        self.assertEqual(self.make('recursive'), ['bootstrap', 'compile', 'compile'])

    def test_custom_roots_are_not_downloaded(self):
        self.assertEqual(self.make('build-dependencies', extra_env={
            'SS928_MPP_ROOT': '/custom/mpp', 'MINIMP4_ROOT': '/custom/minimp4'}), [])

    def test_one_default_root_still_bootstraps(self):
        self.assertEqual(self.make('build-dependencies', 'SS928_MPP_ROOT=/custom/mpp'),
                         ['bootstrap'])

    def test_custom_dependency_directory_survives_recursion(self):
        self.assertEqual(self.make('recursive', 'DEPS_ROOT=custom-deps'),
                         ['bootstrap', 'compile', 'compile'])
        self.assertTrue((self.repo / 'custom-deps/minimp4/minimp4.h').is_file())


if __name__ == '__main__':
    unittest.main()
