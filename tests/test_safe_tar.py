#!/usr/bin/env python3
"""Exercise the legacy safe tar extraction path independently of host Python."""
import io
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from safe_tar import safe_extract


def make_tar(entries):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w') as archive:
        for name, kind, data, mode, linkname in entries:
            member = tarfile.TarInfo(name)
            member.mode = mode
            if kind == 'file':
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
            else:
                member.type = tarfile.SYMTYPE if kind == 'symlink' else tarfile.DIRTYPE
                member.linkname = linkname or ''
                archive.addfile(member)
    return output.getvalue()


class SafeTar(unittest.TestCase):
    def extract_legacy(self, data, destination):
        with mock.patch.object(tarfile, 'data_filter', None, create=True):
            safe_extract(io.BytesIO(data), destination)

    def test_hardened_modes_and_relative_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'out'
            self.extract_legacy(make_tar([
                ('.', 'dir', b'', 0o777, None),
                ('tool/bin/compiler', 'file', b'compiler', 0o4777, None),
                ('tool/bin/nonexec', 'file', b'data', 0o055, None),
                ('tool/link', 'symlink', b'', 0o777, 'bin/compiler'),
            ]), root)
            self.assertEqual((root / 'tool/bin/compiler').read_bytes(), b'compiler')
            self.assertEqual((root / 'tool/bin/compiler').stat().st_mode & 0o777, 0o755)
            self.assertEqual((root / 'tool/bin/nonexec').stat().st_mode & 0o777, 0o044)
            self.assertEqual(os.readlink(root / 'tool/link'), 'bin/compiler')

    def test_chained_symlink_escape_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'out'
            archive = make_tar([
                ('NAME/', 'dir', b'', 0o777, None),
                ('inside/', 'dir', b'', 0o777, None),
                ('NAME/up', 'symlink', b'', 0o777, '../inside'),
                ('NAME/up/up2', 'symlink', b'', 0o777, '..'),
                ('NAME/up/up2/pwned', 'file', b'bad', 0o666, None),
            ])
            with self.assertRaises(RuntimeError):
                self.extract_legacy(archive, root)
            self.assertFalse((Path(directory) / 'pwned').exists())
            self.assertFalse((root / 'inside/pwned').exists())


if __name__ == '__main__':
    unittest.main()
