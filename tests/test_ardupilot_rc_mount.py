#!/usr/bin/env python3
"""Run issue #3's RC joystick regression with real ArduPilot and camera SITL."""

import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys

from test_ardupilot_mavlink import (
    connect_mavlink,
    terminate,
    wait_path,
    write_params,
    request_device_message,
)
from test_rc_mount_rate import exercise, Vendor, M
from pymavlink import DFReader

ROOT = Path(__file__).resolve().parents[1]
USED_PORTS = set()


def reserve_port():
    # The camera's vendor endpoint binds both TCP and UDP. Checking just TCP
    # can select a number occupied by the gimbal's UDP socket (or another app).
    for _ in range(100):
        with socket.socket() as tcp, socket.socket(type=socket.SOCK_DGRAM) as udp:
            tcp.bind(("127.0.0.1", 0))
            port = tcp.getsockname()[1]
            if port in USED_PORTS:
                continue
            try:
                udp.bind(("127.0.0.1", port))
            except OSError:
                continue
            USED_PORTS.add(port)
            return port
    raise RuntimeError("Unable to allocate distinct TCP/UDP test ports")


def run(arducopter, backend, transport, inverted, output):
    root = output / f'{backend}-{transport}-{"inverted" if inverted else "upright"}'
    root.mkdir(parents=True, exist_ok=True)
    network_port = reserve_port()
    vendor_port = reserve_port()
    gimbal_port = reserve_port()
    telemetry_port = reserve_port()
    params = root / "ardupilot.parm"
    write_params(params, transport, network_port)
    with params.open("a") as f:
        f.write("MNT1_RC_RATE 90\nRC9_OPTION 213\nRC10_OPTION 214\nMNT1_DEFLT_MODE 3\n")
    config = root / "camera.ini"
    config.write_text("[logging]\ndisarmed=true\n")
    ready = root / "camera.ready"
    gimbal_ready = root / "gimbal.ready"
    ready.unlink(missing_ok=True)
    gimbal_ready.unlink(missing_ok=True)
    env = dict(
        os.environ,
        CAMERA_APP_BACKEND=backend,
        CAMERA_APP_UART=f"udp://127.0.0.1:{gimbal_port}",
        CAMERA_APP_PORT=str(vendor_port),
        CAMERA_APP_MAVLINK_TCP_PORT=str(network_port if transport == "tcp" else 0),
        CAMERA_APP_MAVLINK_UDP_PORT=str(network_port if transport == "udp" else 0),
        CAMERA_APP_CONFIG=str(config),
        CAMERA_APP_READY_PATH=str(ready),
        CAMERA_APP_LOG_ROOT=str(root / "logs"),
        CAMERA_APP_RECORD_ROOT=str(root / "record"),
        CAMERA_APP_CAPTURE_ROOT=str(root / "capture"),
        CAMERA_APP_RTSP_PORT=str(reserve_port()),
    )
    processes = []
    files = []
    link = None
    vendor = None

    def start(argv, name):
        log = (root / name).open("w")
        files.append(log)
        p = subprocess.Popen(
            argv, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT
        )
        processes.append(p)
        return p

    try:
        g = start(
            [
                sys.executable,
                str(ROOT / "sitl/gimbal_sim.py"),
                "--backend",
                backend,
                "--port",
                str(gimbal_port),
                "--orientation",
                "inverted" if inverted else "upright",
                "--ready-file",
                str(gimbal_ready),
            ],
            "gimbal.log",
        )
        wait_path(gimbal_ready, g)
        build = "sitl" if backend == "mt11" else "a8-sitl"
        camera = start([str(ROOT / "build" / build / "camera-app")], "camera.log")
        wait_path(ready, camera)
        ap = start(
            [
                str(arducopter),
                "--wipe",
                "--model",
                "quad",
                "--speedup",
                "1",
                "--home",
                "-35.363262,149.165237,584,90",
                "--instance",
                str(os.getpid() % 100 + 200),
                "--serial0",
                f"tcp:{telemetry_port}",
                "--serial1",
                "none",
                "--serial2",
                "none",
                "--defaults",
                str(params),
            ],
            "arducopter.log",
        )
        link = connect_mavlink(telemetry_port, ap)
        # NET and MAVLink routing initialise after the first FC heartbeat.
        # Retry a request which can arrive before the device route is learned.
        for attempt in range(15):
            try:
                request_device_message(
                    link,
                    154,
                    M.MAVLINK_MSG_ID_GIMBAL_DEVICE_INFORMATION,
                    "GIMBAL_DEVICE_INFORMATION",
                    timeout=2,
                )
                break
            except TimeoutError:
                if attempt == 14:
                    raise
        vendor = Vendor("127.0.0.1", vendor_port, backend, inverted)
        exercise(link, vendor, root)
        print(
            f"PASS {backend} {transport} inverted={inverted}: ArduPilot RC options 213/214, MNT1_RC_RATE=90",
            flush=True,
        )
    finally:
        if vendor:
            vendor.socket.close()
        if link:
            link.close()
        for p in reversed(processes):
            terminate(p)
        for f in files:
            f.close()
    reader = DFReader.DFReader_binary(str(sorted((root / "logs").glob("*.BIN"))[-1]))
    rates = set()
    try:
        while message := reader.recv_msg():
            if message.get_type() == "GCMD" and message.Mode == 2:
                rates.add((round(message.Pitch), round(message.Yaw)))
            if message.get_type() == "STAT":
                assert message.Dropped == 0 and message.Errors == 0, message
    finally:
        reader.close()
    assert {(18, 0), (-18, 0), (0, 18), (0, -18), (0, 0)} <= rates, rates
    print("PASS BIN confirms both signs on both axes and zero-rate stops", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--arducopter", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--backend", choices=("a8", "mt11"), action="append")
    a = p.parse_args()
    for backend in a.backend or ("a8", "mt11"):
        for transport in ("tcp", "udp"):
            run(
                a.arducopter.resolve(),
                backend,
                transport,
                backend == "a8",
                a.output.resolve(),
            )


if __name__ == "__main__":
    main()
