#!/usr/bin/env python3
"""Measure SDK command response from AP_CameraGimbal BIN logs.

Fits unwrapped gimbal angles over short windows where the *wire command* was
constant, including a settling interval before each window. The gyro fields
are not used as Euler rates: camera-body yaw rate changes with pitch angle.
Output samples overlap and must not be treated as independent observations.
"""
import argparse
import bisect
from collections import defaultdict
import csv
from pathlib import Path
import statistics
import sys
from types import SimpleNamespace

from pymavlink import DFReader


def samples(path, window=0.24, settle=0.10):
    feedback, commands = [], []
    reader = DFReader.DFReader_binary(str(path))
    try:
        while (message := reader.recv_msg()) is not None:
            if message.get_type() == 'GIMB':
                feedback.append(message)
            elif message.get_type() == 'GCMD':
                commands.append(message)
            elif message.get_type() == 'VEND' and message.Opcode in (7, 8, 14):
                data = bytes.fromhex(message.Payload)
                if message.Opcode == 7 and len(data) >= 2:
                    signed = lambda b: b if b < 128 else b - 256
                    commands.append(SimpleNamespace(TimeUS=message.TimeUS, Mode=3,
                        WireY=signed(data[0]), WireP=signed(data[1])))
                else:
                    commands.append(SimpleNamespace(TimeUS=message.TimeUS, Mode=1, WireY=0, WireP=0))
    finally:
        reader.close()
    if len(feedback) < 3 or not commands:
        return
    times = [m.TimeUS * 1e-6 for m in feedback]
    command_times = [m.TimeUS * 1e-6 for m in commands]
    for axis, field in (('Yaw', 'WireY'), ('Pitch', 'WireP')):
        angles = [getattr(feedback[0], axis)]
        for previous, current in zip(feedback, feedback[1:]):
            delta = getattr(current, axis) - getattr(previous, axis)
            if axis == 'Yaw':
                delta = (delta + 180) % 360 - 180
            angles.append(angles[-1] + delta)
        for index, now in enumerate(times):
            left = bisect.bisect_left(times, now - window / 2)
            right = bisect.bisect_right(times, now + window / 2)
            if right - left < 3 or times[right - 1] - times[left] < window * 0.6:
                continue
            first_command = bisect.bisect_right(command_times, times[left] - settle) - 1
            last_command = bisect.bisect_right(command_times, times[right - 1])
            if first_command < 0:
                continue
            if any(m.Mode not in (2, 3) for m in commands[first_command:last_command]):
                continue
            command = getattr(commands[first_command], field)
            if any(getattr(m, field) != command for m in commands[first_command:last_command]):
                continue
            x = [v - now for v in times[left:right]]
            y = angles[left:right]
            xm, ym = statistics.mean(x), statistics.mean(y)
            variance = sum((v - xm) ** 2 for v in x)
            if variance == 0:
                continue
            rate = sum((a - xm) * (b - ym) for a, b in zip(x, y)) / variance
            residual = (sum((b - ym - rate * (a - xm)) ** 2
                            for a, b in zip(x, y)) / len(x)) ** 0.5
            yield axis, int(command), rate, residual, times[index], getattr(feedback[index], 'Pitch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--window', type=float, default=0.24)
    parser.add_argument('--settle', type=float, default=0.10)
    parser.add_argument('--minimum-windows', type=int, default=4)
    parser.add_argument('--samples', type=Path, help='optional CSV of individual measurement windows')
    args = parser.parse_args()
    if args.window <= 0 or args.settle < 0:
        parser.error('window must be positive and settle nonnegative')
    grouped = defaultdict(list)
    detail = []
    for path in args.logs:
        for axis, command, rate, residual, timestamp, pitch in samples(path, args.window, args.settle):
            grouped[axis, command].append(rate)
            detail.append((str(path), axis, command, rate, residual, timestamp, pitch))
    if args.samples:
        with args.samples.open('w', newline='') as out:
            writer = csv.writer(out)
            writer.writerow(('log', 'axis', 'command', 'rate_deg_s', 'fit_rms_deg', 'time_s', 'pitch_deg'))
            writer.writerows(detail)
    writer = csv.writer(sys.stdout)
    writer.writerow(('axis', 'command', 'windows', 'median_deg_s', 'p25_deg_s', 'p75_deg_s'))
    for (axis, command), values in sorted(grouped.items()):
        if len(values) < args.minimum_windows:
            continue
        quartiles = statistics.quantiles(values, n=4, method='inclusive')
        writer.writerow((axis, command, len(values), *[round(v, 3) for v in
                         (statistics.median(values), quartiles[0], quartiles[2])]))


if __name__ == '__main__':
    main()
