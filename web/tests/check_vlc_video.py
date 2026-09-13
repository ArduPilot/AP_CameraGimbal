#!/usr/bin/env python3
"""Check a video's duration and forward/backward seeking using installed libVLC."""
import argparse
import ctypes
import ctypes.util
import json
from pathlib import Path
import subprocess
import time


def check_vlc_video(path, expected_seconds=None):
    if expected_seconds is None:
        probe = subprocess.check_output([
            'ffprobe', '-v', 'error', '-show_entries', 'format=duration',
            '-of', 'json', str(path)])
        expected_seconds = float(json.loads(probe)['format']['duration'])
    library = ctypes.util.find_library('vlc')
    if not library:
        raise RuntimeError('libVLC is required for --vlc checks')
    vlc = ctypes.CDLL(library)

    def bind(name, result, *args):
        function = getattr(vlc, 'libvlc_' + name)
        function.restype = result
        function.argtypes = list(args)
        return function

    ptr = ctypes.c_void_p
    new = bind('new', ptr, ctypes.c_int, ctypes.POINTER(ctypes.c_char_p))
    media_new = bind('media_new_path', ptr, ptr, ctypes.c_char_p)
    player_new = bind('media_player_new_from_media', ptr, ptr)
    play = bind('media_player_play', ctypes.c_int, ptr)
    length = bind('media_player_get_length', ctypes.c_int64, ptr)
    get_time = bind('media_player_get_time', ctypes.c_int64, ptr)
    set_time = bind('media_player_set_time', None, ptr, ctypes.c_int64)
    stop = bind('media_player_stop', None, ptr)
    player_release = bind('media_player_release', None, ptr)
    media_release = bind('media_release', None, ptr)
    release = bind('release', None, ptr)

    args = [b'--intf=dummy', b'--vout=dummy', b'--aout=dummy', b'--no-audio',
            b'--no-video-title-show', b'--codec=avcodec', b'--avcodec-hw=none', b'--quiet']
    instance = new(len(args), (ctypes.c_char_p * len(args))(*args))
    assert instance, 'libVLC initialization failed'
    media = player = None
    expected_ms = round(expected_seconds * 1000)

    def wait_for(check, description):
        deadline = time.monotonic() + 10
        while not check():
            assert time.monotonic() < deadline, (
                str(path), description, 'length_ms', length(player), 'time_ms', get_time(player))
            time.sleep(.02)

    try:
        media = media_new(instance, str(Path(path).resolve()).encode())
        assert media
        player = player_new(media)
        assert player and play(player) == 0
        wait_for(lambda: abs(length(player) - expected_ms) <= 2,
                 f'expected duration {expected_ms} ms')
        # Verify a late seek followed by a backward seek, beyond checking that
        # the timeline label alone is correct. Keep short test clips playing.
        for fraction in (.75, .25):
            target = round(expected_ms * fraction)
            set_time(player, target)
            wait_for(lambda: target - 50 <= get_time(player) <= target + 200,
                     f'seek to {target} ms')
        print(f'PASS VLC duration {expected_seconds:.3f}s and forward/backward seek: {path}', flush=True)
    finally:
        if player:
            stop(player)
            player_release(player)
        if media:
            media_release(media)
        release(instance)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('videos', type=Path, nargs='+')
    args = parser.parse_args()
    for video in args.videos:
        check_vlc_video(video)
