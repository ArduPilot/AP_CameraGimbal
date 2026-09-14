#!/usr/bin/env python3
"""A centre ROI must stay steady while a simulated plane circles between position samples."""
import argparse
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import threading
import time

from test_mavlink_parameters import port, stop, wait_ready, connect
from pymavlink import mavutil
from pymavlink.quaternion import Quaternion

ROOT = Path(__file__).resolve().parents[1]
M = mavutil.mavlink


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/sitl')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='sitl-roi-motion-') as tmp:
        root = Path(tmp)
        config = root / 'camera.ini'
        config.write_text('[mavlink]\nsystem_id=42\n')
        gimbal_port, tcp_port = port(), port()
        env = dict(os.environ, CAMERA_APP_CONFIG=str(config), CAMERA_APP_BACKEND='mt11',
                   CAMERA_APP_UART=f'udp://127.0.0.1:{gimbal_port}', CAMERA_APP_PORT=str(port()),
                   CAMERA_APP_MAVLINK_TCP_PORT=str(tcp_port), CAMERA_APP_MAVLINK_UDP_PORT='0',
                   CAMERA_APP_READY_PATH=str(root / 'camera.ready'))
        for key in ('CAMERA_APP_SITL_TERRAIN', 'CAMERA_APP_SITL_VIDEO1', 'CAMERA_APP_SITL_VIDEO2'):
            env.pop(key, None)
        camera = gimbal = link = receiver = None
        finished = threading.Event()
        with (root / 'camera.log').open('w') as log:
            try:
                gimbal = subprocess.Popen([sys.executable, str(ROOT / 'sitl/gimbal_sim.py'),
                    '--port', str(gimbal_port), '--ready-file', str(root / 'gimbal.ready')],
                    stdout=log, stderr=subprocess.STDOUT)
                wait_ready(root / 'gimbal.ready', gimbal)
                camera = subprocess.Popen([str(args.build.resolve() / 'camera-app')], env=env,
                                           stdout=log, stderr=subprocess.STDOUT)
                wait_ready(root / 'camera.ready', camera)
                link = connect(f'tcp:127.0.0.1:{tcp_port}')
                link.mav.srcSystem, link.mav.srcComponent = 42, 1
                link.mav.heartbeat_send(M.MAV_TYPE_FIXED_WING, M.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, 4)
                lat, lon = -35.2785018, 148.9534632
                started = time.monotonic()
                samples = []

                def receive():
                    # Receive independently of the sending loop, and retain the
                    # camera timestamp so socket scheduling isn't pointing error.
                    while not finished.is_set():
                        message = link.recv_match(blocking=True, timeout=0.05)
                        elapsed = time.monotonic() - started
                        if (message is not None and
                                message.get_type() == 'GIMBAL_DEVICE_ATTITUDE_STATUS'):
                            yaw = Quaternion(message.q).euler[2]
                            samples.append((elapsed, message.time_boot_ms / 1000, yaw))

                receiver = threading.Thread(target=receive)
                receiver.start()
                for frame in range(200):
                    due = started + frame * 0.05
                    time.sleep(max(0, due - time.monotonic()))
                    now = time.monotonic() - started
                    phase = 0.25 * now
                    q = Quaternion([0, 0, math.remainder(phase + math.pi / 2, 2 * math.pi)])
                    link.mav.autopilot_state_for_gimbal_device_send(42, 154,
                        round(time.monotonic() * 1e6), q.q, 0, 0, 0, 0, 0, 0.25, 0,
                        M.MAV_LANDED_STATE_IN_AIR)
                    if frame % 5 == 0:  # deliberately sparse position data
                        link.mav.global_position_int_send(round(now * 1000),
                            round((lat + math.degrees(100 * math.cos(phase) / 6378137)) * 1e7),
                            round((lon + math.degrees(100 * math.sin(phase) / (6378137 * math.cos(math.radians(lat))))) * 1e7),
                            600000, 100000, round(-2500 * math.sin(phase)), round(2500 * math.cos(phase)), 0, 65535)
                    if frame == 0:
                        link.mav.command_int_send(42, 154, M.MAV_FRAME_GLOBAL, M.MAV_CMD_DO_SET_ROI_LOCATION,
                            0, 0, 0, 0, 0, 0, round(lat * 1e7), round(lon * 1e7), 500)
                finished.set()
                receiver.join()
                assert len(samples) > 40, samples
                offset = min(received - stamp for received, stamp, yaw in samples)
                errors = [math.degrees(math.remainder(yaw - (0.25 * (stamp + offset) + math.pi),
                                                     2 * math.pi))
                          for received, stamp, yaw in samples if stamp + offset > 2]
                assert len(errors) > 30, errors
                assert max(abs(e) for e in errors) < 0.1, errors
                assert statistics.pstdev(errors) < 0.04, errors
                print('PASS circling ROI with 4 Hz position: yaw error peak %.3f, stddev %.3f degrees' %
                      (max(map(abs, errors)), statistics.pstdev(errors)))
            except Exception:
                print((root / 'camera.log').read_text())
                raise
            finally:
                finished.set()
                if receiver: receiver.join()
                if link: link.close()
                stop(camera)
                stop(gimbal)


if __name__ == '__main__':
    main()
