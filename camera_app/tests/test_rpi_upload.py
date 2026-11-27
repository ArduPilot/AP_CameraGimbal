#!/usr/bin/env python3
"""RPi cloud upload inside camera-app, against a fake Google Drive (see
test_gdrive.py): a scan on startup, the status file, stopping when the vehicle
arms or its heartbeat is lost, and live reload of the [upload] settings."""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

os.environ.setdefault("MAVLINK20", "1")

from pymavlink import mavutil  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import test_gdrive as drive  # noqa: E402
import test_dropbox as dropbox  # noqa: E402

CREDENTIALS = ("[gdrive]\ntype = drive\nclient_id = id\nclient_secret = secret/+=\n"
               'token = {"access_token":"old","token_type":"Bearer","refresh_token":"1//refresh"}\n')


DROPBOX_CREDENTIALS = ("[dropbox]\ntype = dropbox\nclient_id = appkey\nclient_secret = app/secret+=\n"
                       'token = {"access_token":"old","token_type":"bearer","refresh_token":"refresh-tok"}\n')


def upload_messages(connection):
    """The upload messages (severity, text) the camera has sent to this connection so far."""
    found = []
    while True:
        message = connection.recv_match(type="STATUSTEXT", blocking=False)
        if message is None:
            return found
        if message.text.startswith("Upload"):
            found.append((message.severity, message.text))


def stored_dropbox():
    """What the fake Dropbox holds: sorted "path:size" lines."""
    with dropbox.lock:
        return sorted("%s:%d" % (shown, len(data)) for shown, data, _ in dropbox.state["files"].values())


def stored():
    """What the fake Drive holds: sorted "folder/path/name:size" lines."""
    with drive.lock:
        return sorted("%s/%s:%d" % (drive.tree_path(parent), name, len(data))
                      for name, parent, data, _ in drive.state["files"].values())


def reserve_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def finished(path):
    """A file that has not changed for a minute: a file changed a moment ago is left for later, as one still being written."""
    then = time.time() - 60
    os.utime(path, (then, then))


