#!/usr/bin/env python3
"""Bounded stationary-bench ROI tracking test using synthetic FC telemetry.

Disconnect the real flight controller first. Saves/restores logging and tracking
parameters, and stops rate commands on exit. Pull the resulting BIN for PID data.
"""

import argparse
import math
import time
import sys

from mt11_rate_sweep import Vendor, parameter
from pymavlink import mavutil
from pymavlink.quaternion import Quaternion


def demand(t):
    if t < 8:
        return 0, 0, -10, 0, "hold"
    if t < 18:
        return 15, 0, -15, 0, "positive_step"
    if t < 28:
        return -15, 0, -5, 0, "negative_step"
    if t < 43:
        return -15 + 2 * (t - 28), 2, -5 - (t - 28) * 2 / 3, -2 / 3, "slow_ramp"
    return 0, 0, -10, 0, "return"


def run(host, port, vendor_port, backend, inverted, system=1):
    M = mavutil.mavlink
    vendor = Vendor(host, vendor_port, backend, inverted)
    link = mavutil.mavlink_connection(
        f"tcp:{host}:{port}", source_system=255, source_component=190
    )
    original = {}
    try:
        link.mav.srcSystem, link.mav.srcComponent = system, 1
        link.mav.heartbeat_send(
            M.MAV_TYPE_FIXED_WING,
            M.MAV_AUTOPILOT_ARDUPILOTMEGA,
            0,
            0,
            M.MAV_STATE_STANDBY,
        )
        link.mav.srcSystem, link.mav.srcComponent = 255, 190
        for name in ("LOG_DISARMED", "MAV_POS_TARGET", "TRACK_METHOD"):
            original[name] = parameter(link, system, name)
        parameter(link, system, "LOG_DISARMED", 1)
        parameter(link, system, "MAV_POS_TARGET", 0)
        vendor.center(-10)
        parameter(link, system, "TRACK_METHOD", 1)
        parameter(link, system, "MAV_POS_TARGET", 1)
        lat, lon = -35.2785018, 148.9534632
        north = math.radians(0.001) * 6378137
        start = time.monotonic()
        phase = ""
        for frame in range(1060):
            time.sleep(max(0, start + frame * 0.05 - time.monotonic()))
            elapsed = time.monotonic() - start
            yaw, yaw_rate, pitch, pitch_rate, current = demand(elapsed)
            if phase != current:
                phase = current
                print(f"{elapsed:.2f}: {phase}", flush=True)
            altitude = 100 - north * math.tan(math.radians(pitch))
            down = north * math.radians(pitch_rate) / math.cos(math.radians(pitch)) ** 2
            link.mav.srcSystem, link.mav.srcComponent = system, 1
            if frame % 20 == 0:
                link.mav.heartbeat_send(
                    M.MAV_TYPE_FIXED_WING,
                    M.MAV_AUTOPILOT_ARDUPILOTMEGA,
                    0,
                    0,
                    M.MAV_STATE_STANDBY,
                )
            link.mav.global_position_int_send(
                round(elapsed * 1000),
                round(lat * 1e7),
                round(lon * 1e7),
                round(altitude * 1000),
                round((altitude - 100) * 1000),
                0,
                0,
                round(down * 100),
                65535,
            )
            link.mav.autopilot_state_for_gimbal_device_send(
                system,
                154,
                round(time.monotonic() * 1e6),
                Quaternion([0, 0, math.radians(-yaw)]).q,
                0,
                0,
                0,
                down,
                0,
                math.radians(-yaw_rate),
                0,
                M.MAV_LANDED_STATE_ON_GROUND,
            )
            link.mav.srcSystem, link.mav.srcComponent = 255, 190
            if frame == 0:
                link.mav.command_int_send(
                    system,
                    154,
                    M.MAV_FRAME_GLOBAL,
                    M.MAV_CMD_DO_SET_ROI_LOCATION,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    round((lat + 0.001) * 1e7),
                    round(lon * 1e7),
                    100,
                )
            while (message := link.recv_match(blocking=False)) is not None:
                if (
                    message.get_type() == "COMMAND_ACK"
                    and message.command == M.MAV_CMD_DO_SET_ROI_LOCATION
                ):
                    assert message.result == M.MAV_RESULT_ACCEPTED, message
            pose = vendor.attitude()
            if abs(pose[0]) > 40 or not -30 < pose[1] < 5:
                raise RuntimeError(f"tracking left bench travel limits: {pose[:2]}")
    finally:
        try:
            parameter(link, system, "MAV_POS_TARGET", 0)
            vendor.stop()
            # Remove the synthetic target before restoring the user's policy.
            link.mav.command_long_send(
                system, 154, M.MAV_CMD_DO_SET_ROI_NONE, 0, 0, 0, 0, 0, 0, 0, 0
            )
            time.sleep(0.2)
        finally:
            for name in ("TRACK_METHOD", "MAV_POS_TARGET", "LOG_DISARMED"):
                if name in original:
                    try:
                        parameter(link, system, name, original[name])
                    except Exception as error:
                        print(
                            f"Restore {name}={original[name]} manually: {error}",
                            file=sys.stderr,
                        )
            vendor.stop()
            vendor.socket.close()
            link.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=16001)
    parser.add_argument("--vendor-port", type=int, default=37260)
    parser.add_argument("--backend", choices=("a8", "mt11", "zr10"), required=True)
    parser.add_argument("--inverted", action="store_true")
    args = parser.parse_args()
    run(args.host, args.port, args.vendor_port, args.backend, args.inverted)


if __name__ == "__main__":
    main()
