#!/usr/bin/env python3
"""Stop MT11 SITL processes belonging to this repository."""

import argparse
import os
import pathlib
import signal
import time


def process_details(pid):
    process = pathlib.Path("/proc") / str(pid)
    try:
        if process.stat().st_uid != os.getuid():
            return None
        executable = pathlib.Path(os.readlink(process / "exe")).resolve()
        arguments = (process / "cmdline").read_bytes().split(b"\0")
        arguments = [item.decode("utf-8", "surrogateescape")
                     for item in arguments if item]
        working_directory = pathlib.Path(os.readlink(process / "cwd"))
    except (FileNotFoundError, PermissionError, ProcessLookupError, OSError):
        return None
    return executable, arguments, working_directory


def argument_path(argument, working_directory):
    path = pathlib.Path(argument)
    if not path.is_absolute():
        path = working_directory / path
    return path.resolve()


def classify(pid, repository):
    details = process_details(pid)
    if details is None:
        return None
    executable, arguments, working_directory = details
    camera = (repository / "build/sitl/camera-app").resolve()
    web = (repository / "build/sitl/mt11-web").resolve()
    if executable == camera:
        return "camera-app"
    if executable == web:
        return "web"
    if len(arguments) < 2:
        return None
    try:
        script = argument_path(arguments[1], working_directory)
    except (OSError, RuntimeError):
        return None
    if script == (repository / "sitl/gimbal_sim.py").resolve():
        return "gimbal"
    if script == (repository / "sitl/run.sh").resolve():
        return "launcher"
    return None


def find_processes(repository):
    found = {}
    for entry in pathlib.Path("/proc").iterdir():
        if entry.name.isdigit():
            pid = int(entry.name)
            if pid != os.getpid():
                kind = classify(pid, repository)
                if kind is not None:
                    found[pid] = kind
    return found


def signal_processes(processes, signum):
    for pid in processes:
        try:
            os.kill(pid, signum)
        except ProcessLookupError:
            pass


def wait_for_exit(repository, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = find_processes(repository)
        if not remaining:
            return {}
        time.sleep(0.05)
    return find_processes(repository)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=pathlib.Path)
    parser.add_argument("runtime", type=pathlib.Path)
    args = parser.parse_args()
    repository = args.repository.resolve()
    runtime = args.runtime.resolve()
    processes = find_processes(repository)
    if not processes:
        print("MT11 SITL is not running")
    else:
        description = ", ".join(
            f"{kind} PID {pid}" for pid, kind in sorted(processes.items())
        )
        print(f"Stopping MT11 SITL: {description}")
        launchers = {pid: kind for pid, kind in processes.items()
                     if kind == "launcher"}
        signal_processes(launchers, signal.SIGTERM)
        remaining = wait_for_exit(repository, 3.0)
        if remaining:
            signal_processes(remaining, signal.SIGTERM)
            remaining = wait_for_exit(repository, 2.0)
        if remaining:
            signal_processes(remaining, signal.SIGKILL)
            remaining = wait_for_exit(repository, 1.0)
        if remaining:
            raise SystemExit("Unable to stop: " + ", ".join(
                f"{kind} PID {pid}" for pid, kind in sorted(remaining.items())
            ))
        print("MT11 SITL stopped")
    for name in ("launcher.pid", "gimbal.ready", "camera-app.ready"):
        try:
            (runtime / "run" / name).unlink()
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    main()
