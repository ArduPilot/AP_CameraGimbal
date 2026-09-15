#!/usr/bin/env python3
"""Run the hardware bench trajectory against the calibrated A8 simulator."""

import argparse
import os
from pathlib import Path
import subprocess
import sys

from test_gimbal_angle_hold import MCU
from test_mavlink_parameters import port, stop, wait_ready

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from gimbal_tracking_bench import run
from pymavlink import DFReader

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    (root / "ready").unlink(missing_ok=True)
    config = root / "camera.ini"
    config.write_text("[mavlink]\nsystem_id=1\n")
    mcu = MCU("a8", 2)
    tcp_port, vendor_port = port(), port()
    env = dict(
        os.environ,
        CAMERA_APP_CONFIG=str(config),
        CAMERA_APP_BACKEND="a8",
        CAMERA_APP_UART=f"udp://127.0.0.1:{mcu.socket.getsockname()[1]}",
        CAMERA_APP_PORT=str(vendor_port),
        CAMERA_APP_MAVLINK_TCP_PORT=str(tcp_port),
        CAMERA_APP_MAVLINK_UDP_PORT="0",
        CAMERA_APP_READY_PATH=str(root / "ready"),
        CAMERA_APP_LOG_ROOT=str(root / "logs"),
        CAMERA_APP_RECORD_ROOT=str(root / "record"),
        CAMERA_APP_CAPTURE_ROOT=str(root / "capture"),
        CAMERA_APP_RTSP_PORT=str(port()),
    )
    camera = None
    try:
        with (root / "camera.log").open("w") as out:
            camera = subprocess.Popen(
                [str(ROOT / "build/a8-sitl/camera-app")],
                env=env,
                stdout=out,
                stderr=subprocess.STDOUT,
            )
            wait_ready(root / "ready", camera)
            run("127.0.0.1", tcp_port, vendor_port, "a8", True)
    finally:
        stop(camera)
        mcu.close()
    r = DFReader.DFReader_binary(str(sorted((root / "logs").glob("*.BIN"))[-1]))
    rows = {"PIDY": [], "PIDP": []}
    while message := r.recv_msg():
        if message.get_type() in rows:
            rows[message.get_type()].append(message)
        if message.get_type() == "GCMD":
            assert message.Mode == 2, message
        if message.get_type() == "STAT":
            assert message.Dropped == 0 and message.Errors == 0, message
    r.close()
    assert all(len(v) > 800 for v in rows.values()), "missing tracking data"
    start = min(v[0].TimeUS for v in rows.values())
    for axis, values in rows.items():
        # The first PID record follows the first synthetic telemetry packet;
        # leave a margin before the next instantaneous target step.
        for lo, hi in [(3, 8), (15, 18), (25, 28), (33, 43), (50, 53)]:
            errors = [
                abs(m.Tar - m.Act)
                for m in values
                if lo < (m.TimeUS - start) * 1e-6 < hi - 0.25
            ]
            peak = max(errors)
            print(f"{axis} {lo}-{hi}s: peak error {peak:.3f} deg", flush=True)
            assert peak < 1.5, (axis, lo, peak)
    print("PASS A8 calibrated rate tracking, including below-minimum-speed ramps")


if __name__ == "__main__":
    main()
