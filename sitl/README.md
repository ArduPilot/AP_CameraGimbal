# AP_CameraGimbal SITL

The simulation runs the real host-built camera application and web service
with hardware media stubs. A separate Python process models the MT11, A8 or
ZR10 gimbal MCU and exchanges that camera's private UART frames with `camera-app`
over connected UDP. The A8 model reproduces its distinct upright/inverted
pitch, yaw and yaw-rate wire representations, including private `0x50` yaw in
the `0..360` degree interval.
Public SIYI clients still use the normal UDP or TCP camera endpoint, so commands
cross both protocol layers exactly as they do on the camera.

Build and test either camera from the repository root:

```sh
make sitl
make sitl-test
make a8_sitl
make a8_sitl-test
make zr10_sitl
make zr10_sitl-test
```

Both `sitl-test` and `a8_sitl-test` run the complete camera/backend/web stack
once upright and once inverted. The A8 cases begin at a physical yaw of -38
degrees. Every case commands a positive/right yaw rate and requires the web UI
to report both yaw and yaw rate increasing, including the MT11 conversion from
its left-positive `0x0D` wire representation.

Start all three processes in the foreground with:

```sh
make sitl-run
make a8_sitl-run
```

Alternatively, use the PyQt desktop launcher from the repository root:

```sh
./sitl_launch.py
```

It offers **MT11 / A8 / ZR10**, **Normal / Inverted**, and **Simple test patterns /
3D terrain and imagery**. Start builds the selected backend and launches SITL;
Stop or closing the window stops its processes, including cameras restarted
through the web UI. Build output and service logs appear in the window, and
**Open Web UI** opens the running camera in your browser.

Windows users can run the standalone installer or portable ZIP without installing
Python, Cygwin or FFmpeg. See [Windows packaging and usage](../windows/README.md).

The launcher supports PyQt6 or PyQt5 (for example, install `python3-pyqt6` on
Debian/Ubuntu). Terrain mode automatically uses `build/terrain-venv/bin/python`
when present, or the `CAMERA_GIMBAL_SITL_PYTHON` override. The port and MAVLink
environment overrides described below still apply; A8 MAVLink remains disabled
by default. An explicit `CAMERA_GIMBAL_SITL_BUILD` directory is used for both
the build and launch. Test the GUI without a display using
`QT_QPA_PLATFORM=offscreen python3 sitl/test_launch.py`.
Add `--real` to build and launch both real backends in isolated temporary
directories, checking mounting orientation, web restarts and process cleanup.

Keep that command running and press Ctrl-C in the same terminal to stop all
three processes. For MT11, another terminal can use `make sitl-kill`. The kill
target matches this repository's exact MT11 SITL executable and script paths
and is safe when SITL is already stopped. A per-model launcher lock prevents
accidental duplicate instances.

The default endpoints are:

- web UI: `http://127.0.0.1:8081/` (login page; username `admin`, password
  `ardupilot`; English, Simplified Chinese or Japanese);
- public SIYI API: UDP and TCP port 37260; and
- H.264 RTSP test streams: `rtsp://127.0.0.1:8554/video1` and `/video2`;
- MAVLink 2 camera/gimbal service: UDP and TCP port 14550; and
- simulated private gimbal link: an automatically selected free UDP port,
  printed by the launcher.

The writable camera filesystem and logs are under `build/sitl/runtime` for
MT11 and `build/a8-sitl/runtime` for A8. The web UI can change its simulated
`camera.ini`, password and media tree without
touching host `/app`, `/mnt`, `/run` or `/dev`. Rebuilding preserves these
runtime settings; `make sitl-clean` resets the complete simulated filesystem.
Firmware uploads remain inside its simulated microSD tree and reboot requests
are explicitly ignored.

