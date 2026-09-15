#!/usr/bin/env python3
"""Exercise web live saves, ROI rate tracking and stale-data stops on isolated SITL.

Requires the portable SITL web build and the usual MAVLink/video test dependencies.
"""
import argparse
import base64
import math
import os
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

from html.parser import HTMLParser
from pymavlink.quaternion import Quaternion
from test_gimbal_angle_hold import MCU
from test_mavlink_parameters import connect, port, stop, wait_ready, receive, M
from test_camera_definition import read

ROOT = Path(__file__).resolve().parents[1]


class ParameterPage(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.values = {}
        self.in_form = False
        self.select = None
        self.notice = ''
        self.in_notice = False
        self.have_notice = False
        self.feed(text)

    def handle_starttag(self, tag, attributes):
        attrs = dict(attributes)
        if tag == 'form':
            self.in_form = attrs.get('action') == '/parameters'
        if self.in_form:
            if tag == 'input' and 'name' in attrs:
                self.values[attrs['name']] = attrs.get('value', '')
            elif tag == 'select':
                self.select = attrs.get('name')
            elif tag == 'option' and self.select:
                if self.select not in self.values or 'selected' in attrs:
                    self.values[self.select] = attrs.get('value', '')
        if tag == 'p' and 'notice' in attrs.get('class', '').split() and not self.have_notice:
            self.in_notice = self.have_notice = True

    def handle_endtag(self, tag):
        if tag == 'form':
            self.in_form = False
        if tag == 'select':
            self.select = None
        if tag == 'p':
            self.in_notice = False

    def handle_data(self, data):
        if self.in_notice:
            self.notice += data


def wait_for(predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, 'condition timed out'
        time.sleep(.05)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/sitl')
    args = parser.parse_args()
    build = args.build.resolve()
    with tempfile.TemporaryDirectory(prefix='live-tracking-') as directory:
        root = Path(directory)
        for name in ('app', 'run', 'mnt', 'record', 'capture'):
            (root / name).mkdir()
        config = root / 'app/camera.ini'
        config.write_text('[mavlink]\nsystem_id=42\n[stream.sub]\nresolution=1280x720\n')
        (root / 'app/web.pass').write_text('test-password\n')
        mcu = MCU('mt11', 1)
        with mcu.lock:
            mcu.gimbal.yaw, mcu.gimbal.pitch = 30, -45
        ready = root / 'run/camera-app.ready'
        tcp_port, web_port = port(), port()
        env = dict(os.environ, CAMERA_APP_CONFIG=str(config), CAMERA_APP_BACKEND='mt11',
                   CAMERA_APP_UART=f'udp://127.0.0.1:{mcu.socket.getsockname()[1]}',
                   CAMERA_APP_PORT=str(port()), CAMERA_APP_MAVLINK_TCP_PORT=str(tcp_port),
                   CAMERA_APP_MAVLINK_UDP_PORT='0', CAMERA_APP_READY_PATH=str(ready),
                   CAMERA_APP_RTSP_PORT=str(port()), CAMERA_APP_RECORD_ROOT=str(root / 'record'),
                   CAMERA_APP_RECORD_STATE=str(root / 'record.state'),
                   CAMERA_APP_CAPTURE_ROOT=str(root / 'capture'),
                   CAMERA_APP_SITL_VIDEO1=str(build / 'rgb.h264'),
                   CAMERA_APP_SITL_VIDEO2=str(build / 'thermal.h264'),
                   CAMERA_APP_SITL_PHOTO=str(build / 'photo.jpg'),
                   CAMERA_GIMBAL_SITL_RUNTIME=str(root),
                   CAMERA_GIMBAL_SITL_CAMERA_EXE=str(build / 'camera-app'),
                   CAMERA_GIMBAL_SITL_WEB_EXE=str(build / 'mt11-web-portable'))
        env.pop('CAMERA_APP_SITL_TERRAIN', None)
        camera = web = link = None
        with (root / 'run/camera.log').open('w') as log, (root / 'run/web.log').open('w') as wlog:
            try:
                camera = subprocess.Popen([str(build / 'camera-app')], env=env, stdout=log, stderr=subprocess.STDOUT)
                wait_ready(ready, camera)
                pid = camera.pid
                link = connect(f'tcp:127.0.0.1:{tcp_port}')
                web = subprocess.Popen([str(build / 'mt11-web-portable'), '-p', str(web_port)],
                                       env=env, stdout=wlog, stderr=subprocess.STDOUT)
                base = f'http://127.0.0.1:{web_port}'
                headers = {'Authorization': 'Basic ' + base64.b64encode(b'admin:test-password').decode()}

                def page(data=None):
                    req = urllib.request.Request(base + '/parameters', headers=headers,
                        data=None if data is None else urllib.parse.urlencode(data).encode())
                    with urllib.request.urlopen(req, timeout=15) as response:
                        return response.read().decode()

                def web_ready():
                    try:
                        return 'Tracking control method' in page()
                    except OSError:
                        return False
                wait_for(web_ready)

                def save(**changes):
                    values = ParameterPage(page()).values
                    assert 'csrf' in values
                    values.update(action='save', **changes)
                    result = page(values)
                    return ParameterPage(result).notice

                def heartbeat(armed=True):
                    link.mav.srcSystem, link.mav.srcComponent = 42, 1
                    link.mav.heartbeat_send(M.MAV_TYPE_FIXED_WING, M.MAV_AUTOPILOT_ARDUPILOTMEGA,
                        M.MAV_MODE_FLAG_SAFETY_ARMED if armed else 0, 0, M.MAV_STATE_ACTIVE)
                    link.mav.srcSystem, link.mav.srcComponent = 255, 190

                heartbeat()
                notice = save(tracking_method='rate', brightness='70', saturation='40',
                              contrast='80', autorecord='while_armed')
                assert 'All saved settings applied' in notice, notice
                assert read(link, 'TRACK_METHOD') == 1
                assert read(link, 'IMG_BRIGHTNESS') == 70
                assert read(link, 'IMG_SATURATION') == 40
                assert read(link, 'IMG_CONTRAST') == 80
                assert read(link, 'REC_AUTOSTART') == 2

                def recording():
                    link.mav.command_long_send(42, 100, M.MAV_CMD_REQUEST_CAMERA_CAPTURE_STATUS,
                                              0, 1, 0, 0, 0, 0, 0, 0)
                    return receive(link, 'CAMERA_CAPTURE_STATUS').video_status != 0
                assert recording(), 'While Armed did not apply to an already armed vehicle'

                lat, lon = -35.2785018, 148.9534632
                def flight(elapsed):
                    phase = .25 * elapsed
                    link.mav.srcSystem, link.mav.srcComponent = 42, 1
                    q = Quaternion([0, 0, math.remainder(.18 * elapsed + 5 * math.pi / 6, 2 * math.pi)]).q
                    link.mav.autopilot_state_for_gimbal_device_send(42, 154,
                        round(time.monotonic() * 1e6), q, 0, 0, 0, 0, 0, .18, 0, M.MAV_LANDED_STATE_IN_AIR)
                    link.mav.global_position_int_send(round(elapsed * 1000),
                        round((lat + math.degrees(100 * math.cos(phase) / 6378137)) * 1e7),
                        round((lon + math.degrees(100 * math.sin(phase) / (6378137 * math.cos(math.radians(lat))))) * 1e7),
                        600000, 100000, round(-2500 * math.sin(phase)), round(2500 * math.cos(phase)), 0, 65535)
                    link.mav.srcSystem, link.mav.srcComponent = 255, 190

                flight(0)
                link.mav.command_int_send(42, 154, M.MAV_FRAME_GLOBAL, M.MAV_CMD_DO_SET_ROI_LOCATION,
                    0, 0, 0, 0, 0, 0, round(lat * 1e7), round(lon * 1e7), 500)
                ack = receive(link, 'COMMAND_ACK', lambda m: m.command == M.MAV_CMD_DO_SET_ROI_LOCATION)
                assert ack.result == M.MAV_RESULT_ACCEPTED
                start = time.monotonic()
                errors = []
                before_angles = mcu.count()
                for frame in range(300):
                    time.sleep(max(0, start + frame * .05 - time.monotonic()))
                    elapsed = time.monotonic() - start
                    flight(elapsed)
                    if frame % 20 == 0:
                        heartbeat()
                    while link.recv_match(blocking=False) is not None:
                        pass
                    if elapsed > 4:
                        with mcu.lock:
                            errors.append(mcu.gimbal.yaw - (30 + math.degrees(.07 * elapsed)))
                assert mcu.count() == before_angles, 'Rate tracking sent absolute angle commands'
                assert max(map(abs, errors)) < 2, (min(errors), max(errors))
                print('PASS moving ROI using rates: peak yaw error %.3f degrees' % max(map(abs, errors)), flush=True)

                time.sleep(1.8)
                with mcu.lock:
                    assert mcu.gimbal.yaw_rate == mcu.gimbal.pitch_rate == 0, 'stale vehicle data did not stop rates'
                flight(15)
                time.sleep(.2)
                with mcu.lock:
                    mcu.drop_feedback = True
                for frame in range(24):
                    flight(15 + frame * .05)
                    time.sleep(.05)
                with mcu.lock:
                    assert mcu.gimbal.yaw_rate == mcu.gimbal.pitch_rate == 0, 'stale gimbal feedback did not stop rates'
                    mcu.drop_feedback = False
                print('PASS stale vehicle data and stale gimbal feedback stop rate commands', flush=True)

                flight(17)
                notice = save(tracking_method='angle', brightness='75', main_resolution='1280x720')
                assert 'video format waits' in notice, notice
                assert read(link, 'TRACK_METHOD') == 0 and read(link, 'IMG_BRIGHTNESS') == 75
                assert read(link, 'VIDEO_MAIN_RES') == 1, 'recording format changed mid-recording'
                flight(17.5)
                time.sleep(.3)
                assert mcu.count() > before_angles, 'switch to Angle did not change control method'
                assert recording()
                link.mav.command_long_send(42, 100, M.MAV_CMD_VIDEO_STOP_CAPTURE, 0, 0, 0, 0, 0, 0, 0, 0)
                receive(link, 'COMMAND_ACK', lambda m: m.command == M.MAV_CMD_VIDEO_STOP_CAPTURE)
                wait_for(lambda: read(link, 'VIDEO_MAIN_RES') == 0)
                assert 'All saved settings applied' in page()
                assert camera.poll() is None and camera.pid == pid
                print('PASS web changes apply while recording; format waits for stop, without app restart', flush=True)

                notice = save(mavlink_tcp_port='14551')
                assert 'Restart to apply' in notice, notice
                good = config.read_text()
                config.write_text(good.replace('brightness = "75"', 'brightness = "999"'))
                wait_for(lambda: 'Could not' in page() or 'invalid' in page())
                assert read(link, 'IMG_BRIGHTNESS') == 75, 'invalid config changed active settings'
                replacement = config.with_suffix('.new')
                replacement.write_text(good.replace('brightness = "75"', 'brightness = "82"'))
                replacement.replace(config)
                wait_for(lambda: read(link, 'IMG_BRIGHTNESS') == 82)
                assert camera.poll() is None and camera.pid == pid
                print('PASS restart-only acknowledgement, invalid file rejection and atomic external config reload', flush=True)
            except Exception:
                print((root / 'run/camera.log').read_text())
                print((root / 'run/web.log').read_text())
                raise
            finally:
                if link:
                    link.close()
                stop(web)
                stop(camera)
                mcu.close()


if __name__ == '__main__':
    main()