def write_config(path, enable, folder="cam", service="gdrive"):
    # Replace the file so the camera app's change detection always sees it.
    temporary = path.with_suffix(".tmp")
    temporary.write_text(
        "[general]\ntimezone = UTC\n"
        "[mavlink]\nsystem_id = 1\n"
        f"[upload]\nenable = {'true' if enable else 'false'}\nservice = {service}\nfolder = {folder}\nscan_minutes = 1\n",
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


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_rpi_upload.py CAMERA_APP_RPI_HOST")
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    mavlink_port = reserve_port()
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        server, cert = drive.start_server(directory)
        for name in ("capture", "record"):
            (temporary / name).mkdir()
        (temporary / "capture" / "a.jpg").write_bytes(b"a" * 1000)
        (temporary / "capture" / "b.jpg").write_bytes(b"b" * 500)
        (temporary / "capture" / "unfinished.part").write_bytes(b"x")
        (temporary / "record" / "c.mp4").write_bytes(b"c" * 3000)
        ready = temporary / "ready"
        upload = pathlib.Path(str(ready) + ".upload")
        config = temporary / "camera.ini"
        write_config(config, True)
        env = os.environ.copy()
        env.update({
            "CAMERA_APP_READY_PATH": str(ready),
            "CAMERA_APP_MAVLINK_TCP_PORT": str(mavlink_port),
            "CAMERA_APP_MAVLINK_UDP_PORT": "0",
            "CAMERA_APP_RECORD_ROOT": str(temporary / "record"),
            "CAMERA_APP_CAPTURE_ROOT": str(temporary / "capture"),
            "CAMERA_APP_LOG_ROOT": str(temporary),
            "CAMERA_APP_UPLOAD_CA_FILE": cert,
            "CAMERA_APP_UPLOAD_TEST_ENDPOINT": f"localhost:{server.server_address[1]}",
            "CAMERA_APP_RCLONE_CONFIG": str(temporary / "rclone.conf"),
            "CAMERA_APP_UPLOAD_HOLDOFF_MS": "6000",
        })
        process = subprocess.Popen(
            [binary, "--backend", "rpi_libcam_caddx", "--config", str(config)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        try:
            wait_for("ready", ready.exists, process)

            # With no credentials saved nothing is sent and the status says why.
            wait_for("no credentials", lambda: status(upload).get("state") == "disabled" and
                     "No Google Drive credentials" in status(upload).get("reason", ""), process)
            assert stored() == [], stored()
            (temporary / "rclone.conf").write_text(CREDENTIALS, encoding="ascii")
            # The first scan waits for the start-up hold-off (6 s here, 30 s normally), even
            # though no flight controller has been seen.
            time.sleep(1.0)
            assert stored() == [], "uploaded during the start-up hold-off"
            # (the credentials are looked for once a second, and the status written about as often)
            wait_for("waiting for the hold-off", lambda: status(upload).get("state") == "waiting", process)
            assert stored() == [], "uploaded during the start-up hold-off"

            # No flight controller seen: counts as disarmed, so the scan runs at once.
            done = wait_for("the first scan",
                            lambda: status(upload).get("state") == "waiting" and
                            status(upload).get("last_success", "0") != "0" and status(upload), process)
            assert done["files_done"] == "3" and done["files_total"] == "3" and done["service"] == "gdrive", done
            assert done["bytes_done"] == "4500" and done["last_error"] == "", done
            expected = ["cam/photos/a.jpg:1000", "cam/photos/b.jpg:500", "cam/recordings/c.mp4:3000"]
            assert stored() == expected, stored()

            # With nothing new, "Upload now" sends nothing and says nothing: the files are remembered.
            connection = mavutil.mavlink_connection(
                f"tcp:127.0.0.1:{mavlink_port}", source_system=1, source_component=1)
            connection.mav.heartbeat_send(mavutil.mavlink.MAV_TYPE_GCS, mavutil.mavlink.MAV_AUTOPILOT_INVALID, 0, 0, 4)
            pathlib.Path(str(ready) + ".upload-now").write_text("")
            wait_for("a scan with nothing new", lambda: not pathlib.Path(str(ready) + ".upload-now").exists(), process)
            time.sleep(1.5)
            assert stored() == expected and not upload_messages(connection), "uploaded or announced with nothing new"
            wait_for("nothing to do", lambda: status(upload).get("state") == "waiting" and
                     status(upload).get("files_total") == "0", process)

            # The web page's "Upload now" button creates a file next to the ready file; it
            # starts the next scan at once, long before the scan interval. A new file shows it,
            # and the ground station is told, at INFO severity, when it starts and when it is done.
            (temporary / "capture" / "new.jpg").write_bytes(b"n" * 200)
            finished(temporary / "capture" / "new.jpg")
            pathlib.Path(str(ready) + ".upload-now").write_text("")
            wait_for("upload now", lambda: "cam/photos/new.jpg:200" in stored(), process)
            assert not pathlib.Path(str(ready) + ".upload-now").exists()
            wait_for("upload now done", lambda: status(upload).get("state") == "waiting", process)
            messages = []
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not any(text.endswith(" complete") for _, text in messages):
                messages += upload_messages(connection)
                time.sleep(0.1)
            assert messages == [(mavutil.mavlink.MAV_SEVERITY_INFO, "Upload to Google Drive started (1 file)"),
                                (mavutil.mavlink.MAV_SEVERITY_INFO, "Upload to Google Drive complete")], messages

            # Disarming (after being armed) looks for new files again; make that one slow.
            with drive.lock:
                drive.state["slow"] = True
            (temporary / "capture" / "slow1.jpg").write_bytes(b"s" * 300)
            finished(temporary / "capture" / "slow1.jpg")
            def heartbeat(armed):
                connection.mav.heartbeat_send(
                    mavutil.mavlink.MAV_TYPE_QUADROTOR, mavutil.mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA,
                    mavutil.mavlink.MAV_MODE_FLAG_SAFETY_ARMED if armed else 0, 0, 4)

            heartbeat(True)
            time.sleep(0.5)
            heartbeat(False)
            wait_for("a scan after disarming", lambda: status(upload).get("state") == "running", process)

            # Armed: the upload is stopped, and that is not an error.
            heartbeat(True)
            wait_for("stopping when armed", lambda: status(upload).get("state") == "waiting", process)
            assert status(upload)["last_error"] == "" and "cam/photos/slow1.jpg:300" not in stored()

            # Disarmed again runs it; then the heartbeat stops, which counts as armed.
            heartbeat(False)
            wait_for("a second scan", lambda: status(upload).get("state") == "running", process)
            time.sleep(5.5)
            wait_for("stopping when the heartbeat is lost",
                     lambda: status(upload).get("state") == "waiting", process)
            assert "cam/photos/slow1.jpg:300" not in stored()
            connection.close()

            # Turning it off in the settings applies without a restart.
            with drive.lock:
                drive.state["slow"] = False
            write_config(config, False)
            wait_for("disabling", lambda: status(upload).get("state") == "disabled", process)
            assert status(upload)["reason"] == "Disabled"
            before = stored()

            # A folder with a colon is rejected and the old settings are kept.
            write_config(config, True, folder="gdrive:cam")
            time.sleep(1.5)
            assert status(upload).get("state") == "disabled" and stored() == before
            assert process.poll() is None, "camera app restarted"
        finally:
            process.terminate()
            try:
                output, _ = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                output, _ = process.communicate()
        assert "Upload folder must be blank or a folder name" in output, output

        # A capture root too long for the uploader turns uploads off, with the reason in
        # their status, instead of being truncated into a path that could name another
        # directory, and the camera app still starts.
        long_capture = temporary / ("c" * 200) / ("c" * 100)
        long_capture.mkdir(parents=True)
        write_config(config, True)
        env["CAMERA_APP_CAPTURE_ROOT"] = str(long_capture)
        env["CAMERA_APP_MAVLINK_TCP_PORT"] = str(reserve_port())
        ready.unlink(missing_ok=True)
        process = subprocess.Popen(
            [binary, "--backend", "rpi_libcam_caddx", "--config", str(config)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        try:
            wait_for("ready with a long path", ready.exists, process)
            wait_for("uploads off for the long path", lambda: status(upload).get("state") == "disabled" and
                     "too long" in status(upload).get("reason", ""), process)
        finally:
            process.terminate()
            process.communicate(timeout=5)

        # The same camera app with the service set to Dropbox, against a fake Dropbox.
        dropbox_directory = temporary / "dropbox-fixture"
        dropbox_directory.mkdir()
        dropbox_server, dropbox_cert = drive.start_server(str(dropbox_directory), dropbox.Handler)
        (temporary / "rclone.conf").write_text(CREDENTIALS + DROPBOX_CREDENTIALS, encoding="ascii")
        write_config(config, True, service="dropbox")
        env["CAMERA_APP_CAPTURE_ROOT"] = str(temporary / "capture")
        env["CAMERA_APP_UPLOAD_CA_FILE"] = dropbox_cert
        env["CAMERA_APP_UPLOAD_TEST_ENDPOINT"] = f"localhost:{dropbox_server.server_address[1]}"
        env["CAMERA_APP_UPLOAD_HOLDOFF_MS"] = "1000"
        env["CAMERA_APP_MAVLINK_TCP_PORT"] = str(reserve_port())
        ready.unlink(missing_ok=True)
        process = subprocess.Popen(
            [binary, "--backend", "rpi_libcam_caddx", "--config", str(config)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        try:
            wait_for("ready for Dropbox", ready.exists, process)
            done = wait_for("the Dropbox scan",
                            lambda: status(upload).get("state") == "waiting" and
                            status(upload).get("last_success", "0") != "0" and status(upload), process, timeout=30.0)
            assert done["files_done"] == "5" and done["last_error"] == "" and done["service"] == "dropbox", done
            assert stored_dropbox() == ["/cam/photos/a.jpg:1000", "/cam/photos/b.jpg:500", "/cam/photos/new.jpg:200",
                                        "/cam/photos/slow1.jpg:300", "/cam/recordings/c.mp4:3000"], stored_dropbox()
        finally:
            process.terminate()
            process.communicate(timeout=5)
        dropbox_server.shutdown()
        server.shutdown()
    print("PASS RPi upload: startup scan, status file, stop when armed or heartbeat lost, live settings,\n"
          "     media roots too long to upload turn uploads off, Dropbox as the service")


if __name__ == "__main__":
    main()
