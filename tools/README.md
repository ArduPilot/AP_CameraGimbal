# Tools

- `build_camera_definitions.py` exports each target's built-in camera definition
  XML using the host compiler. `make camera-definitions` writes these to
  `build/camera-definitions/`; the firmware serves the same XML over MAVFTP.

- `install_build_environment.py` installs x86_64 Debian/Ubuntu hardware-build
  prerequisites and fetches pinned A8/ZR10/Z1-Mini compilers and SDKs. It creates
  an isolated Python environment and `build/environment.mk` for top-level Make.
  Use `--skip-system` to leave system packages alone, or `--targets` to select
  cameras. It never downloads camera firmware bundles.
- `bootstrap_dependencies.sh` downloads and verifies the pinned public SS928
  MPP sample tree and minimp4 header used by target and SITL builds.
- `build_mt11_tools.sh` downloads, verifies and cross-compiles static AArch64
  `rsync`, `strace`, `ltrace`, `tcpdump`, Dropbear and `dropbearkey` binaries for
  `/app/bin` in MT11 packages.
- `build_mt11_package.sh` creates an MT11 application update from the supported
  in-repository kernel, rootfs and update descriptor. It is normally invoked
  through `make mt11_package`; web and root passwords default to `ardupilot`
  and can be overridden.
- `repack-root-password.sh` rebuilds the MT11 root filesystem with a supplied
  SHA-512 crypt verifier, `MT11_ROOT_PASSWORD`, or an interactively entered
  password. The package builder invokes it with the configured project value.
- `build_a8_package.sh` creates the SIYI A8 mini SD update (`make a8_package`);
  its first argument is the checked-in platform directory. See `packaging/a8/README.md`.
- `build_zr10_firmware.sh` combines the checked-in ZR10 platform directory with
  the AP application archive, UUID helper and startup scripts to make an SD image.
- `video_telemetry.py INPUT --output telemetry.jsonl` extracts per-frame
  position/attitude SEI metadata from H.264/H.265 files, MP4 recordings or RTSP
  streams. Container/RTSP inputs require FFmpeg; see the
  [schema and timing notes](../camera_app/README.md#video-telemetry-metadata).
- `temp_to_png.py` converts an MT11 640x512 little-endian radiometric `_I.bin`
  capture to a viewable PNG.

These scripts do not download a vendor firmware bundle. Reviewed immutable
MT11 platform images used by the package builder are under
`packaging/mt11/base`.

Native GDB is intentionally not part of the MT11 overlay. A stripped static
GDB 17.2 build occupied 10.2 MiB (4.1 MiB compressed), which is too costly on
the camera's 140 MiB `/app` volume.
