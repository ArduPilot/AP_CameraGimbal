#!/usr/bin/env python3
"""Opt-in Firefox test against a running SITL Live page (temporary profile)."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


class Marionette:
    def __init__(self, connection):
        self.connection = connection
        self.reader = connection.makefile('rb')
        self.sequence = 0
        self.receive()

    def receive(self):
        length = bytearray()
        while True:
            char = self.reader.read(1)
            if not char:
                raise EOFError('Firefox closed its automation connection')
            if char == b':':
                break
            length.extend(char)
        return json.loads(self.reader.read(int(length)))

    def command(self, name, arguments=None):
        self.sequence += 1
        message = json.dumps([0, self.sequence, name, arguments or {}]).encode()
        self.connection.sendall(str(len(message)).encode() + b':' + message)
        reply = self.receive()
        if reply[2]:
            raise RuntimeError(reply[2])
        return reply[3]

    def js(self, script, *arguments):
        return self.command('WebDriver:ExecuteScript', {
            'script': script, 'args': list(arguments),
            'newSandbox': False, 'sandbox': 'default',
        })['value']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:8081/live')
    parser.add_argument('--headful', action='store_true',
                        help='show a window; keep it unobscured during measurement')
    parser.add_argument('--min-fps', type=float, default=15,
                        help='minimum presented rate for the 20 Hz terrain source')
    args = parser.parse_args()
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='camera-live-firefox-') as directory:
        root = Path(directory)
        (root / 'user.js').write_text(
            f'user_pref("marionette.port", {port});\n'
            'user_pref("media.autoplay.default", 0);\n'
            'user_pref("browser.shell.checkDefaultBrowser", false);\n')
        with (root / 'firefox.log').open('w') as log:
            process = subprocess.Popen(
                ['firefox', '--no-remote', '--profile', directory, '--marionette'] +
                ([] if args.headful else ['--headless']), stdout=log, stderr=log)
            connection = None
            try:
                deadline = time.monotonic() + 20
                while connection is None:
                    try:
                        connection = socket.create_connection(('127.0.0.1', port), timeout=10)
                    except OSError:
                        assert process.poll() is None and time.monotonic() < deadline
                        time.sleep(.1)
                browser = Marionette(connection)
                browser.command('WebDriver:NewSession', {'capabilities': {
                    'alwaysMatch': {'pageLoadStrategy': 'eager'}}})
                browser.command('WebDriver:Navigate', {'url': args.url})
                if browser.js('return !!document.querySelector("#password")'):
                    browser.js('document.querySelector("#password").value=arguments[0];'
                               'document.querySelector("form").submit();',
                               os.environ.get('CAMERA_WEB_PASSWORD', 'ardupilot'))
                    time.sleep(1)
                    browser.command('WebDriver:Navigate', {'url': args.url})
                browser.js('''
                    window.measure = {frames: [], seeks: 0};
                    const video = document.querySelector('video');
                    if (!video) throw Error('Live page has no video');
                    video.addEventListener('seeking', () => measure.seeks++);
                    let callback;
                    const frame = (now, data) => {
                        measure.frames.push([now, data.mediaTime]);
                        callback = video.requestVideoFrameCallback(frame);
                    };
                    const arm = () => {
                        if (callback) video.cancelVideoFrameCallback(callback);
                        callback = video.requestVideoFrameCallback(frame);
                    };
                    video.addEventListener('loadeddata', arm);
                    arm();
                ''')
                deadline = time.monotonic() + 15
                while browser.js('return measure.frames.length') < 20:
                    assert time.monotonic() < deadline, 'Video did not start'
                    time.sleep(.2)
                paused_at = browser.js('const v=document.querySelector("video");'
                                       'v.pause();return v.currentTime;')
                time.sleep(5)
                paused = browser.js('const v=document.querySelector("video");'
                                    'return {paused:v.paused,time:v.currentTime,'
                                    'lag:v.buffered.end(v.buffered.length-1)-v.currentTime};')
                assert paused['paused'] and abs(paused['time'] - paused_at) < .1, paused
                assert paused['lag'] > 3, paused
                browser.js('document.querySelector("video").play();')
                deadline = time.monotonic() + 5
                while True:
                    lag = browser.js('const v=document.querySelector("video");'
                                     'if (v.readyState < 2 || !v.buffered.length) return 99;'
                                     'return v.buffered.end(v.buffered.length-1)-v.currentTime;')
                    if lag < 1:
                        break
                    assert time.monotonic() < deadline, f'Playback remains {lag:.2f}s behind'
                    time.sleep(.1)
                browser.js('measure.frames=[];measure.seeks=0;')
                time.sleep(8)
                result = browser.js('return measure;')
                frames = result['frames']
                fps = (len(frames) - 1) * 1000 / (frames[-1][0] - frames[0][0]) if len(frames) > 1 else 0
                assert fps >= args.min_fps, f'Only {fps:.1f} displayed fps after catch-up'
                assert result['seeks'] < 4, f'Repeated catch-up interrupts playback: {result["seeks"]}'
                print(f'PASS pause held for 5s; resumed within {lag:.2f}s of live; '
                      f'{fps:.1f} displayed fps; {result["seeks"]} further seeks')
            except Exception:
                print((root / 'firefox.log').read_text())
                raise
            finally:
                if connection:
                    connection.close()
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    main()
