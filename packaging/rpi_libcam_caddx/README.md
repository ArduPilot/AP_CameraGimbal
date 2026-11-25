# Raspberry Pi + libcamera + CADDX gimbal

The `rpi_libcam_caddx` target runs AP_CameraGimbal on a Raspberry Pi CM4
companion computer with an IMX477 CSI camera and a CADDX gimbal, same as
XFRobot C20-T. It runs alongside [Rpanion-server](https://github.com/stephendade/Rpanion-server), which
keeps ownership of networking and the flight-controller link.

Video is captured with libcamera and encoded by the Pi 4 / CM4 hardware H.264
encoder. A single stream is served at `rtsp://<pi>:8554/video1` and used for
recording, at the main resolution: 1920x1080 (default), 1280x720, 960x540,
854x480, 640x360, 426x240 or 320x180, set by `[stream.main] resolution` and the
`VIDEO_MAIN_RES` parameter, and on the web Parameters page. Smaller sizes need
less bandwidth. The size applies without restarting the camera app, but not
while recording. Its bitrate (default 4096 kbps, 1000 to 20000) and frame
rate (default 30, the most the encoder handles at 1080p; 5 to 30) are
`[stream.main] bitrate_kbps` and `frame_rate` in `camera.ini`, the MAVLink
parameters `VIDEO_MAIN_KBPS` and `VIDEO_MAIN_FPS`, and on the web Parameters
page.
The bitrate applies immediately; the frame rate applies after the camera app
is restarted. The `IMG_*` camera parameters (brightness,
contrast, saturation, exposure compensation, ISO, shutter, metering and white
balance) can be changed from the GCS while streaming.

The web interface is served at `http://<pi>:8080/`. The CADDX gimbal is
controlled over a UART; see [Gimbal](#gimbal).

Photos are taken from the GCS (including interval capture) or the web
Photos page and saved as JPEGs with GPS metadata under
`/var/lib/ap_cameragimbal/capture`. Their size is set by `[capture]
resolution` (MAVLink parameter `PHOTO_RES`, also on the web Parameters page):

| Setting | Photo | Video during the photo |
|---|---|---|
| `video` (default) | video frame at the main resolution | not interrupted |
| `2028x1520` | 2x2 binned sensor, full field of view | paused about 0.5 s |
| `4056x3040` | full sensor resolution | paused about 0.7 s |

The pauses are for 30 fps video. The first six frames after each photo are
skipped, so at a lower `VIDEO_MAIN_FPS` they last longer: about 0.9 to 1.0 s at
15 fps for the full-size photo.

The larger sizes switch the sensor mode and back, holding the exposure and
white balance of the last video frame, so they match the video's brightness
and colour. A 4056x3040 photo takes about 0.7 s in all, so interval capture
at one photo per second works. RTSP viewers see a frozen frame. Recordings
skip the paused frames: the MP4 plays continuously and is shorter than the
real time by the pauses. Photos are taken one at a time: while one is being
taken (up to about 0.7 s, or about 2 s if the capture fails) the camera app
handles no other MAVLink messages, so later requests and gimbal commands wait
until it is saved.

Not yet supported: zoom and overlays. A Pi 5 / CM5 needs a software encoder,
which is not implemented yet.

## Build

On the Pi (Raspberry Pi OS Bookworm or later):

```sh
sudo apt install build-essential git python3 python3-jinja2 python3-jsonschema cmake libcamera-dev libjpeg-dev pkg-config
git submodule update --init --recursive
make dependencies
make rpi_libcam_caddx
```

The binaries are `camera_app/build/rpi_libcam_caddx/camera-app` and
`web/rpi_libcam_caddx-web`. The build also compiles the Mbed TLS submodule
(`modules/mbedtls`) that [cloud upload](#cloud-upload) uses, which is why it needs
`cmake` and the two Python modules.

## Install

```sh
sudo install -m 755 camera_app/build/rpi_libcam_caddx/camera-app /usr/local/bin/ap-camera-app
sudo install -m 755 web/rpi_libcam_caddx-web /usr/local/bin/ap-camera-web
sudo install -D -m 644 -t /usr/local/share/ap_cameragimbal/webroot web/webroot/*.*
sudo install -D -m 644 packaging/rpi_libcam_caddx/camera.ini /etc/ap_cameragimbal/camera.ini
sudo install -m 644 packaging/rpi_libcam_caddx/ap-cameragimbal.service \
    packaging/rpi_libcam_caddx/ap-cameragimbal-web.service /etc/systemd/system/
sudo install -D -m 644 packaging/ca-certificates/cloud-roots.pem /usr/local/share/ap_cameragimbal/ca-certificates.pem
sudo systemctl daemon-reload
sudo systemctl enable --now ap-cameragimbal ap-cameragimbal-web
journalctl -u ap-cameragimbal -f
```

Only one process can use the camera, so leave Rpanion-server's video
streaming stopped.

To update, `git pull`, rebuild, repeat the first three install commands and the
CA certificates one, and run
`sudo systemctl restart ap-cameragimbal ap-cameragimbal-web`. Installing
`camera.ini` again replaces any settings changed from the GCS or web page.

## Web interface

Browse to `http://<pi>:8080/` (for example `http://10.0.2.100:8080/` over
Rpanion-server's WiFi access point) and log in as `admin` with password
`ardupilot`; the first start writes it to `/etc/ap_cameragimbal/web.pass`.
Change it on the Users page.

The Live tab plays the camera's video in the browser, the Parameters page
edits `/etc/ap_cameragimbal/camera.ini`, the Debug tab shows the camera
service's journal, and the file browser covers recordings under
`/var/lib/ap_cameragimbal`. Restarting the camera app stops it and lets
systemd start it again; Reboot reboots the Pi. Firmware upload is not offered.
The web page sends its camera controls to camera-app's MAVLink TCP port
(`[mavlink] tcp_port`, 16001).

## MAVLink

The camera listens for MAVLink on UDP port 16001. In Rpanion-server's
Flight Controller page, add a UDP output to `127.0.0.1:16001` so mavlink-router
forwards traffic between the flight controller, the GCS and the camera.

Behind mavlink-router the camera cannot tell which network the GCS is on, so
by default the RTSP address it advertises in `VIDEO_STREAM_INFORMATION` is a
guess: an address on 192.168.144.0/24 if the Pi has one (the usual SIYI/MT11
camera subnet), otherwise the numerically lowest non-loopback IPv4 address.
When the Pi has more than one network, for example Ethernet to the flight
controller and WiFi to the GCS, set the URI the GCS can reach in the web
interface under Parameters → Network → Video stream 1 URI (`[network]
video1_uri` in `camera.ini`), a complete `rtsp://` or `rtsps://` URI such as
`rtsp://10.0.0.5:8554/video1.misb` for the default MISB format, or
`rtsp://10.0.0.5:8554/video1` when `[video] telemetry = apcg`. It overrides
primary stream ID 1 in either format. The APCG compatibility stream keeps its
automatically generated address. It is sent to the GCS as is, so it can also set a
different port, a host name or an extra path; a blank value goes back to the
guess. The camera does not check that it serves the path you give: to serve an
extra path, set `alias` under `[stream.main]` as well. Credentials are not
allowed in the URI. It applies without restarting the camera app, but a GCS
that has already read the stream information may need to reconnect.

If the flight controller runs ArduPilot with a MAVLink camera (`CAM1_TYPE` 6),
it keeps its own copy of the camera's stream information and serves that to
the GCS alongside the camera's reply. In testing, with ArduPilot 4.8.0 it kept
serving the old URI after the setting was changed, so the GCS showed the old
address until the flight controller was rebooted.

`CAMERA_APP_VIDEO_ADDRESS` is no longer used. Move any address to the video stream 1 URI
setting and delete its `video-address.conf` systemd drop-in.

## Gimbal

The CADDX gimbal uses the protocol of ArduPilot's `AP_Mount_CADDX` driver
(115200 8N1). It sends no feedback, so the attitude reported to the GCS is the
commanded target. Nothing is sent to the gimbal until the first gimbal command.
Set the link on the web Parameters page (`[network] uart_device_override`, "Gimbal link"). Pick
a suggestion or type another value: a serial device such as `/dev/ttyAMA0`, or
a network address as `udp://IPv4:PORT` or `tcp://IPv4:PORT` (TCP reconnects
if the connection drops). A change needs
a camera-app restart. When the setting is blank, `--uart` is used, and if that
is not given, the `CAMERA_APP_UART` environment variable.

Without a link no gimbal is advertised: camera-app sends no gimbal
heartbeat or `GIMBAL_DEVICE_INFORMATION` and reports gimbal device ID 0 in
`CAMERA_INFORMATION`, so a gimbal connected to the flight controller is not
duplicated.

If the gimbal is wired to the flight controller instead, ArduPilot's serial
passthrough can forward it to the Pi over Ethernet. For a gimbal on SERIAL3
and network port 2 as a UDP server:

```
NET_P2_TYPE = 2         (UDP server; reboot after setting)
NET_P2_PORT = 15002
NET_P2_PROTOCOL = -1
SERIAL_PASS1 = 3        (SERIAL3, keeping SERIAL3_BAUD = 115)
SERIAL_PASS2 = 22       (network ports are numbered from 21)
SERIAL_PASSTIMO = -1    (no timeout, kept across reboots)
```

then set the Gimbal link to `udp://<flight controller IP>:15002`, for example
`udp://192.168.144.15:15002`. While passthrough is active ArduPilot's own CADDX
mount driver cannot reach the gimbal; set `MNT1_TYPE = 6` (MAVLink gimbal
protocol v2).

## Environment variables

- `CAMERA_APP_RPI_CAMERA`: libcamera camera ID (default: first camera).
- `CAMERA_APP_RPI_ENCODER`: V4L2 H.264 encoder device (default `/dev/video11`).
- `CAMERA_APP_UART`: gimbal link, a serial device, `udp://IPv4:PORT` or `tcp://IPv4:PORT`; used
  when `--uart` and the Gimbal link setting are not given.
- `CAMERA_APP_UPLOAD_CA_FILE`, `CAMERA_APP_RCLONE_CONFIG`: the PEM bundle of CA certificates
  the cloud service must chain to (default
  `/usr/local/share/ap_cameragimbal/ca-certificates.pem`) and the file holding the
  Google credentials (default `/etc/ap_cameragimbal/rclone.conf`, in rclone's
  format, which is why it has that name). If you change
  `CAMERA_APP_RCLONE_CONFIG`, set it in both the camera and the web service
  units: the web page saves the credentials to that file and the camera reads it.

## Cloud upload

The camera app can copy photos and recordings to your own Google Drive or Dropbox
(choose the cloud service on the web Parameters page, Cloud Upload tab). It talks
to the service directly (TLS, with the certificates in
`/usr/local/share/ap_cameragimbal/ca-certificates.pem`), so nothing else needs
installing on the camera. It is off by default. You provide your own
credentials, so none of ours are involved; setting them up takes about ten
minutes, once. It waits until the camera's clock is set, since certificates have
dates.

Uploads run while the camera is recording but not while the vehicle is
**armed**, and stop at once when it is armed, so they never compete with
the flight (an **Upload now** that was started while disarmed is the exception: it carries on until it is done). The file being recorded is left until the recording ends, and so is
any file changed in the last few seconds. They also wait 30 s after the camera app starts, in
case the autopilot has not connected yet (so restarting it in flight does not
start an upload). With no flight controller connected the vehicle counts
as disarmed, so a camera on the bench uploads; once a flight controller has
been seen, a heartbeat lost for 5 s counts as armed. Photos go to
`<folder>/photos` and recordings to `<folder>/recordings`, keeping any
sub-folders. Files that are uploaded and checked are remembered (by name, size and
modification time) in `upload-cache-<service>.txt` beside the credentials, so later
checks send only new or changed files and use no network when there are none; changing
the service or folder starts afresh. The camera also sends the GCS three short INFO
messages naming the service: `Upload to Dropbox started (N files)`, `Upload to Dropbox complete` and
`Upload to Dropbox failed` (or Google Drive); files being written (`*.part`, `*.tmp`) are skipped. Large files go
in 4 MiB pieces and carry on from where they stopped after a lost connection. A
failed upload is retried every minute. The camera app looks for new files every
`scan_minutes`, shortly after a recording ends and when the vehicle disarms. **Upload now** on the web page
starts a scan at once, even while armed or recording. The upload runs at
the lowest CPU and disk priority.

### Setting up the credentials

**Follow the step-by-step guide: [docs/cloud-upload-setup.md](../../docs/cloud-upload-setup.md).**
It covers creating the Google or Dropbox app, getting the token with `rclone
authorize`, entering it on the Cloud Upload tab, and the mistakes people most often
make. The files are stored in `/etc/ap_cameragimbal/rclone.conf`, readable
only by root, and are never shown again. The token Google gives for the Drive scope
covers all the files in that Google account's Drive, so use an account just for the
camera if that matters. Revoke access at <https://myaccount.google.com/permissions>
(Google) or <https://www.dropbox.com/account/connected_apps> (Dropbox).

`after_upload` chooses what happens to a file once it is uploaded and checked:
`keep` it, `delete` it, or `delete_when_low`: a check that starts with free space
below `low_space_percent` deletes every file it uploads (not just enough to get
back above the limit), and other checks keep them. A file is deleted
only after the service reports the same size and checksum as the local file (MD5 for Google Drive, content hash
for Dropbox). `bandwidth_kib_s` limits the speed in
KiB/s.

## Defaults when upgrading

Fresh configurations use MAVLink TCP/UDP **16001** and **Rate** position
tracking. Upgrades retain explicit settings in `camera.ini`, so an existing
RPi installation can still listen on **14570**. A configuration without
`mavlink.tracking_method` inherits the new **Rate** default; an explicit
**Angle** setting is retained. Check Parameters for **MAV_TCP_PORT**,
**MAV_UDP_PORT** and **TRACK_METHOD** before changing the controller connection.
