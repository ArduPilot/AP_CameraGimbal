#!/usr/bin/env python3
"""Check rendered pixels, including PARAM_EXT -> camera app -> live RTSP video."""
import argparse
import math
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

import av
import cv2
import numpy as np

from image_controls import ImageControls, PALETTES

ROOT = Path(__file__).resolve().parents[1]


def chart():
    image = np.zeros((180, 320, 3), np.uint8)
    image[:] = (100, 60, 40)
    image[::4] = (40, 90, 120)
    image[60:120, 120:200] = (25, 30, 20)
    return image


def unit():
    controls = ImageControls()
    source = chart()
    np.testing.assert_array_equal(controls.apply(source, {}), source)
    assert controls.apply(source, {'brightness': 80}).mean() > source.mean() + 60
    assert controls.apply(source, {'brightness': 0}).max() == 0
    gray = controls.apply(source, {'saturation': 0})
    assert np.max(np.ptp(gray.astype(int), axis=2)) == 0
    assert controls.apply(source, {'contrast': 100}).std() > source.std()
    assert controls.apply(source, {'exposure': 3}).mean() > source.mean() * 1.8
    assert controls.apply(source, {'iso': 2}).mean() > source.mean() * 1.8
    assert controls.apply(source, {'shutter': 8}).mean() < source.mean() / 10
    assert controls.apply(source, {'metering': 2}).mean() > source.mean() * 1.5
    warm = controls.apply(source, {'white_balance': 2}).mean(axis=(0, 1))
    assert warm[0] > source[:, :, 0].mean() and warm[2] < source[:, :, 2].mean()
    sharp = cv2.Laplacian(source, cv2.CV_32F).var()
    assert cv2.Laplacian(controls.apply(source, {'defocus': 1}), cv2.CV_32F).var() < sharp * .3
    white = controls.apply(source, {}, True)
    black = controls.apply(source, {'thermal_palette': 10}, True)
    np.testing.assert_allclose(white.astype(int) + black, 255, atol=1)
    assert controls.apply(source, {'thermal_gain': 0}, True).std() < white.std() * .6
    assert len({controls.apply(source, {'thermal_palette': p}, True).tobytes() for p in PALETTES}) == len(PALETTES)
    print('PASS image control directions, clipping, focus, thermal gain and all palettes', flush=True)


class Viewer:
    def __init__(self, port, path):
        from test_video_telemetry import RTSP
        self.client = RTSP(port, path)
        self.image = None
        self.error = None
        self.stopped = False
        self.updated = 0
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        try:
            decoder = av.CodecContext.create('h264', 'r')
            for nals, record in self.client.frames(100000, 'h264'):
                if self.stopped:
                    break
                for frame in decoder.decode(av.Packet(b''.join(b'\0\0\0\1' + n for n in nals))):
                    self.image = frame.to_ndarray(format='rgb24')
                    self.updated = time.monotonic()
        except Exception as error:
            if not self.stopped:
                self.error = error

    def frame(self):
        # Discard the bounded producer queue and receive genuinely new pixels.
        deadline, after = time.monotonic() + 10, time.monotonic() + .4
        while time.monotonic() < deadline:
            if self.error:
                raise self.error
            if self.updated >= after:
                return self.image.copy()
            time.sleep(.01)
        raise TimeoutError('No fresh decoded video')

    def close(self):
        self.stopped = True
        self.client.sock.shutdown(socket.SHUT_RDWR)
        self.thread.join(timeout=5)
        assert not self.thread.is_alive()
        self.client.close()


