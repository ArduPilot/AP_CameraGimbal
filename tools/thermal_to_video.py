#!/usr/bin/env python3
"""Losslessly convert legacy 640x512 gray16le thermal files to APCG Matroska.

Encode:  thermal_to_video.py CAPTURE_DIRECTORY OUTPUT.mkv
Extract: thermal_to_video.py --extract INPUT.mkv OUTPUT_DIRECTORY
Requires numpy and PyAV >=18.1. Existing outputs are never overwritten.
"""
import argparse
import datetime
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import struct
import tempfile
import time

WIDTH, HEIGHT = 640, 512
FRAME_BYTES = WIDTH * HEIGHT * 2
SCHEMA = 'apcg.thermal.v1'
ADDITION_TYPE = 0x41504347  # Experimental APCG FourCC, matching firmware.
# Milliseconds are an integer, not a decimal fraction: _13 means 13 ms.
FILENAME_TIME = re.compile(r'^(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})_(\d{1,3})(?:_\d+)?_I\.bin$')


def uint(value):
    return value.to_bytes(max(1, (value.bit_length() + 7) // 8), 'big')


def element(tag, payload):
    """EBML element with a known length (all-ones lengths are reserved)."""
    size = len(payload)
    length = next(n for n in range(1, 9) if size < (1 << (7*n)) - 1)
    return uint(tag) + ((1 << (7*length)) | size).to_bytes(length, 'big') + payload


def integer(tag, value):
    return element(tag, uint(value))


def text(tag, value):
    return element(tag, value.encode('utf-8'))


def matroska_header(extra, frame_duration_ns, duration_ms):
    """Same FFV1 track and BlockAdditionMapping as thermal_matroska.h."""
    ebml = b''.join(integer(tag, value) for tag, value in
                    ((0x4286, 1), (0x42F7, 1), (0x42F2, 4), (0x42F3, 8)))
    ebml += text(0x4282, 'matroska') + integer(0x4287, 4) + integer(0x4285, 2)
    segment = bytes.fromhex('1853806701ffffffffffffff')  # unknown Segment length
    info = integer(0x2AD7B1, 1000000)  # 1 ms timebase
    info += text(0x4D80, 'AP_CameraGimbal') + text(0x5741, 'thermal_to_video.py')
    info += element(0x4489, struct.pack('>d', duration_ms))
    track = b''.join(integer(tag, value) for tag, value in
                     ((0xD7, 1), (0x73C5, 1), (0x83, 1), (0x9C, 0), (0x55EE, 2)))
    track += integer(0x23E383, frame_duration_ns)
    track += text(0x86, 'V_FFV1') + element(0x63A2, extra)
    video = integer(0xB0, WIDTH) + integer(0xBA, HEIGHT)
    video += element(0x55B0, integer(0x55B2, 16))
    track += element(0xE0, video)
    mapping = integer(0x41F0, 2) + text(0x41A4, 'apcg.thermal.v1 JSON')
    mapping += integer(0x41E7, ADDITION_TYPE)
    track += element(0x41E4, mapping)
    return element(0x1A45DFA3, ebml) + segment + element(0x1549A966, info) + element(0x1654AE6B, element(0xAE, track))


def matroska_frame(encoded, metadata, pts_ms):
    block = element(0xA1, b'\x81\0\0\0' + encoded)
    extra = json.dumps(metadata, separators=(',', ':'), allow_nan=False).encode('utf-8')
    additions = element(0x75A1, element(0xA6, integer(0xEE, 2) + element(0xA5, extra)))
    return element(0x1F43B675, integer(0xE7, pts_ms) + element(0xA0, block + additions))


def filename_time(path):
    match = FILENAME_TIME.fullmatch(path.name)
    if not match:
        return None
    date = datetime.datetime.strptime(match[1], '%Y-%m-%d_%H-%M-%S')
    delta = date - datetime.datetime(1970, 1, 1)
    return (delta.days*86400 + delta.seconds)*1000 + int(match[2])


def inputs(directory, fps=None):
    if not directory.is_dir():
        raise ValueError('encoding input must be a directory')
    files = list(directory.glob('*.bin'))
    if not files:
        raise ValueError('no .bin thermal frames found')
    stamps = [filename_time(p) for p in files]
    use_names = all(t is not None for t in stamps)
    entries = []
    for path, stamp in zip(files, stamps):
        info = path.stat()
        if not path.is_file() or info.st_size != FRAME_BYTES:
            raise ValueError('%s: expected %u bytes, got %u' % (path, FRAME_BYTES, info.st_size))
        entries.append((stamp if use_names else info.st_mtime_ns//1000000, path, info.st_mtime_ns))
    entries.sort(key=lambda row: (row[0], row[1].name))
    first = entries[0][0]
    timestamps = [round(i*1000/fps) if fps else stamp-first for i, (stamp, _, _) in enumerate(entries)]
    differences = [b-a for a, b in zip(timestamps, timestamps[1:]) if b>a]
    interval_ms = 1000/fps if fps else statistics.median(differences) if differences else 200
    source = 'legacy_fixed_fps' if fps else 'legacy_filename' if use_names else 'legacy_file_mtime'
    return entries, timestamps, max(1, round(interval_ms*1000000)), source


def metadata_for_frame(path, data, pixels, number, count, pts_ms, mtime_ns, timing, rotation):
    lo, hi = int(pixels.min()), int(pixels.max())
    return dict(schema=SCHEMA, frame_id=number, capture_monotonic_us=pts_ms*1000+1,
                timestamp_source=timing, capture_clock='reconstructed_relative', simulated=False,
                width=WIDTH, height=HEIGHT, pixel_format='gray16le', bits_per_sample=16,
                temperature_scale_k=1/64, temperature_offset_k=0, gain=-1,
                minimum_raw=lo, maximum_raw=hi, minimum_c=lo/64-273.15, maximum_c=hi/64-273.15,
                rotation_deg=rotation, hfov_deg=24.2, calibration='nominal_pinhole',
                altitude_datum='AMSL', gimbal_frame='roll_pitch_level_yaw_vehicle', telemetry=None,
                original_filename=path.name, source_mtime_ns=mtime_ns,
                source_sha256=hashlib.sha256(data).hexdigest(), archive_frame_count=count)


def dependencies():
    try:
        import av
        import numpy as np
    except ImportError as error:
        raise RuntimeError("Install dependencies: python3 -m pip install 'av>=18.1' numpy") from error
    return av, np


def encode(directory, output, fps=None, rotation=180):
    av, np = dependencies()
    entries, timestamps, duration_ns, timing = inputs(directory, fps)
    if output.exists():
        raise ValueError('output already exists: %s' % output)
    codec = av.CodecContext.create('ffv1', 'w')
    codec.width, codec.height = WIDTH, HEIGHT
    codec.pix_fmt = 'gray16le'
    codec.time_base = Fraction(1, 1000)
    codec.gop_size, codec.thread_count = 1, 2
    codec.flags |= av.codec.context.Flags.global_header
    codec.options = {'level': '3', 'coder': '1', 'slicecrc': '1'}
    codec.open()
    if not codec.extradata:
        raise RuntimeError('FFV1 encoder did not supply CodecPrivate')
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.'+output.name+'.', suffix='.tmp', dir=output.parent)
    temporary = Path(name)
    start = last_report = time.monotonic()
    print('Encoding %u frames (%s timing) to %s' % (len(entries), timing, output), flush=True)
    try:
        with os.fdopen(fd, 'wb') as dest:
            dest.write(matroska_header(codec.extradata, duration_ns, timestamps[-1]+duration_ns/1000000))
            for number, ((_, path, mtime_ns), pts) in enumerate(zip(entries, timestamps), 1):
                data = path.read_bytes()
                if len(data) != FRAME_BYTES or path.stat().st_mtime_ns != mtime_ns:
                    raise ValueError('input changed during encoding: %s' % path)
                pixels = np.frombuffer(data, dtype='<u2').reshape(HEIGHT, WIDTH)
                frame = av.VideoFrame.from_ndarray(pixels.astype(np.uint16, copy=False), format='gray16le')
                frame.pts, frame.time_base = pts, codec.time_base
                packets = codec.encode(frame)
                if len(packets) != 1 or not packets[0].is_keyframe:
                    raise RuntimeError('expected one independently coded FFV1 packet per frame')
                metadata = metadata_for_frame(path, data, pixels, number, len(entries), pts, mtime_ns, timing, rotation)
                dest.write(matroska_frame(bytes(packets[0]), metadata, pts))
                now = time.monotonic()
                if now-last_report >= 10 or number == len(entries):
                    print('  %u/%u frames, %.1f frames/s, %.1f MB written' %
                          (number, len(entries), number/(now-start), dest.tell()/1e6), flush=True)
                    last_report = now
            if codec.encode(None):
                raise RuntimeError('unexpected delayed FFV1 packets')
            dest.flush()
            os.fsync(dest.fileno())
        # Publish without replacing another process's output.
        os.link(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    raw_size, compressed = len(entries)*FRAME_BYTES, output.stat().st_size
    result = dict(frames=len(entries), raw_bytes=raw_size, matroska_bytes=compressed,
                  compression_ratio=raw_size/compressed, saving_percent=100*(1-compressed/raw_size))
    print('Encoded %u frames: %s raw bytes -> %s Matroska bytes; %.3f:1, %.2f%% smaller' %
          (len(entries), format(raw_size, ','), format(compressed, ','), result['compression_ratio'], result['saving_percent']), flush=True)
    return result


def packet_metadata(packet):
    if not hasattr(packet, 'get_sidedata'):
        raise RuntimeError('PyAV >=18.1 with packet side-data access is required')
    data = bytes(packet.get_sidedata('matroska_block_additional'))
    if not 8 < len(data) <= 65544 or int.from_bytes(data[:8], 'big') != ADDITION_TYPE:
        raise ValueError('missing or invalid APCG frame metadata')
    m = json.loads(data[8:])
    if not isinstance(m, dict) or m.get('schema') != SCHEMA:
        raise ValueError('unsupported thermal metadata schema')
    if (m.get('width'), m.get('height'), m.get('pixel_format'), m.get('bits_per_sample')) != (WIDTH, HEIGHT, 'gray16le', 16):
        raise ValueError('unsupported radiometric format')
    if type(m.get('frame_id')) is not int or m['frame_id'] < 1:
        raise ValueError('invalid frame ID')
    return m


def output_name(metadata):
    name = metadata.get('original_filename', '%012u.bin' % metadata['frame_id'])
    if (not isinstance(name, str) or not name or name in ('.', '..') or
            '/' in name or '\\' in name or '\0' in name or ':' in name or not name.endswith('.bin')):
        raise ValueError('unsafe original filename in thermal metadata')
    return name


def extract(source, output, save_metadata=False):
    av, _ = dependencies()
    if not source.is_file():
        raise ValueError('extraction input must be a Matroska file')
    if output.exists():
        raise ValueError('output directory already exists: %s' % output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix='.'+output.name+'.', dir=output.parent))
    count = checked = 0
    expected_count = None
    start = last_report = time.monotonic()
    print('Extracting %s to %s' % (source, output), flush=True)
    try:
        with av.open(str(source)) as container:
            streams = container.streams.video
            if len(streams) != 1 or streams[0].codec_context.name != 'ffv1':
                raise ValueError('expected one FFV1 video track')
            stream = streams[0]
            stream.codec_context.options = {'err_detect': 'crccheck+explode'}
            if (stream.width, stream.height) != (WIDTH, HEIGHT):
                raise ValueError('unexpected image dimensions')
            for packet in container.demux(stream):
                if not packet.size:
                    continue
                if packet.is_corrupt:
                    raise ValueError('corrupt video packet')
                m = packet_metadata(packet)
                total = m.get('archive_frame_count')
                if count == 0:
                    expected_count = total
                if total != expected_count or (total is not None and (type(total) is not int or total < 1)):
                    raise ValueError('inconsistent archive frame count')
                if total is not None and m['frame_id'] != count+1:
                    raise ValueError('missing, duplicate or out-of-order archive frame')
                frames = packet.decode()
                if len(frames) != 1 or frames[0].format.name != 'gray16le' or (frames[0].width, frames[0].height) != (WIDTH, HEIGHT):
                    raise ValueError('decoder did not return one native gray16le frame')
                pixels = frames[0].to_ndarray()
                data = pixels.astype('<u2', copy=False).tobytes()
                digest = m.get('source_sha256')
                if digest is not None:
                    if digest != hashlib.sha256(data).hexdigest():
                        raise ValueError('raw pixel checksum mismatch at frame %u' % m['frame_id'])
                    checked += 1
                if m.get('minimum_raw') != int(pixels.min()) or m.get('maximum_raw') != int(pixels.max()):
                    raise ValueError('decoded pixels do not match frame range')
                dest = temporary/output_name(m)
                with dest.open('xb') as f:
                    f.write(data)
                mtime = m.get('source_mtime_ns')
                if mtime is not None:
                    if type(mtime) is not int:
                        raise ValueError('invalid original file timestamp')
                    os.utime(dest, ns=(mtime, mtime))
                if save_metadata:
                    with dest.with_suffix('.json').open('x') as f:
                        json.dump(m, f, indent=2, allow_nan=False)
                        f.write('\n')
                count += 1
                now = time.monotonic()
                if now-last_report >= 10:
                    print('  %u frames extracted, %.1f frames/s' % (count, count/(now-start)), flush=True)
                    last_report = now
        if not count or (expected_count is not None and count != expected_count):
            raise ValueError('incomplete archive: extracted %u of %s frames' % (count, expected_count))
        if output.exists():
            raise ValueError('output directory appeared during extraction')
        temporary.rename(output)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    print('Extracted %u frames (%s raw bytes); %u SHA-256 checks passed' %
          (count, format(count*FRAME_BYTES, ','), checked), flush=True)
    return dict(frames=count, raw_bytes=count*FRAME_BYTES, checksums_verified=checked)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--extract', action='store_true', help='extract raw .bin files instead of encoding')
    parser.add_argument('--metadata', action='store_true', help='also extract per-frame .json sidecars')
    parser.add_argument('--fps', type=float, help='override capture timing with a fixed playback frame rate')
    parser.add_argument('--rotation', type=int, choices=(0, 180), default=180,
                        help='display rotation metadata; raw pixels are never rotated (default: 180)')
    args = parser.parse_args()
    if args.fps is not None and (not math.isfinite(args.fps) or not 0 < args.fps <= 1000):
        parser.error('--fps must be finite and between 0 and 1000')
    if args.extract and args.fps is not None:
        parser.error('--fps is only for encoding')
    if not args.extract and args.metadata:
        parser.error('--metadata is only for extraction')
    try:
        if args.extract:
            extract(args.input, args.output, args.metadata)
        else:
            encode(args.input, args.output, args.fps, args.rotation)
    except (OSError, ValueError, RuntimeError, EOFError) as error:
        parser.exit(1, 'thermal_to_video: %s\n' % error)


if __name__ == '__main__':
    main()