Both Live and RTSP streams are generated H.264 test patterns using the same
in-process RTSP implementation as the hardware build. Both video sources use
the configured stream resolutions: 1920x1080 main and 1280x720 sub by default.
A8 shows the same visible scene in both encodings. MT11 thermal video stays at
1280x720, matching the real camera; selecting thermal as main sends visible
video at the configured substream size. A separate visible encoding keeps its
recording dimensions stable during that swap. Simple fixtures are resized and
encoded once at startup, so saved resolution changes also apply after a web
restart. The MT11 thermal stream is grayscale; its Sensors tab receives changing simulated LiDAR ranges and
fixed thermal extrema, and shutter captures create test JPEGs.
Rate, centre and absolute-angle commands update the simulator's bounded gimbal
state and are observable through SIYI attitude queries.

## Optional 3D terrain video

Set `CAMERA_GIMBAL_SITL_VIDEO=terrain` for satellite imagery draped over the
same ArduPilot quantized meshes used by MAVProxy's map3d module. The default
remains `simple`, with the existing generated test streams and no additional
Python dependencies. Both MT11 and A8 support the terrain option.

Install a recent MAVProxy with `mavproxy_map3d`, plus the optional renderer
dependencies. A virtual environment can reuse an already installed MAVProxy:

```sh
python3 -m venv --system-site-packages build/terrain-venv
build/terrain-venv/bin/pip install -r sitl/requirements-terrain.txt
# If needed, install your current MAVProxy checkout into this environment:
# build/terrain-venv/bin/pip install -e /path/to/MAVProxy

CAMERA_GIMBAL_SITL_VIDEO=terrain \
CAMERA_GIMBAL_SITL_PYTHON="$PWD/build/terrain-venv/bin/python" make sitl-run
# Use make a8_sitl-run for A8; explicitly enable its MAVLink port as above.
```

