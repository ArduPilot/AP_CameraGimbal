#!/usr/bin/env python3
"""Round trips and failure handling for the radiometric archive converter."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import thermal_to_video as thermal
import numpy as np


class ThermalArchiveTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root/'source'
        self.source.mkdir()
        self.names = ['2026-08-28_02-08-32_%s_I.bin' % ms for ms in ('5', '25', '100')]
        rng = np.random.default_rng(42)
        arrays = [np.arange(thermal.WIDTH*thermal.HEIGHT, dtype=np.uint32).astype('<u2'),
                  rng.integers(0, 65536, size=thermal.WIDTH*thermal.HEIGHT, dtype=np.uint16),
                  np.full(thermal.WIDTH*thermal.HEIGHT, 18532, dtype='<u2')]
        for n, (name, data) in enumerate(zip(self.names, arrays)):
            p = self.source/name
            p.write_bytes(data.astype('<u2').tobytes())
            os.utime(p, ns=(1700000000000000000+n, 1700000000000000000+n))
        self.video = self.root/'thermal.mkv'

    def test_bit_exact_names_times_and_metadata(self):
        thermal.encode(self.source, self.video)
        dest = self.root/'extracted'
        result = thermal.extract(self.video, dest, save_metadata=True)
        self.assertEqual(result['checksums_verified'], 3)
        self.assertEqual(sorted(p.name for p in dest.glob('*.bin')), sorted(self.names))
        for name in self.names:
            self.assertEqual((self.source/name).read_bytes(), (dest/name).read_bytes())
            self.assertEqual((self.source/name).stat().st_mtime_ns, (dest/name).stat().st_mtime_ns)
        first = json.loads((dest/self.names[0]).with_suffix('.json').read_text())
        second = json.loads((dest/self.names[1]).with_suffix('.json').read_text())
        self.assertEqual(first['capture_monotonic_us'], 1)
        self.assertEqual(second['capture_monotonic_us'], 20001)
        self.assertIsNone(first['telemetry'])
        import av
        with av.open(str(self.video)) as container:
            self.assertEqual([p.pts for p in container.demux(video=0) if p.size], [0, 20, 95])

    def test_extract_live_profile_without_archive_fields(self):
        import av
        thermal.encode(self.source, self.video)
        live = self.root/'live.mkv'
        with av.open(str(self.video)) as container, live.open('wb') as out:
            stream = container.streams.video[0]
            out.write(thermal.matroska_header(stream.codec_context.extradata, 200000000, 600))
            for packet in container.demux(stream):
                if not packet.size:
                    continue
                metadata = thermal.packet_metadata(packet)
                for key in ('original_filename', 'source_mtime_ns', 'source_sha256', 'archive_frame_count'):
                    metadata.pop(key)
                out.write(thermal.matroska_frame(bytes(packet), metadata, packet.pts))
        output = self.root/'live-frames'
        result = thermal.extract(live, output)
        self.assertEqual(result, dict(frames=3, raw_bytes=3*thermal.FRAME_BYTES, checksums_verified=0))
        for n, name in enumerate(self.names, 1):
            self.assertEqual((self.source/name).read_bytes(), (output/('%012u.bin' % n)).read_bytes())

    def test_fixed_fps_and_mtime_fallback(self):
        for n, name in enumerate(self.names):
            (self.source/name).rename(self.source/('%s.bin' % n))
        entries, times, duration, source = thermal.inputs(self.source, fps=5)
        self.assertEqual(times, [0, 200, 400])
        self.assertEqual(source, 'legacy_fixed_fps')
        self.assertEqual(thermal.inputs(self.source)[3], 'legacy_file_mtime')

    def test_wrong_size_and_no_overwrite(self):
        (self.source/'bad.bin').write_bytes(b'bad')
        with self.assertRaises(ValueError):
            thermal.encode(self.source, self.video)
        self.assertFalse(self.video.exists())
        (self.source/'bad.bin').unlink()
        self.video.write_bytes(b'keep me')
        with self.assertRaises(ValueError):
            thermal.encode(self.source, self.video)
        self.assertEqual(self.video.read_bytes(), b'keep me')
        with self.assertRaises(ValueError):
            thermal.extract(self.video, self.source)

    def test_checksum_and_truncation(self):
        thermal.encode(self.source, self.video)
        data = self.video.read_bytes()
        digest = hashlib.sha256((self.source/self.names[0]).read_bytes()).hexdigest().encode()
        self.assertIn(digest, data)
        self.video.write_bytes(data.replace(digest, b'0'*64, 1))
        output = self.root/'bad-output'
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            thermal.extract(self.video, output)
        self.assertFalse(output.exists())
        # A clean cut between complete frames is also an incomplete archive.
        at = data.rfind(bytes.fromhex('1f43b675'))
        self.assertGreater(at, 0)
        self.video.write_bytes(data[:at])
        with self.assertRaisesRegex(ValueError, 'incomplete archive'):
            thermal.extract(self.video, output)
        self.assertFalse(output.exists())
        self.assertFalse(list(self.root.glob('.bad-output.*')))

    def test_unsafe_names(self):
        for name in ('../escape.bin', '/escape.bin', 'a/b.bin', 'a\\b.bin', 'C:escape.bin', '\0.bin'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                thermal.output_name(dict(original_filename=name, frame_id=1))


if __name__ == '__main__':
    unittest.main()
