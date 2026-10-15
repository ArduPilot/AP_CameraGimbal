#!/usr/bin/env python3
"""RPi video IP address: configured RTSP host, live reload, rejection and automatic fallback."""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

os.environ.setdefault("MAVLINK20", "1")

from pymavlink import mavutil  # noqa: E402

CAMERA_COMPONENT = 100


def reserve_port(socktype):
    sock = socket.socket(socket.AF_INET, socktype)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def wait_path(path, process, timeout=8.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            raise AssertionError(f"camera app exited {process.returncode}")
        time.sleep(0.02)
    raise TimeoutError(path)


def write_config(path, video_ip_address):
    # Replace the file so the camera app's change detection always sees it.
    temporary = path.with_suffix(".tmp")
    temporary.write_text(
        "[general]\ntimezone = UTC\n"
        "[mavlink]\nsystem_id = 1\n"
        f"[network]\nvideo_ip_address = {video_ip_address}\n",
        encoding="ascii",
    )
    os.replace(temporary, path)


def stream_uri(connection):
    connection.mav.command_long_send(
        1, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_REQUEST_VIDEO_STREAM_INFORMATION, 0,
        1, 0, 0, 0, 0, 0, 0)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        message = connection.recv_match(
            type="VIDEO_STREAM_INFORMATION", blocking=True, timeout=0.5)
        if message is not None and message.stream_id == 1:
            return message.uri
    raise AssertionError("no VIDEO_STREAM_INFORMATION")


def wait_uri(connection, predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    uri = None
    while time.monotonic() < deadline:
        uri = stream_uri(connection)
        if predicate(uri):
            return uri
        time.sleep(0.2)
    raise AssertionError(f"unexpected stream URI {uri!r}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_rpi_video_ip_address.py CAMERA_APP_RPI_HOST")
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    mavlink_port = reserve_port(socket.SOCK_STREAM)
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        ready = temporary / "ready"
        config = temporary / "camera.ini"
        write_config(config, "192.0.2.10")
        env = os.environ.copy()
        env.update({
            "CAMERA_APP_READY_PATH": str(ready),
            "CAMERA_APP_MAVLINK_TCP_PORT": str(mavlink_port),
            "CAMERA_APP_MAVLINK_UDP_PORT": "0",
            "CAMERA_APP_RECORD_ROOT": str(temporary),
            "CAMERA_APP_CAPTURE_ROOT": str(temporary),
            "CAMERA_APP_LOG_ROOT": str(temporary),
        })
        process = subprocess.Popen(
            [binary, "--backend", "rpi_libcam_caddx", "--config", str(config)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        try:
            wait_path(ready, process)
            # A loopback MAVLink route stands in for a local mavlink-router.
            connection = mavutil.mavlink_connection(
                f"tcp:127.0.0.1:{mavlink_port}", source_system=250)
            assert stream_uri(connection).startswith("rtsp://192.0.2.10:")

            write_config(config, "198.51.100.7")
            wait_uri(connection, lambda uri: uri.startswith("rtsp://198.51.100.7:"))

            # Loopback is not an address the GCS can use; keep the last good value.
            write_config(config, "127.0.0.1")
            time.sleep(1.5)
            assert stream_uri(connection).startswith("rtsp://198.51.100.7:")

            write_config(config, '""')
            automatic = wait_uri(
                connection, lambda uri: not uri.startswith("rtsp://198.51.100.7:"))
            assert automatic.startswith("rtsp://") and automatic.endswith("/video1")
            assert process.poll() is None, "camera app restarted"
            connection.close()
        finally:
            process.terminate()
            try:
                output, _ = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                output, _ = process.communicate()
        assert "Video IP address must be blank or an IPv4 host address" in output, output
    print("PASS RPi video IP address: configured, live reload, invalid kept previous, automatic")


if __name__ == "__main__":
    main()
