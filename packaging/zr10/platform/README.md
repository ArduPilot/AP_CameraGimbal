# ZR10 platform files

These customer-partition inputs support ZR10 Infinity6B0 / GC4663 with camera
firmware v0.3.9 svn2499, distributed in the SIYI v0.4.3 bundle. They were
extracted byte-for-byte from the image identified by SHA-256
`c8fb622301f50d3c69469bbd97f7049c3ba2942a9f952d22af57388f298554c9`.
`SHA256SUMS` records each file and is checked during packaging.

- `gc4663_MIPI.ko`: the sensor kernel module, loaded with `chmap=1`.
- `day_gc4663-2-aec-AWB.bin`: factory image tuning used by the backend.
- `network_config.ini`: initial Ethernet configuration.

The kernel, `/config` MI modules and rootfs SDK libraries remain installed on
the camera. Our startup script and UUID helper are built from project sources.
No vendor firmware bundle, camera executable, Boa or user-interface assets
are needed during a build. These are platform components with their own
licenses, separate from the AP CameraGimbal application.
