#!/usr/bin/env python3
"""Keep RTSP connected when video queues overflow during a delayed handshake."""
import ctypes
from pathlib import Path
import socket
import subprocess
import tempfile
import threading

REPO = Path(__file__).resolve().parents[1]

# Compile a small C shim so this test does not duplicate ca_support_config's ABI.
SHIM = r'''
#include "camera_app/support_video.h"
#include <string.h>
struct ca_support_video *test_open(unsigned port) {
    struct ca_support_config config = {.enabled = true, .video1_port = port};
    strcpy(config.host, "127.0.0.1");
    strcpy(config.video1_name, "queue-test");
    struct ca_support_video *publisher = NULL;
    if (ca_support_video_open(&publisher, &config, 0, CA_VIDEO_H264) < 0) return NULL;
    return publisher;
}
'''


def read_exact(connection, length):
    data = b''
    while len(data) < length:
        chunk = connection.recv(length - len(data))
        assert chunk, 'publisher disconnected after queue overflow'
        data += chunk
    return data


def check(library, byte_limit):
    waiting, release = threading.Event(), threading.Event()
    errors, timestamps = [], []
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(5)

        def proxy():
            try:
                with listener.accept()[0] as connection:
                    connection.settimeout(5)
                    for method in ('OPTIONS', 'ANNOUNCE', 'SETUP', 'RECORD'):
                        header = b''
                        while not header.endswith(b'\r\n\r\n'):
                            header += read_exact(connection, 1)
                        lines = header.decode().split('\r\n')
                        assert lines[0].startswith(method + ' '), lines[0]
                        values = dict(line.split(': ', 1) for line in lines[1:] if ': ' in line)
                        read_exact(connection, int(values['Content-Length']))
                        if method == 'RECORD':
                            waiting.set()
                            assert release.wait(5)
                        connection.sendall(('RTSP/1.0 200 OK\r\nCSeq: ' + values['CSeq'] +
                                            '\r\nSession: test\r\nContent-Length: 0\r\n\r\n').encode())
                    for expected in (0, 100, 101):
                        header = read_exact(connection, 4)
                        assert header[:2] == b'$\x00', header
                        packet = read_exact(connection, int.from_bytes(header[2:], 'big'))
                        timestamp = int.from_bytes(packet[4:8], 'big')
                        timestamps.append(timestamp)
                        assert timestamp == expected * 3000, timestamps
                        assert packet[12] & 31 == (5 if expected != 101 else 1)
            except BaseException as error:
                errors.append(error)
                waiting.set()

        worker = threading.Thread(target=proxy)
        worker.start()
        publisher = library.test_open(listener.getsockname()[1])
        assert publisher

        def push(sequence, key=False, size=6):
            data = b'\x00\x00\x00\x01' + bytes([0x65 if key else 0x41]) + b'\x01' * (size - 5)
            library.ca_support_video_push(publisher, data, len(data), sequence * 3000, key)

        try:
            push(0, key=True)
            assert waiting.wait(5), 'publisher never reached RECORD'
            assert not errors, errors
            if byte_limit:
                for i in range(1, 4):
                    push(i, size=1024 * 1024)
            else:
                for i in range(1, 65):
                    push(i)
            # An entire GOP was discarded. Only the next keyframe and its
            # following inter-frame may follow the original in-flight frame.
            push(100, key=True)
            push(101)
            release.set()
            worker.join(7)
            assert not worker.is_alive(), 'receiver stalled'
            assert not errors, errors
            assert timestamps == [0, 300000, 303000], timestamps
        finally:
            release.set()
            library.ca_support_video_close(publisher)
            worker.join(7)
    print('PASS RTSP connection and keyframe recovery after ' + ('byte' if byte_limit else 'frame') + ' queue limit')


def main():
    with tempfile.TemporaryDirectory(prefix='support-video-queue-') as temp:
        root = Path(temp)
        shim = root / 'shim.c'
        shim.write_text(SHIM)
        target = root / 'publisher.so'
        sources = ['streaming/support_video.c', 'recording/video_metadata.c', 'recording/metadata.c', 'log.c']
        subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(REPO / 'camera_app/include'), str(shim),
                        *(str(REPO / 'camera_app/src' / source) for source in sources),
                        '-lpthread', '-lm', '-o', str(target)], check=True)
        library = ctypes.CDLL(str(target))
        library.test_open.argtypes = [ctypes.c_uint]
        library.test_open.restype = ctypes.c_void_p
        library.ca_support_video_push.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_bool]
        library.ca_support_video_close.argtypes = [ctypes.c_void_p]
        check(library, False)
        check(library, True)


if __name__ == '__main__':
    main()
