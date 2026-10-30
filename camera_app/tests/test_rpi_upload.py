#!/usr/bin/env python3
"""RPi cloud upload inside camera-app, against a fake rclone: a scan on startup,
the status file, stopping when the vehicle arms or its heartbeat is lost, and
live reload of the [upload] settings."""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

os.environ.setdefault("MAVLINK20", "1")

from pymavlink import mavutil  # noqa: E402


def reserve_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def write_config(path, enable, folder="cam"):
    # Replace the file so the camera app's change detection always sees it.
    temporary = path.with_suffix(".tmp")
    temporary.write_text(
        "[general]\ntimezone = UTC\n"
        "[mavlink]\nsystem_id = 1\n"
        f"[upload]\nenable = {'true' if enable else 'false'}\nfolder = {folder}\nscan_minutes = 1\n",
        encoding="ascii",
    )
    os.replace(temporary, path)


def status(path):
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except FileNotFoundError:
        return {}
    return dict(line.split("=", 1) for line in lines if "=" in line)


def wait_for(description, condition, process, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        assert process.poll() is None, f"camera app exited {process.returncode}"
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {description}")


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_rpi_upload.py CAMERA_APP_RPI_HOST")
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    fake = str(pathlib.Path(__file__).with_name("fake_rclone.sh").resolve())
    mavlink_port = reserve_port()
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        for name in ("capture", "record"):
            (temporary / name).mkdir()
        ready = temporary / "ready"
        upload = pathlib.Path(str(ready) + ".upload")
        config = temporary / "camera.ini"
        mode = temporary / "mode"
        log = temporary / "runs.log"
        pid_file = temporary / "fake.pid"
        mode.write_text("ok")
        write_config(config, True)
        env = os.environ.copy()
        env.update({
            "CAMERA_APP_READY_PATH": str(ready),
            "CAMERA_APP_MAVLINK_TCP_PORT": str(mavlink_port),
            "CAMERA_APP_MAVLINK_UDP_PORT": "0",
            "CAMERA_APP_RECORD_ROOT": str(temporary / "record"),
            "CAMERA_APP_CAPTURE_ROOT": str(temporary / "capture"),
            "CAMERA_APP_LOG_ROOT": str(temporary),
            "CAMERA_APP_RCLONE": fake,
            "CAMERA_APP_RCLONE_CONFIG": str(temporary / "rclone.conf"),
            "CAMERA_APP_UPLOAD_HOLDOFF_MS": "6000",
            "FAKE_RCLONE_MODE_FILE": str(mode),
            "FAKE_RCLONE_LOG": str(log),
            "FAKE_RCLONE_PID": str(pid_file),
        })
        process = subprocess.Popen(
            [binary, "--backend", "rpi_libcam_caddx", "--config", str(config)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        try:
            wait_for("ready", ready.exists, process)

            # With no credentials saved nothing runs and the status says why.
            wait_for("no credentials", lambda: status(upload).get("state") == "disabled" and
                     "No Google Drive credentials" in status(upload).get("reason", ""), process)
            assert not log.exists() or not log.read_text(), "rclone ran without credentials"
            (temporary / "rclone.conf").write_text("[gdrive]\ntype = drive\n", encoding="ascii")
            # The first scan waits for the start-up hold-off (6 s here, 30 s normally), even
            # though no flight controller has been seen.
            time.sleep(1.0)
            assert not log.exists() or not log.read_text(), "uploaded during the start-up hold-off"
            assert status(upload).get("state") == "waiting", status(upload)

            # No flight controller seen: counts as disarmed, so the scan runs at once.
            done = wait_for("the first scan",
                            lambda: status(upload).get("state") == "waiting" and
                            status(upload).get("last_success", "0") != "0" and status(upload), process)
            assert done["files_done"] == "2" and done["files_total"] == "4", done
            assert done["last_error"] == "", done
            runs = log.read_text().splitlines()
            assert len(runs) == 2 and "gdrive:cam/photos" in runs[0] and "gdrive:cam/recordings" in runs[1], runs

            # The web page's "Upload now" button creates a file next to the ready file; it
            # starts the next scan at once, long before the scan interval.
            before = len(log.read_text().splitlines())
            pathlib.Path(str(ready) + ".upload-now").write_text("")
            wait_for("upload now", lambda: len(log.read_text().splitlines()) >= before + 2, process)
            assert not pathlib.Path(str(ready) + ".upload-now").exists()
            wait_for("upload now done", lambda: status(upload).get("state") == "waiting", process)

            # Disarming (after being armed) looks for new files again; make that run slow.
            mode.write_text("slow")
            connection = mavutil.mavlink_connection(
                f"tcp:127.0.0.1:{mavlink_port}", source_system=1, source_component=1)

            def heartbeat(armed):
                connection.mav.heartbeat_send(
                    mavutil.mavlink.MAV_TYPE_QUADROTOR, mavutil.mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA,
                    mavutil.mavlink.MAV_MODE_FLAG_SAFETY_ARMED if armed else 0, 0, 4)

            heartbeat(True)
            time.sleep(0.5)
            heartbeat(False)
            wait_for("a scan after disarming", lambda: status(upload).get("state") == "running", process)
            slow_pid = int(wait_for("the fake rclone", pid_file.exists, process) and pid_file.read_text())
            assert alive(slow_pid)

            # Armed: rclone is stopped, and that is not an error.
            heartbeat(True)
            wait_for("stopping when armed", lambda: status(upload).get("state") == "waiting", process)
            assert not alive(slow_pid) and status(upload)["last_error"] == ""

            # Disarmed again runs it; then the heartbeat stops, which counts as armed.
            heartbeat(False)
            wait_for("a second scan", lambda: status(upload).get("state") == "running", process)
            slow_pid = int(pid_file.read_text())
            time.sleep(5.5)
            wait_for("stopping when the heartbeat is lost",
                     lambda: status(upload).get("state") == "waiting", process)
            assert not alive(slow_pid)
            connection.close()

            # Turning it off in the settings applies without a restart.
            mode.write_text("ok")
            write_config(config, False)
            wait_for("disabling", lambda: status(upload).get("state") == "disabled", process)
            assert status(upload)["reason"] == "Disabled"

            # A folder with a colon is rejected and the old settings are kept.
            before = log.read_text()
            write_config(config, True, folder="gdrive:cam")
            time.sleep(1.5)
            assert status(upload).get("state") == "disabled" and log.read_text() == before
            assert process.poll() is None, "camera app restarted"
        finally:
            process.terminate()
            try:
                output, _ = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                output, _ = process.communicate()
        assert "Upload folder must be blank or a folder name" in output, output

        # A capture root too long for the uploader is refused, not truncated
        # into a path that could name another directory.
        long_capture = temporary / ("c" * 200) / ("c" * 100)
        long_capture.mkdir(parents=True)
        write_config(config, True)
        env["CAMERA_APP_CAPTURE_ROOT"] = str(long_capture)
        env["CAMERA_APP_MAVLINK_TCP_PORT"] = str(reserve_port())
        result = subprocess.run(
            [binary, "--backend", "rpi_libcam_caddx", "--config", str(config)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10,
        )
        assert result.returncode != 0 and "File name too long" in result.stdout, result.stdout
    print("PASS RPi upload: startup scan, status file, stop when armed or heartbeat lost, live settings,\n"
          "     media roots too long to upload are refused")


if __name__ == "__main__":
    main()
