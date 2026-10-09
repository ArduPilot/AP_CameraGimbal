#!/usr/bin/env python3
"""Survey interruption by vendor controls and failed capture-start acknowledgement."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

from test_mavlink_parameters import connect, port, receive, stop, survey_status_path, wait_ready, write
from test_sitl import siyi
from test_unigcs import Client
from pymavlink import mavutil

M = mavutil.mavlink
ROOT = Path(__file__).resolve().parents[1]


def run(binary):
    with tempfile.TemporaryDirectory(prefix='survey-controls-') as directory:
        root = Path(directory)
        cfg = root/'camera.ini'
        cfg.write_text('[mavlink]\nsystem_id=42\nposition_targeting=true\n')
        gp, cp, sp, up = (port() for _ in range(4))
        env = dict(os.environ, CAMERA_APP_BACKEND='mt11', CAMERA_APP_CONFIG=str(cfg),
                   CAMERA_APP_UART=f'udp://127.0.0.1:{gp}', CAMERA_APP_PORT=str(sp),
                   CAMERA_APP_UNIGCS_PORT=str(up), CAMERA_APP_DISCOVERY_PORT=str(port()),
                   CAMERA_APP_MAVLINK_TCP_PORT=str(cp), CAMERA_APP_MAVLINK_UDP_PORT='0',
                   CAMERA_APP_RTSP_PORT=str(port()), CAMERA_APP_RAW_THERMAL_PORT=str(port()),
                   CAMERA_APP_CAPTURE_ROOT=str(root/'capture'), CAMERA_APP_RECORD_ROOT=str(root/'record'),
                   CAMERA_APP_LOG_ROOT=str(root/'logs'), CAMERA_APP_READY_PATH=str(root/'camera.ready'))
        for key in ('CAMERA_APP_SITL_VIDEO1', 'CAMERA_APP_SITL_VIDEO2', 'CAMERA_APP_SITL_TERRAIN'):
            env.pop(key, None)
        processes = []
        link = client = None
        status_path = survey_status_path(cfg)
        with (root/'test.log').open('w+') as log:
            try:
                processes.append(subprocess.Popen([sys.executable, str(ROOT/'sitl/gimbal_sim.py'),
                    '--port', str(gp), '--ready-file', str(root/'gimbal.ready')], stdout=log, stderr=log))
                wait_ready(root/'gimbal.ready', processes[-1])
                processes.append(subprocess.Popen([str(binary)], env=env, stdout=log, stderr=log))
                wait_ready(root/'camera.ready', processes[-1])
                link = connect(f'tcp:127.0.0.1:{cp}')
                receive(link, 'HEARTBEAT')

                def command(command_id, params, expected=M.MAV_RESULT_ACCEPTED):
                    link.mav.command_long_send(42, 100, command_id, 0, *(params + [0]*(7-len(params))))
                    ack = receive(link, 'COMMAND_ACK', lambda m: m.command == command_id)
                    assert ack.result == expected, ack

                def state(predicate):
                    deadline = time.monotonic()+3
                    last = None
                    while time.monotonic() < deadline:
                        if status_path.exists():
                            last = json.loads(status_path.read_text())
                            if predicate(last):
                                return last
                        time.sleep(.05)
                    raise AssertionError(last)

                def start():
                    command(M.MAV_CMD_SET_CAMERA_MODE, [0, 2])
                    state(lambda s: s['mode'] == 2 and not s['state'].startswith('survey paused'))

                # Cover every manual SIYI command recognized by the dispatcher.
                for opcode, payload in [(0x07, b'\x0a\x00'), (0x08, b'\x01'),
                                        (0x0e, bytes(4)), (0x40, bytes(6)),
                                        (0x0c, b'\x03'), (0x0c, b'\x04'), (0x0c, b'\x05')]:
                    start()
                    with socket.create_connection(('127.0.0.1', sp), 2) as sock:
                        sock.sendall(siyi(1, opcode, payload))
                        state(lambda s: s['state'] == 'survey paused: manual gimbal control')
                client = Client(up)
                for opcode, payload in [(0x9a, b'\x0a\x00'), (0x9b, b'\x01')]:
                    start()
                    assert client.request(opcode, payload, dest=0x2e, link=0x11) == b'\x01'
                    state(lambda s: s['state'] == 'survey paused: manual gimbal control')

                start()
                assert write(link, 'MAV_POS_TARGET', 0) == 0
                before = state(lambda s: s['state'].startswith('survey paused'))
                command(M.MAV_CMD_IMAGE_START_CAPTURE, [0, .1, 2], M.MAV_RESULT_UNSUPPORTED)
                after = state(lambda s: s['state'] == before['state'])
                assert after['captured'] == before['captured']
                assert not Path(str(cfg)+'.survey.json').exists()
                print('PASS SIYI/UniGCS pause survey, failed capture start is rejected, status uses temporary storage')
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read())
                raise
            finally:
                if client:
                    client.sock.close()
                if link:
                    link.close()
                for process in reversed(processes):
                    stop(process)
                status_path.unlink(missing_ok=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--camera', type=Path, default=ROOT/'build/sitl/camera-app')
    run(parser.parse_args().camera.resolve())