The renderer uses map3d's terrain decoder, mesh rendering, imagery fetching
and on-disk caches. Its terrain endpoint currently resolves to
[`https://plot.ardupilot.org/quantized`](https://plot.ardupilot.org/quantized/layer.json),
the quantized representation of ArduPilot terrain. It uses that endpoint from
the installed map3d module, rather than fetching flat SRTM height images.
Imagery defaults to **Esri World Imagery**; `CAMERA_GIMBAL_SITL_IMAGERY` can
select an existing MAVProxy tile service, such as `MicrosoftSat` (map3d's
current default). First use needs network access; subsequent runs reuse the
normal MAVProxy tile cache.

Send the simulated vehicle's MAVLink stream to the camera. Position and AMSL
altitude come from the selected flight controller, and the view uses the
simulated gimbal's level-referenced roll/pitch and vehicle-relative yaw.
Missing or stale telemetry produces a waiting screen. Gimbal controls and
optical/digital zoom alter the rendered view and its advertised FOV. A8's two
streams encode the same visible scene at their configured resolutions; MT11's
thermal view is a grayscale simulation with the thermal sensor's FOV, **not a
temperature model**. Still photographs continue to use the test JPEG fixture.

The resulting H.264 runs through the ordinary RTSP, web Live, recording and
SupportProxy paths, including per-frame telemetry. The renderer is a separate
child of camera-app and is stopped with it. Web-triggered camera restarts
inherit the selected video mode. Renderer logs report the most recent ten
seconds of frame rate and render/encode p95 and maximum time, so short stalls
are visible rather than hidden by a lifetime average.

Terrain video defaults to **20 fps** for both cameras; override with
`CAMERA_GIMBAL_SITL_FPS` (1–60). The camera requests `GLOBAL_POSITION_INT` and
`AUTOPILOT_STATE_FOR_GIMBAL_DEVICE` at 10 Hz from the selected flight controller,
refreshing the requests every five seconds even when slower telemetry is present.
The latter provides the preferred vehicle attitude; `ATTITUDE` is a metadata
fallback after one second without gimbal-state updates.

Terrain rendering extrapolates position with NED velocity and orientation with
angular rates to the frame time, without delaying video to interpolate future
samples. Prediction is limited to 250 ms on link loss. Small telemetry corrections are
blended into the velocity prediction over 150 ms, avoiding packet-arrival
jitter without buffering future telemetry. Frames are rendered ahead of their
50 ms presentation deadlines and published on that cadence. Two queued encoded
frames plus the frame awaiting display provide about 150 ms of render headroom
at 20 fps, so a short texture upload does not pause playback. Prediction remains
capped at 250 ms; the frame queue cannot grow without bound. Gimbal Euler rates are
estimated from successive timestamped samples with angle wrapping. Recorded SEI
also includes optional NED `velocity` and vehicle `yaw_rate_rad_s` fields; old
telemetry readers continue to work.

Prefetch and performance settings:

| Environment variable | Default | Meaning |
| --- | --- | --- |
| `CAMERA_GIMBAL_SITL_VIDEO` | `simple` | `simple` or `terrain` |
| `CAMERA_GIMBAL_SITL_PYTHON` | `python3` | Python interpreter with renderer dependencies |
| `CAMERA_GIMBAL_SITL_TILE_THREADS` | `16` | Concurrent imagery downloads, 1–64 |
| `CAMERA_GIMBAL_SITL_PREFETCH_RADIUS` | `2` | Fine terrain tile radius around the view, vehicle and predicted position, 1–4 |
| `CAMERA_GIMBAL_SITL_IMAGERY` | `EsriWorldImagery` | Satellite imagery service |
| `CAMERA_GIMBAL_SITL_FPS` | `20` | Terrain video frame rate, 1–60 |

Six terrain workers fetch/decode meshes and assemble textures, alongside the
imagery download pool. Prefetch covers the current view and a ten-second
flight prediction (limited to 3 km), with coarser terrain beyond the fine
region. Completed imagery is applied progressively, with at most one new mesh
or texture applied per frame, since VTK defers OpenGL uploads until rendering.
Texture sizes use stable 256/512/1024/2048-pixel levels with
hysteresis; flying a few metres does not rebuild textures at a different
single-pixel width. Idle terrain result processing is skipped. A fixed-size
framebuffer with per-stream viewports avoids reallocating OpenGL buffers for
different stream sizes. Thermal grayscale conversion uses OpenCV. Two encoder
workers operate independently of downloads; smaller visible encodings reuse
the larger render. The camera allows one outstanding
render request, with at most two completed frames queued for presentation.
RTSP and web live-video TCP connections use `TCP_NODELAY` so a frame's final
packet is not held back by TCP acknowledgement delays.

VTK needs a working OpenGL implementation; recent VTK wheels can render
offscreen with EGL or OSMesa, while older builds may need an X display/Xvfb.
Hardware acceleration helps at 1080p. On slower software renderers, lower the
frame rate or visible stream resolutions. Initial uncached terrain appears
progressively and cannot be guaranteed complete before imagery downloads.

Optional tests (not part of the ordinary offline SITL tests):

```sh
make sitl-terrain-test SITL_TERRAIN_PYTHON="$PWD/build/terrain-venv/bin/python"
make a8_sitl-terrain-test SITL_TERRAIN_PYTHON="$PWD/build/terrain-venv/bin/python"
build/terrain-venv/bin/python sitl/test_terrain_video.py --network \
    --seconds 35 --output /tmp/terrain-video-check
```

The geometry/encoding test uses synthetic meshes and delayed worker jobs
without downloads. The integration tests use real terrain, camera-app, the
gimbal simulator, RTSP and MP4 recordings. The optional `--network` benchmark
writes two rendered PNGs and frame timing statistics while flying over real
terrain; `--a8 --fps 20 --width 1920 --height 1080` exercises a larger view.

The MT11 launcher accepts these environment overrides:

```text
MT11_SITL_WEB_PORT       default 8081
MT11_SITL_CAMERA_PORT    default 37260
MT11_SITL_GIMBAL_PORT    optional fixed port; default is OS-selected
MT11_SITL_RTSP_PORT      default 8554 (native web video uses the next port)
MT11_SITL_MAVLINK_TCP_PORT default 14550; 0 disables TCP
MT11_SITL_MAVLINK_UDP_PORT default 14550; 0 disables UDP
MT11_SITL_ORIENTATION    upright or inverted; default upright
```

The A8 launcher has the equivalent `A8_SITL_WEB_PORT`,
`A8_SITL_CAMERA_PORT`, `A8_SITL_GIMBAL_PORT`, `A8_SITL_RTSP_PORT`,
`A8_SITL_MAVLINK_TCP_PORT`, `A8_SITL_MAVLINK_UDP_PORT` and
`A8_SITL_ORIENTATION` variables. Its MAVLink ports default to zero so a
standalone A8 simulation cannot accidentally accept gimbal targets broadcast
by another ArduPilot instance on port 14550. Set one or both port variables to
14550 when deliberately testing MAVLink integration.

The MT11 MAVLink ports normally come from the persistent simulated
`app/camera.ini`, whose initial values are 14550. Setting either model's
MAVLink launcher variable explicitly overrides that INI value for the process
and for web-triggered restarts; leave MT11 variables unset when changing its
ports in the web UI. A8 always uses its isolated launcher defaults unless the
variables are explicitly set.

For example:

```sh
MT11_SITL_ORIENTATION=inverted MT11_SITL_WEB_PORT=8081 make sitl-run
A8_SITL_ORIENTATION=inverted A8_SITL_WEB_PORT=8081 make a8_sitl-run
```

`gimbal_sim.py` can also be launched independently with `--backend mt11` or
`--backend a8`. `camera-app` selects it with `--uart udp://IPv4:PORT` (with
`localhost` also accepted), or equivalently with `CAMERA_APP_UART`. Physical
operation continues to use each backend's normal UART device.

Test MAVLink system-ID selection and the numeric parameter service with the
actual MAVProxy CLI:

```sh
make sitl-mavlink-test
make a8_sitl-mavlink-test
```

These require `pymavlink`, `MAVProxy` (`mavproxy.py` on PATH), and `pexpect`.
They run camera-app and the gimbal model with isolated temporary files and
ports, check MAVProxy fetch-all/get/set, TCP/UDP addressing, invalid values,
16-character names, and persistence through fixed/automatic ID restarts.
The console transcript, fetched parameter file, telemetry and process logs
are retained in the temporary directory printed by the test. To choose the
log directory, run `python3 sitl/test_mavlink_parameters.py --output PATH`
(with `--backend a8` for A8).

The default automatic system ID waits for a flight controller heartbeat; a
standalone MAVProxy GCS cannot select it. Set **MAVLink system ID** to 1–255
in the web UI and restart camera-app when testing without a flight controller.
The parameter tests supply simulated flight controller heartbeats themselves.
See [the parameter table](../camera_app/README.md#mavlink-camera-and-gimbal-services)
for names, enum values and restart behavior.

Armed recording tests exercise all three automatic recording modes, automatic
and configured system IDs, already-armed startup, unrelated systems/components,
repeated heartbeats, rearming, and finalized MP4 recordings:

```sh
make sitl-armed-recording-test
make a8_sitl-armed-recording-test
```

Video telemetry tests use the real camera application, gimbal model, RTP
packetizer and MP4 muxer with generated H.264/H.265 fixtures:

```sh
make sitl-video-telemetry-test
make a8_sitl-video-telemetry-test
```

They require `pymavlink` and an FFmpeg build with libx264/libx265. The tests
check both RTSP streams for one SEI per access unit, correct RTP timestamps
and marker bits, telemetry changes following MAVLink updates, clean video
decoding, absent/stale sources, and the recorded MP4 frame and telemetry
timestamps. Zoom checks cover 1x, 2x, 4x and 5x, the MT11 wide/tele
crossover, constant thermal FOV, and visible/thermal source swapping. They
compare per-frame FOV with MAVLink stream information and status.
Each run retains media and logs in a printed temporary
directory; `sitl/test_video_telemetry.py --output PATH` chooses its location.

SITL recording now writes real MP4 files from its video fixtures (both MT11
streams; the main A8 stream). The first recorded frame is a keyframe.
Recordings use the visible main-stream dimensions and, on MT11, 1280x720 thermal.
The separate hardware recording-resolution setting is not simulated. Sources
ending in `.h265` or `.hevc` are accepted for RTSP testing; recording those
fixtures is disabled because SITL has no separate H.264 recording encoder.
Hardware continues to use its dedicated H.264 recording channels.

`make recording-recovery-test` runs the real MP4 writer and Files HTTP endpoint
in an isolated runtime. It downloads before close, checks growing immutable
prefixes and fragment write locking, kills the recorder with SIGKILL, and tests
truncation inside the final MP4 boxes. FFmpeg verifies decoded frames and their
telemetry. This simulates interrupted writes, not physical SD-card power loss.

`make -C camera_app test` also blocks the background recording sync deliberately
while writing more frames. It verifies that writes and download locks continue,
sync requests coalesce, close waits for the final flush, and I/O errors propagate.

## SupportProxy integration tests

These tests launch a real SupportProxy with a temporary key database and
isolated ports, alongside camera-app and the gimbal simulator. They require a
built SupportProxy checkout (by default the sibling `../SupportProxy`), its
Python key-database dependencies, `pymavlink`, and FFmpeg with libx264/libx265.

```sh
make sitl-supportproxy-test SUPPORTPROXY_ROOT=/path/to/SupportProxy
make a8_sitl-supportproxy-test SUPPORTPROXY_ROOT=/path/to/SupportProxy
```

The MT11 suite checks signed bidirectional MAVLink, local TCP and UDP links,
camera parameters and advertised proxy video URLs, password and session-based
publishing, H.264/H.265 video decoding, and per-frame telemetry/FOV. It also
tests disabled, video-only and single-video configurations, and reconnection
after a proxy restart while local recording continues. The A8 target runs the
signed MAVLink and two-stream H.264 case. Outputs are retained in the printed
temporary directory; `sitl/test_support_proxy.py --output PATH` chooses it.
The host signing unit test additionally rejects forged, unsigned and replayed
MAVLink packets. The video queue regression test delays the RTSP handshake,
forces both frame-count and byte-count limits, and verifies keyframe recovery
without closing the established connection.

Camera SITL skips network address/route changes. Test the production network
configuration code separately inside a disposable network namespace:

```sh
python3 sitl/test_support_network.py
# If unprivileged network namespaces are unavailable:
python3 sitl/test_support_network.py --sudo
```

This creates a dummy interface, verifies that its existing address survives,
adds the configured address and default route, and checks repeated setup and
invalid settings. The `--sudo` variant requires passwordless sudo for
`unshare`; neither variant changes the host network.

SITL preserves the selected backend's `runtime/app/camera.ini` across builds,
web restarts and launcher stop/start. **Clear parameters on launch** is unchecked
by default. Selecting it restores that camera's template on the next launch,
backs up the previous settings to `camera.ini.reset.bak`, and keeps recordings,
photos and web credentials. Command-line equivalent:
`CAMERA_GIMBAL_SITL_RESET_PARAMETERS=1 make sitl-run` (or `make a8_sitl-run`).

`make sitl-buffering-test` verifies frame cadence with injected 140 ms renderer
stalls and verifies that stopping a full render queue cleans up promptly.

## Shared camera properties

The launcher and MCU simulators read the compiler export of the [target headers](../include/apcam/README.md). MT11, A8, ZR10 and Z1-Mini have their own lens and protocol properties. Build Z1-Mini with `make z1mini_sitl` and start it with `make z1mini_sitl-run`, or select it in the PyQt launcher. Live and recording resolutions are independent, including 1080p live plus 4K recording on Z1-Mini.
