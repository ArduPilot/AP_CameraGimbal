#!/usr/bin/env python3
"""Issue #3: exercise RC mount rate control through an actual ArduPilot instance.

Uses channels 9/10 for mount pitch/yaw, restores all changed parameters, and
releases overrides. The camera must have no competing control clients.
"""

import argparse
import json
import math
import os
from pathlib import Path
import sys
import time

os.environ.setdefault("MAVLINK20", "1")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from mt11_rate_sweep import Vendor
from pymavlink import mavutil

M = mavutil.mavlink


def parameter(link, name, value=None):
    for attempt in range(3):
        if value is None:
            link.mav.param_request_read_send(1, 1, name.encode(), -1)
        else:
            link.mav.param_set_send(1, 1, name.encode(), value, M.MAV_PARAM_TYPE_REAL32)
        until = time.monotonic() + 2
        while time.monotonic() < until:
            m = link.recv_match(type="PARAM_VALUE", blocking=True, timeout=0.1)
            if (
                m
                and m.get_srcSystem() == 1
                and m.get_srcComponent() == 1
                and m.param_id == name
                and (value is None or abs(m.param_value - value) < 0.01)
            ):
                return m.param_value
    raise TimeoutError(f"parameter {name}={value}")


def command(link, command_id, values):
    link.mav.command_long_send(1, 1, command_id, 0, *(values + [0] * (7 - len(values))))
    until = time.monotonic() + 4
    while time.monotonic() < until:
        m = link.recv_match(type="COMMAND_ACK", blocking=True, timeout=0.1)
        if m and m.command == command_id and m.get_srcComponent() == 1:
            assert m.result == M.MAV_RESULT_ACCEPTED, m
            return
    raise TimeoutError(f"command {command_id}")


def exercise(link, vendor, output):
    originals = {}
    rows = []

    def override(pitch=1500, yaw=1500, release=False):
        channels = [65535] * 18
        channels[8:10] = [65534, 65534] if release else [pitch, yaw]
        link.mav.rc_channels_override_send(1, 1, *channels)

    def sample(duration, pitch=1500, yaw=1500, label=""):
        values = []
        end = time.monotonic() + duration
        while time.monotonic() < end:
            override(pitch, yaw)
            pose = vendor.attitude()
            values.append((time.monotonic(), pose[0], pose[1]))
            rows.append(
                {
                    "phase": label,
                    "time": values[-1][0],
                    "yaw": pose[0],
                    "pitch": pose[1],
                }
            )
            assert abs(pose[0]) < 35 and -23 < pose[1] < 12, ("travel guard", pose)
            time.sleep(0.04)
        return values

    def center():
        override()
        command(
            link,
            M.MAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW,
            [-10, 0, math.nan, math.nan, 0, 0, 0],
        )
        end = time.monotonic() + 8
        settled_since = None
        while time.monotonic() < end:
            override()
            pose = vendor.attitude()
            if abs(pose[0]) < 4 and abs(pose[1] + 10) < 1.5:
                if settled_since is None:
                    settled_since = time.monotonic()
                elif time.monotonic() - settled_since > 0.3:
                    break
            else:
                settled_since = None
            time.sleep(0.05)
        else:
            raise AssertionError(("center failed", pose))
        command(
            link,
            M.MAV_CMD_DO_MOUNT_CONTROL,
            [0, 0, 0, 0, 0, 0, M.MAV_MOUNT_MODE_RC_TARGETING],
        )
        sample(0.5, label="centered")

    changes = {"MNT1_RC_RATE": 90, "RC9_OPTION": 213, "RC10_OPTION": 214}
    for channel in (9, 10):
        changes.update(
            {
                f"RC{channel}_MIN": 1000,
                f"RC{channel}_MAX": 2000,
                f"RC{channel}_TRIM": 1500,
                f"RC{channel}_DZ": 0,
                f"RC{channel}_REVERSED": 0,
            }
        )
    try:
        for name in changes:
            originals[name] = parameter(link, name)
        output.mkdir(parents=True, exist_ok=True)
        (output / "original-params.json").write_text(
            json.dumps(originals, indent=2) + "\n"
        )
        override()
        for name, value in changes.items():
            parameter(link, name, value)
        # Let ArduPilot latch centered RC inputs before an angle command;
        # a newly detected stick change otherwise switches back to RC mode.
        for _ in range(15):
            override()
            time.sleep(0.05)
        for axis in ("pitch", "yaw"):
            for direction in (1, -1):
                center()
                index = 2 if axis == "pitch" else 1
                initial = vendor.attitude()[1 if axis == "pitch" else 0]
                pwm = 1500 + direction * 100
                moving = sample(
                    0.65,
                    pitch=pwm if axis == "pitch" else 1500,
                    yaw=pwm if axis == "yaw" else 1500,
                    label=f"{axis} {direction:+d}",
                )
                change = moving[-1][index] - initial
                assert direction * change > 3, (
                    axis,
                    direction,
                    "no rate motion",
                    change,
                )
                sample(0.5, label="braking")
                held = sample(0.7, label="stick centered")
                drift = held[-1][index] - held[0][index]
                assert abs(drift) < 0.8, (axis, "did not stop", drift)
                # Rate mode must retain the new pointing angle when centered,
                # not return to a stick-dependent angle as angle mode would.
                assert direction * (held[-1][index] - initial) > 3, (
                    axis,
                    "centered stick returned to initial angle",
                    held[-1],
                    initial,
                )
                assert abs(held[-1][index] - moving[-1][index]) < 4, (
                    axis,
                    "excessive motion after centering",
                    held[-1],
                    moving[-1],
                )
                print(
                    f"PASS {axis} {direction:+d}: {change:+.2f} deg, centered drift {drift:+.2f} deg",
                    flush=True,
                )
        center()
    finally:
        try:
            override()
            time.sleep(0.3)
            # Stop the physical device even when RC setup or feedback failed.
            vendor.stop()
            restore_errors = []
            for name, value in originals.items():
                try:
                    parameter(link, name, value)
                except Exception as error:
                    restore_errors.append(str(error))
            if restore_errors:
                raise RuntimeError(
                    "Parameter restoration failed: " + "; ".join(restore_errors)
                )
        finally:
            override(release=True)
            output.mkdir(parents=True, exist_ok=True)
            (output / "samples.json").write_text(json.dumps(rows, indent=2) + "\n")
            vendor.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--master", default="udpin:127.0.0.1:14550")
    p.add_argument("--host", required=True)
    p.add_argument("--vendor-port", type=int, default=37260)
    p.add_argument("--backend", choices=("a8", "mt11"), default="a8")
    p.add_argument("--inverted", action="store_true")
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    link = mavutil.mavlink_connection(a.master, source_system=255, source_component=190)
    vendor = Vendor(a.host, a.vendor_port, a.backend, a.inverted)
    try:
        assert link.wait_heartbeat(timeout=5), "no ArduPilot heartbeat"
        exercise(link, vendor, a.output)
    finally:
        vendor.socket.close()
        link.close()


if __name__ == "__main__":
    main()
