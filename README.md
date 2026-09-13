# AP_CameraGimbal

AP_CameraGimbal is an open camera and gimbal service for ArduPilot payloads.
It provides MAVLink Camera Protocol v2, MAVLink Gimbal Protocol v2 and SIYI
protocol endpoints, with camera-specific hardware isolated behind backends.

The Reebot UniPod MT11 and SIYI A8 mini are supported. An experimental SIYI
ZR10 backend and SD application package are available for hardware testing; see
[`packaging/zr10/README.md`](packaging/zr10/README.md) for the build, installation
and current validation limits. The experimental [XFRobot Z1-Mini overlay](packaging/z1mini/README.md)
adds MAVLink gimbal control, web CPU temperature, and H.264 streaming/recording
using the retained vendor AX620A sensor/ISP service. An optional native package
adds separate 4K recording with 1080p live video and embedded telemetry. Build the
base package with
`make z1mini_package Z1MINI_CROSS_COMPILE=/path/to/arm-none-linux-gnueabihf-`.
The app, web server and persistent overlay have passed bench installation and
normal reboot tests; power-cycle recovery and sustained operation remain unvalidated.

More cameras and vendors can be added without duplicating the network protocol, recording, targeting or
configuration layers.

Optional [SupportProxy integration](camera_app/README.md#supportproxy) forwards
MAVLink and both video streams to an ArduPilot SupportProxy instance, with
configurable networking, MAVLink signing and video publishing credentials.

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

Research notes, firmware extractions, protocol captures, hardware probes and
complete vendor firmware bundles belong in the separate porting worktree,
outside this public source tree. Required platform files are kept separately
under `packaging/<camera>/platform/` and `packaging/mt11/base/`.

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
The build uses the current source tree; it does not check out the tag. Each
camera folder contains its firmware, a user-facing `README.md` for first
installation and subsequent updates, `SHA256SUMS`, and `BUILD_INFO.json` with
the source revision and whether tracked files have uncommitted changes:

```text
release/v1.0/
  A8/SIYI_4K_MINI_UpgradeSD.bin
  MT11/MT11_FW_ArduPilot_v1.0_<hash>.bin
  ZR10/ZR10_UpgradeSD.bin
  Z1-Mini/Z1Mini_AP_native_v1.0_<hash>.gcu
```

The SIYI bootloader filenames are ready to copy directly to the top level of
the microSD card. The A8 and ZR10 web updaters accept these same files.
Z1-Mini uses the XFRobot `.gcu` updater; its ArduPilot web firmware upload is
not yet implemented. Follow the README for the particular camera.

On x86_64 Debian/Ubuntu, the environment installer installs system prerequisites
and fetches pinned toolchains and SDKs into `build/`. It writes
`build/environment.mk`, so subsequent top-level Make commands need no shell
setup. Use `--skip-system` when system packages are already installed, or
`--targets a8 zr10` to fetch only selected target toolchains. Camera firmware
bundles are never downloaded or used by the release build.

Run the environment installer once in each fresh clone before building hardware
packages; `make` does not install system packages or the complete toolchain set.
Hardware builds stop immediately with the setup command if the generated
`build/environment.mk` is missing. Host tests, SITL and cleaning remain available
without hardware environment setup.
Top-level builds initialize missing MAVLink submodules and fetch the pinned
MPP/minimp4 sources automatically, including when called through `make release`.
Supplying both `SS928_MPP_ROOT` and `MINIMP4_ROOT` as custom source directories
disables those source downloads.

For manual build setup, see the
[A8](packaging/a8/README.md), [ZR10](packaging/zr10/README.md) and
[Z1-Mini](packaging/z1mini/README.md) packaging documentation. MT11 requires
the cross compiler and MPP dependencies described above, plus the platform
images in `packaging/mt11/base/`. Release builds select the Z1-Mini native
capture package; the environment installer configures `Z1MINI_CROSS_COMPILE`
and `Z1MINI_AX_SDK_INCLUDE`, or these can be set manually.
Platform paths can be overridden with `A8_PLATFORM_DIR` and `ZR10_PLATFORM_DIR`.
`RELEASE_ROOT` overrides
the output directory. Cameras build serially, and a failed camera build keeps
that camera's previously completed release folder intact.

Run `make release-test` to check packaging and failure recovery without the
cross toolchains. Installation guide templates live in `packaging/release/`.
After a release build, `make platform-test` checks the A8/ZR10 images and UUID
helpers. The Camera firmware CI workflow installs the environment, builds all
four targets and publishes the release folders as CI artifacts.

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

The reviewed MT11 kernel, base rootfs and update descriptor are included under
`packaging/mt11/base`, so no vendor firmware download is required. The builder
validates these inputs before use and writes the finished update below
`build/`. See the package section of
[`camera_app/README.md`](camera_app/README.md) before installing an update on
hardware. Change the default credentials before exposing a camera to an
untrusted network.

## License

AP_CameraGimbal is licensed under the GNU General Public License version 3.
See [`COPYING.txt`](COPYING.txt) for the complete license text. Device-specific
third-party binary inputs are documented separately in
[`THIRD_PARTY.md`](THIRD_PARTY.md).
