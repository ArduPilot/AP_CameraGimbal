# AP_CameraGimbal

AP_CameraGimbal is an firmware for camera/gimbal devices. It provides
MAVLink Camera Protocol v2, MAVLink Gimbal Protocol v2 and vendor
specific protocol implementations, with camera-specific hardware
isolated behind backends.

The following hardware is currently supported:
 - SIYI MT11
 - SIYI A8-mini
 - SIYI ZR10
 - XFRobot Z1-Mini

Adding support for new hardware is usually fairly straight forward.

The firmware also supports operation as SITL (software in the loop)
for all supported cameras, including:

 - emulation of the gimbal controls
 - emulation of the web interface and camera controls
 - support for synthesised live video based on ESRI satellite data draped over terrain data

Key features of the firmware:
 - integrated comprehsive on-camera web server for configuration and
   monitoring
 - rich set of MAVLink2 camera and gimbal controls
 - support for camera specific features via XML camera definition
   files
 - support for RGB, thermal and zoom lenses
 - support for integrated lidars
 - support for lat/lon/alt targetting within the camera
 - comprehsive CI feature testing
 - support for the ArduPilot SupportProxy for MAVLink and video
   proxying
 - on-camera logging for analysis and diagnosing issues

## Repository layout

- `camera_app/` contains the camera service, reusable protocol and media code,
  backend interface, tests and detailed configuration documentation.
- `camera_app/src/backends/` contains the camera-specific MT11, A8, ZR10 and Z1-Mini backends.
- `web/` contains the shared administration and live-view service.
- `packaging/` and `tools/` contain camera-specific installation and package
  builders.
- `sitl/` contains the host simulator used for local development and CI.
- `tests/` contains end-to-end ArduPilot integration tests.
- `lua/` contains the legacy ArduPilot SIYI control script.

## Quick start

Build and run the host simulation and its integration tests:

```sh
git submodule update --init --recursive
make dependencies
make sitl-test
make sitl-run
# Or exercise the A8 backend in both mounting orientations:
make a8_sitl-test
make a8_sitl-run
```

The submodule provides the MAVLink message definitions and pymavlink; builds
generate the camera application's MAVLink C bindings from them as needed.

The simulation exposes the web interface on `http://127.0.0.1:8081/`, SIYI on
TCP/UDP 37260, MAVLink on TCP/UDP 14550, and RTSP on port 8554. See
[`sitl/README.md`](sitl/README.md) for prerequisites, endpoints and overrides.

Run the camera application's host tests with:

```sh
make -C camera_app test
```

The MT11 target build requires an AArch64 cross compiler, the SS928 V2.0.2.2
B090 MPP tree and minimp4. Their locations can be supplied with
`CROSS_COMPILE`, `SS928_MPP_ROOT` and `MINIMP4_ROOT`:

```sh
make CAMERA_BACKEND=mt11 \
    SS928_MPP_ROOT=/path/to/ss928-mpp \
    MINIMP4_ROOT=/path/to/minimp4
```

See [`camera_app/README.md`](camera_app/README.md) for camera features,
configuration, protocol support and deployment, and [`web/README.md`](web/README.md)
for the administration interface. Pinned external source and license details
are recorded in [`THIRD_PARTY.md`](THIRD_PARTY.md).

## Release packages

Build the hardware installation packages with:

```sh
python3 tools/install_build_environment.py
make release
# Or build just the camera being tested:
make release RELEASE_TARGETS=A8
```

The output is `release/<version>/`, where `<version>` is the latest reachable
`vX.y` tag (currently `v1.0`). Checkpoint tags such as `post-refactor` are ignored.

## Adding camera support

Backends live below `camera_app/src/backends/<camera>/` and implement the
interfaces declared in `camera_app/include/camera_app/backend.h` and
`camera_app/include/camera_app/media.h`. Keep transport-neutral SIYI, MAVLink,
recording and streaming functionality in their existing common directories.

Add the backend name and its target objects to `camera_app/Makefile`, then add
a SITL model and end-to-end tests for public camera and gimbal behaviour. Put
device installers and reviewed platform inputs below `packaging/<camera>/`;
do not add complete vendor update bundles, vendor applications, captures or
reverse-engineering notes.

## MT11 firmware packages

Create a version tag of the form `vX.y`, then run:

```sh
make mt11_package
```

New packages default both the web and root passwords to `ardupilot`. Override
`MT11_WEB_PASSWORD` and `MT11_ROOT_PASSWORD` for a production camera, or set
`MT11_ROOT_PASSWORD_HASH_FILE` to supply a pre-generated root verifier.

With the default dependency paths, this target fetches and verifies the pinned
SS928 MPP and minimp4 sources automatically. It also builds static AArch64
`rsync`, `strace`, `ltrace`, `tcpdump`, Dropbear and `dropbearkey` utilities for
`/app/bin`. Dropbear starts on TCP port 22 and permits root password login;
SSH forwarding is disabled. Set both `SS928_MPP_ROOT` and `MINIMP4_ROOT` to
use existing source trees instead.

## License

AP_CameraGimbal is licensed under the GNU General Public License version 3.
See [`COPYING.txt`](COPYING.txt) for the complete license text. Device-specific
third-party binary inputs are documented separately in
[`THIRD_PARTY.md`](THIRD_PARTY.md).