def integration(backend):
    from test_camera_definition import CameraDefinition, download, write, connect, port, stop, wait_ready, M, receive
    build = ROOT / 'build' / ('sitl' if backend == 'mt11' else backend + '-sitl')
    with tempfile.TemporaryDirectory(prefix='sitl-image-controls-') as tmp:
        root = Path(tmp)
        cv2.imwrite(str(root / 'chart.png'), cv2.cvtColor(chart(), cv2.COLOR_RGB2BGR))
        subprocess.run(['ffmpeg', '-v', 'error', '-loop', '1', '-i', str(root / 'chart.png'),
                        '-t', '0.2', '-r', '20', '-c:v', 'libx264', '-pix_fmt', 'yuv420p',
                        str(root / 'source.mp4')], check=True)
        config = root / 'camera.ini'
        config.write_text('[logging]\ndisarmed=true\n[mavlink]\nsystem_id=42\n[stream.main]\nresolution=' +
                          ('1920x1080\n' if backend == 'z1mini' else '1280x720\n'))
        gp, tcp, rtsp = port(), port(), port()
        env = dict(os.environ, CAMERA_APP_CONFIG=str(config), CAMERA_APP_BACKEND=backend,
                   CAMERA_APP_UART=f'udp://127.0.0.1:{gp}', CAMERA_APP_PORT=str(port()),
                   CAMERA_APP_RTSP_PORT=str(rtsp), CAMERA_APP_MAVLINK_TCP_PORT=str(tcp),
                   CAMERA_APP_MAVLINK_UDP_PORT='0', CAMERA_APP_READY_PATH=str(root / 'camera.ready'),
                   CAMERA_APP_SITL_VIDEO1=str(root / 'source.mp4'), CAMERA_APP_SITL_VIDEO2=str(root / 'source.mp4'),
                   CAMERA_GIMBAL_SITL_PYTHON=sys.executable, CAMERA_GIMBAL_SITL_FPS='20',
                   CAMERA_APP_LOG_ROOT=str(root / 'logs'), CAMERA_APP_RECORD_ROOT=str(root / 'record'), CAMERA_APP_CAPTURE_ROOT=str(root / 'capture'))
        env.pop('CAMERA_APP_SITL_TERRAIN', None)
        camera = gimbal = link = viewer = thermal = None
        with (root / 'camera.log').open('w') as log:
            try:
                gimbal = subprocess.Popen([sys.executable, str(ROOT / 'sitl/gimbal_sim.py'), '--backend', backend,
                    '--port', str(gp), '--ready-file', str(root / 'gimbal.ready')], stdout=log, stderr=log)
                wait_ready(root / 'gimbal.ready', gimbal)
                camera = subprocess.Popen([str(build / 'camera-app'), '--backend', backend], env=env, stdout=log, stderr=log)
                wait_ready(root / 'camera.ready', camera)
                link = connect(f'tcp:127.0.0.1:{tcp}')
                definition = CameraDefinition(download(link))
                viewer = Viewer(rtsp, 'video1')
                baseline = viewer.frame()
                # Single-lens targets without image sliders otherwise finish
                # before a useful 5 Hz diagnostic sample window is collected.
                sample_until = time.monotonic() + 2
                while time.monotonic() < sample_until:
                    viewer.frame()
                if 'IMG_BRIGHTNESS' in definition.parameters:
                    for name, value, check in (
                        ('IMG_BRIGHTNESS', 80, lambda im: im.mean() > baseline.mean() + 60),
                        ('IMG_SATURATION', 0, lambda im: np.abs(im[:, :, 0].astype(float) - im[:, :, 2]).mean() < 2),
                        ('IMG_CONTRAST', 0, lambda im: im.std() < 2),
                        ('IMG_EXPOSURE', 3, lambda im: im.mean() > baseline.mean() * 1.7),
                        ('IMG_ISO', 2, lambda im: im.mean() > baseline.mean() * 1.7),
                        ('IMG_SHUTTER', 8, lambda im: im.mean() < baseline.mean() * .15),
                        ('IMG_METERING', 2, lambda im: im.mean() > baseline.mean() * 1.4),
                        ('IMG_WHITE_BAL', 2, lambda im: im[:, :, 0].mean() > baseline[:, :, 0].mean() * 1.08),
                    ):
                        write(link, definition, name, value)
                        changed = viewer.frame()
                        assert check(changed), (name, baseline.mean(), changed.mean(), changed.std())
                        write(link, definition, name, 50 if name in ('IMG_BRIGHTNESS', 'IMG_SATURATION', 'IMG_CONTRAST') else 0)
                        restored = viewer.frame()
                        assert np.abs(restored.astype(float) - baseline).mean() < 3, name
                        print(f'PASS {backend} PARAM_EXT {name} changes live pixels and restores them', flush=True)
                if 'CAM_ZOOM' in definition.parameters:
                    write(link, definition, 'CAM_ZOOM', 50.0)
                    assert np.abs(viewer.frame().astype(float) - baseline).mean() > 10
                    write(link, definition, 'CAM_ZOOM', 0.0)
                if 'CAM_PALETTE' in definition.parameters:
                    thermal = Viewer(rtsp, 'video2')
                    white = thermal.frame()
                    write(link, definition, 'CAM_PALETTE', 10)
                    black = thermal.frame()
                    assert np.abs(white.astype(float) + black - 255).mean() < 4
                    write(link, definition, 'CAM_PALETTE', 3)
                    iron = thermal.frame()
                    assert np.abs(iron[:, :, 0].astype(float) - iron[:, :, 2]).mean() > 15
                    print('PASS live thermal white/black-hot and ironbow palettes', flush=True)
                    write(link, definition, 'CAM_PALETTE', 0)
                    write(link, definition, 'CAM_THERM_GAIN', 0)
                    assert thermal.frame().std() < white.std() * .65
                    write(link, definition, 'CAM_THERM_GAIN', 1)
                    write(link, definition, 'CAM_SOURCE', 1)
                    swapped = viewer.frame()
                    assert np.abs(swapped[:, :, 0].astype(float) - swapped[:, :, 2]).mean() < 2
                    write(link, definition, 'CAM_SOURCE', 0)
                    write(link, definition, 'CAM_LENS', 0)
                if 'CAM_AUTOFOCUS' in definition.parameters:
                    link.mav.command_long_send(42, 100, M.MAV_CMD_SET_CAMERA_FOCUS, 0, 2, 100, 0, 0, 0, 0, 0)
                    ack = receive(link, 'COMMAND_ACK', lambda m: m.command == M.MAV_CMD_SET_CAMERA_FOCUS)
                    assert ack.result == M.MAV_RESULT_ACCEPTED, ack
                    blurred = viewer.frame()
                    write(link, definition, 'CAM_AUTOFOCUS', 1)
                    focused = viewer.frame()
                    assert cv2.Laplacian(focused, cv2.CV_32F).var() > cv2.Laplacian(blurred, cv2.CV_32F).var() * 2
                    print('PASS manual defocus and camera.xml autofocus restore sharp video', flush=True)
                if backend != 'z1mini':
                    if 'IMG_BRIGHTNESS' in definition.parameters:
                        write(link, definition, 'IMG_BRIGHTNESS', 80)
                    for scope in ((0, 1) if backend == 'mt11' else (1,)):
                        if 'PHOTO_SCOPE' in definition.parameters:
                            write(link, definition, 'PHOTO_SCOPE', scope)
                        before = set((root / 'capture').glob('*.jpg'))
                        link.mav.command_long_send(42, 100, M.MAV_CMD_IMAGE_START_CAPTURE, 0, 0, 0, 1, 0, 0, 0, 0)
                        ack = receive(link, 'COMMAND_ACK', lambda m: m.command == M.MAV_CMD_IMAGE_START_CAPTURE)
                        assert ack.result == M.MAV_RESULT_ACCEPTED, ack
                        new = set((root / 'capture').glob('*.jpg')) - before
                        assert len(new) == (3 if backend == 'mt11' and scope == 1 else 1), new
                        for path in new:
                            photo = cv2.imread(str(path))
                            assert photo is not None
                            if '_C.jpg' in path.name and 'IMG_BRIGHTNESS' in definition.parameters:
                                assert photo.mean() > baseline.mean() + 60
                    print('PASS still capture scope and processed JPEG pixels', flush=True)
                if 'IMG_BRIGHTNESS' in definition.parameters:
                    write(link, definition, 'REC_AUTOSTART', 1)
                    viewer.frame()
                    # Issue #8: still capture must not stop or reject an active
                    # video recording, including a timed photogrammetry series.
                    before = set((root / 'capture').glob('*.jpg'))
                    link.mav.command_long_send(42, 100, M.MAV_CMD_IMAGE_START_CAPTURE,
                                               0, 0, .2, 3, 100, 0, 0, 0)
                    ack = receive(link, 'COMMAND_ACK', lambda m: m.command == M.MAV_CMD_IMAGE_START_CAPTURE)
                    assert ack.result == M.MAV_RESULT_ACCEPTED, ack
                    expected = 9 if backend == 'mt11' else 3
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        new = set((root / 'capture').glob('*.jpg')) - before
                        if len(new) == expected and all(cv2.imread(str(p)) is not None for p in new):
                            break
                        time.sleep(.05)
                    else:
                        raise AssertionError(f'{backend}: expected {expected} readable JPEGs, got {new}')
                    link.mav.command_long_send(42, 100, M.MAV_CMD_REQUEST_CAMERA_CAPTURE_STATUS,
                                               0, 1, 0, 0, 0, 0, 0, 0)
                    status = receive(link, 'CAMERA_CAPTURE_STATUS')
                    assert status.video_status == 1, status
                    print('PASS interval photos while video remains recording (issue #8)', flush=True)
                    write(link, definition, 'IMG_BRIGHTNESS', 20)
                    viewer.frame()
                    write(link, definition, 'REC_AUTOSTART', 0)
                    recording = next((root / 'record').glob('SITL_0_*.mp4'))
                    with av.open(str(recording)) as video:
                        levels = [frame.to_ndarray(format='rgb24').mean() for frame in video.decode(video=0)]
                    assert len(levels) > 5 and max(levels) - min(levels) > 70, levels
                    print('PASS recording includes live brightness changes without interrupting capture', flush=True)
                if backend == 'mt11':
                    viewer.close()
                    viewer = None
                    thermal.close()
                    thermal = None
                    write(link, definition, 'VIDEO_MAIN_CODEC', 1)
                    write(link, definition, 'VIDEO_SUB_CODEC', 1)
                    for source in (0, 1, 0):
                        write(link, definition, 'CAM_SOURCE', source)
                        time.sleep(.4)
                        for index, codec in enumerate(('h264', 'hevc') if source else ('hevc', 'h264')):
                            with av.open(f'rtsp://127.0.0.1:{rtsp}/video{index+1}',
                                         options={'rtsp_transport': 'tcp'}, timeout=10) as stream:
                                assert stream.streams.video[0].codec_context.name == codec
                                assert next(stream.decode(video=0)).width == 1280
                    print('PASS H.265 RGB/H.264 thermal swaps advertise and deliver matching codecs', flush=True)
                print(f'PASS {backend} simulated video controls', flush=True)
                assert camera.poll() is None
            except Exception:
                print((root / 'camera.log').read_text())
                raise
            finally:
                if viewer:
                    viewer.close()
                if thermal:
                    thermal.close()
                if link:
                    link.close()
                stop(camera)
                stop(gimbal)
            from pymavlink import mavutil
            logs = list((root / 'logs').glob('*.BIN'))
            assert logs, 'No BIN log created'
            reader = mavutil.mavlink_connection(str(logs[0]))
            samples = []
            while True:
                message = reader.recv_match(type='AE')
                if message is None:
                    break
                samples.append(message)
            reader.close()
            good = [s for s in samples if s.Valid & 0x1f == 0x1f]
            assert len(good) >= 5, f'Insufficient exposure feedback: {len(good)} valid / {len(samples)} total'
            assert all(s.Src == 1 for s in samples), 'Simulation must identify its source'
            for sample in samples:
                for bit, field in ((1, 'US'), (2, 'AG'), (4, 'DG'), (8, 'IG'),
                                   (16, 'Y'), (32, 'Targ'), (64, 'Err')):
                    if not sample.Valid & bit:
                        assert math.isnan(getattr(sample, field)), (field, sample)
            assert all(s.Result == 0 and s.US > 0 and 0 <= s.Y <= 255 for s in good)
            assert all(not s.Valid & (256 | 512) for s in good), 'No invented convergence/limit flags'
            if backend == 'mt11':
                assert any(s.Lens == 2 and s.Valid == 0 and s.Result != 0 for s in samples)
            if backend in ('mt11', 'a8'):
                assert min(s.US for s in good) < 600, 'Manual shutter not reflected in log'
                assert max(s.AG for s in good) >= 2, 'Manual ISO not reflected in log'
            times = sorted({s.TimeUS for s in good})
            rate = (len(times) - 1) * 1e6 / (times[-1] - times[0])
            assert 3 <= rate <= 6, rate
            print(f'PASS {backend} BIN AE decoding, validity, units and {rate:.1f} Hz sampling', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=('mt11', 'a8', 'zr10', 'z1mini'))
    args = parser.parse_args()
    unit()
    if args.backend:
        integration(args.backend)
