#!/usr/bin/env python3
"""Bench-test raw SIYI rate commands, with BIN logging and CSV feedback.

Use a stationary base. This deliberately bypasses the MAVLink rate conversion.
ROI tracking is suspended for the sweep and its setting restored on exit.
Run yaw and pitch separately, and repeat upright/inverted and in both orders.
"""
import argparse
import csv
import math
import os
from pathlib import Path
import signal
import socket
import struct
import sys
import time

os.environ['MAVLINK20'] = '1'
from pymavlink import mavutil

# The protocol helpers have no third-party dependencies or hardware side effects.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'sitl'))
from gimbal_sim import crc16, parse_siyi
from target_properties import TARGETS, transform


class Vendor:
    def __init__(self, host, port, backend='mt11', inverted=False):
        self.properties = TARGETS[backend]
        self.inverted = inverted
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.connect((host, port))
        self.socket.settimeout(.2)
        self.sequence = 0

    def send(self, opcode, payload=b''):
        self.sequence = (self.sequence + 1) & 65535
        packet = struct.pack('<2sBHHB', b'\x55\x66', 1, len(payload), self.sequence, opcode) + payload
        self.socket.send(packet + struct.pack('<H', crc16(packet)))

    def stop(self):
        self.send(7, b'\0\0')

    def attitude(self):
        # The app also forwards its own periodic attitude requests. Discard
        # queued replies instead of measuring old poses. MT11's MCU uses its
        # own response sequence counter, so it cannot be matched to our request.
        self.socket.setblocking(False)
        try:
            while self.socket.recv(4096):
                pass
        except BlockingIOError:
            pass
        finally:
            self.socket.settimeout(.2)
        self.send(13)
        deadline = time.monotonic() + .3
        while time.monotonic() < deadline:
            try:
                _, _, opcode, payload = parse_siyi(self.socket.recv(4096))
            except (socket.timeout, ValueError):
                continue
            if opcode == 13 and len(payload) >= 12:
                yaw, pitch, roll, gy, gp, gr = struct.unpack_from('<6h', payload)
                pose = transform(self.properties, 'feedback', self.inverted,
                                 (roll * .1, pitch * .1, yaw * .1))
                rates = transform(self.properties, 'feedback', self.inverted,
                                  (gr * .1, gp * .1, gy * .1), rates=True)
                return (*reversed(pose), *reversed(rates))
        raise TimeoutError('no gimbal feedback; sweep stopped')

    def center(self, pitch=-45):
        self.stop()
        _, wire_pitch, wire_yaw = transform(self.properties, 'angle_command',
                                             self.inverted, (0, pitch, 0))
        self.send(14, struct.pack('<hh', round(wire_yaw * 10), round(wire_pitch * 10)))
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            pose = self.attitude()
            # MT11 absolute yaw can settle several degrees off zero. The
            # sweep measures relative travel; require a stable, central pose.
            if abs(pose[0]) < 8 and abs(pose[1] - pitch) < 2:
                time.sleep(.3)
                settled = self.attitude()
                if abs(settled[0] - pose[0]) < 1 and abs(settled[1] - pose[1]) < .5:
                    return
            time.sleep(.05)
        raise TimeoutError(f'gimbal did not settle at the sweep start position: {pose[:2]}')


