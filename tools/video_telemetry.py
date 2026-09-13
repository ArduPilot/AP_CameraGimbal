#!/usr/bin/env python3
"""Extract AP_CameraGimbal per-frame SEI telemetry as JSON lines.

Accepts Annex-B .h264/.h265/.hevc files directly, or an MP4/RTSP input through
ffprobe/ffmpeg (stream copy, no decoding). Use --stream to select a video track.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

UUID = bytes.fromhex("8d646b4e556f4a908b7c35e629510321")
START = re.compile(b"\x00\x00(?:\x00)?\x01")


def nals(source):
    """Incremental Annex-B parser, retaining at most one NAL plus one chunk."""
    pending = b""
    while True:
        chunk = source.read(65536)
        pending += chunk
        starts = list(START.finditer(pending))
        for first, second in zip(starts, starts[1:]):
            yield pending[first.end():second.start()].rstrip(b"\0")
        if starts:
            pending = pending[starts[-1].start():]
        elif len(pending) > 4:
            pending = pending[-4:]
        if not chunk:
            first = START.match(pending)
            if first:
                yield pending[first.end():].rstrip(b"\0")
            break


def telemetry(nal, codec="h264"):
    if not nal:
        return
    header = 2 if codec in ("h265", "hevc") else 1
    kind = (nal[0] >> 1) & 63 if header == 2 else nal[0] & 31
    if kind not in ((39, 40) if header == 2 else (6,)):
        return
    rbsp = bytearray()
    zeros = 0
    for byte in nal[header:]:
        if zeros == 2 and byte == 3:
            zeros = 0
            continue
        rbsp.append(byte)
        zeros = zeros + 1 if byte == 0 else 0
    offset = 0
    while offset < len(rbsp) and rbsp[offset:] != b"\x80":
        fields = []
        for _ in range(2):
            value = 0
            while offset < len(rbsp) and rbsp[offset] == 255:
                value += 255
                offset += 1
            if offset == len(rbsp):
                return
            value += rbsp[offset]
            offset += 1
            fields.append(value)
        kind, size = fields
        if offset + size > len(rbsp):
            return
        payload = rbsp[offset:offset + size]
        offset += size
        if kind == 5 and payload[:16] == UUID:
            yield json.loads(payload[16:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input")
    parser.add_argument("--stream", type=int, default=0, help="video stream index (default 0)")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    extension = pathlib.Path(args.input).suffix.lower()
    process = None
    if extension in (".h264", ".264", ".h265", ".hevc"):
        codec = "h264" if extension in (".h264", ".264") else "hevc"
        source = open(args.input, "rb")
    else:
        options = ["-rtsp_transport", "tcp"] if args.input.startswith("rtsp://") else []
        probe = subprocess.check_output(["ffprobe", "-v", "error", *options,
            "-select_streams", f"v:{args.stream}", "-show_entries", "stream=codec_name",
            "-of", "json", args.input])
        codec = json.loads(probe)["streams"][0]["codec_name"]
        if codec not in ("h264", "hevc"):
            parser.error(f"unsupported video codec: {codec}")
        process = subprocess.Popen(["ffmpeg", "-v", "error", *options, "-i", args.input,
            "-map", f"0:v:{args.stream}", "-c:v", "copy", "-bsf:v", f"{codec}_mp4toannexb",
            "-f", codec, "pipe:1"], stdout=subprocess.PIPE)
        source = process.stdout
    output = args.output.open("w") if args.output else sys.stdout
    try:
        for nal in nals(source):
            for record in telemetry(nal, codec):
                print(json.dumps(record, separators=(",", ":")), file=output, flush=True)
        if process is not None and process.wait() != 0:
            raise SystemExit("ffmpeg could not read the video stream")
    finally:
        source.close()
        if args.output:
            output.close()
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait()


if __name__ == "__main__":
    main()