def parameter(link, system, name, value=None):
    for _ in range(3):
        if value is None:
            link.mav.param_request_read_send(system, 100, name.encode(), -1)
        else:
            link.mav.param_set_send(system, 100, name.encode(), value, mavutil.mavlink.MAV_PARAM_TYPE_INT32)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            message = link.recv_match(type='PARAM_VALUE', blocking=True, timeout=.2)
            if (message and message.get_srcSystem() == system and
                    message.get_srcComponent() == 100 and message.param_id == name and
                    (value is None or message.param_value == value)):
                return message.param_value
    raise TimeoutError('parameter operation failed: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--backend', choices=('mt11', 'a8', 'zr10'), default='mt11')
    parser.add_argument('--inverted', action='store_true')
    parser.add_argument('--mavlink-rates', action='store_true',
                        help='interpret commands as deg/s and test MAVLink-to-vendor conversion')
    parser.add_argument('--vendor-port', type=int, default=37260)
    parser.add_argument('--mavlink-port', type=int, default=16001)
    parser.add_argument('--system', type=int, default=1)
    parser.add_argument('--axis', choices=('yaw', 'pitch'), required=True)
    parser.add_argument('--commands', default=','.join(str(n) for n in range(13)),
                        help='raw magnitudes 0..100; each nonzero command is tested in both directions')
    parser.add_argument('--reverse', action='store_true', help='test magnitudes in descending order')
    parser.add_argument('--hold', type=float, default=1.5)
    parser.add_argument('--pitch', type=float, default=-45, help='starting pitch in degrees (-70..0)')
    parser.add_argument('--stop-hold', type=float, default=.5, help='seconds of feedback after each zero-rate stop')
    parser.add_argument('--max-travel', type=float, default=25, help='degrees from initial angle before stopping each sample')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        commands = [int(value) for value in args.commands.split(',')]
    except ValueError:
        parser.error('commands must be comma-separated integers')
    if not commands or any(n < 0 or n > 100 for n in commands):
        parser.error('command magnitudes must be 0..100')
    if not 0 < args.hold <= 30 or not 0 < args.max_travel <= 45:
        parser.error('hold must be 0..30 seconds and max-travel 0..45 degrees')
    if not -70 <= args.pitch <= 0 or not 0 <= args.stop_hold <= 5:
        parser.error('pitch must be -70..0 degrees and stop-hold 0..5 seconds')
    if args.reverse:
        commands.reverse()
    vendor = Vendor(args.host, args.vendor_port, args.backend, args.inverted)
    link = mavutil.mavlink_connection(f'tcp:{args.host}:{args.mavlink_port}',
                                      source_system=255, source_component=190)
    original = {}
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        for name in ('LOG_DISARMED', 'MAV_POS_TARGET'):
            original[name] = parameter(link, args.system, name)
        parameter(link, args.system, 'LOG_DISARMED', 1)
        parameter(link, args.system, 'MAV_POS_TARGET', 0)
        with args.output.open('w', newline='') as output:
            writer = csv.writer(output)
            writer.writerow(('monotonic_s', 'elapsed_s', 'axis', 'command', 'yaw_deg', 'pitch_deg',
                             'roll_deg', 'body_yaw_deg_s', 'body_pitch_deg_s', 'body_roll_deg_s', 'phase'))
            for magnitude in commands:
                for command in ([0] if magnitude == 0 else [magnitude, -magnitude]):
                    vendor.center(args.pitch)
                    initial = vendor.attitude()
                    payload = struct.pack('<bb', command if args.axis == 'yaw' else 0,
                                          command if args.axis == 'pitch' else 0)
                    start = time.monotonic()
                    kind = 'MAVLink deg/s' if args.mavlink_rates else 'raw command'
                    print(f'{args.axis}: {kind} {command}', flush=True)
                    while time.monotonic() - start < args.hold:
                        if args.mavlink_rates:
                            link.mav.gimbal_device_set_attitude_send(args.system, 154,
                                mavutil.mavlink.GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME,
                                [math.nan] * 4, math.nan,
                                math.radians(command if args.axis == 'pitch' else 0),
                                math.radians(command if args.axis == 'yaw' else 0))
                        else:
                            vendor.send(7, payload)
                        pose = vendor.attitude()
                        now = time.monotonic()
                        writer.writerow((now, now - start, args.axis, command, *pose, 'run'))
                        index = 0 if args.axis == 'yaw' else 1
                        displacement = math.remainder(pose[index] - initial[index], 360)
                        if abs(displacement) >= args.max_travel or not -80 < pose[1] < 15:
                            print('  travel limit reached; ending sample', flush=True)
                            break
                        time.sleep(.04)
                    vendor.stop()
                    stopped = time.monotonic()
                    while time.monotonic() - stopped < args.stop_hold:
                        pose = vendor.attitude()
                        now = time.monotonic()
                        writer.writerow((now, now - stopped, args.axis, 0, *pose, 'stop'))
                        time.sleep(.04)
                    output.flush()
    finally:
        for _ in range(3):
            try:
                vendor.stop()
            except OSError as error:
                print('Could not send stop:', error, file=sys.stderr)
            time.sleep(.05)
        for name in ('MAV_POS_TARGET', 'LOG_DISARMED'):
            if name in original:
                try:
                    parameter(link, args.system, name, original[name])
                except Exception as error:
                    print(f'Restore {name}={original[name]:g} manually: {error}', file=sys.stderr)
        vendor.socket.close()
        link.close()


if __name__ == '__main__':
    main()
