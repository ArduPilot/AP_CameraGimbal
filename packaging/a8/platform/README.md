# A8 platform files

These files are the minimal customer-partition platform inputs for the A8 mini
with camera firmware v0.3.7 svn3085 and kernel 4.9.227. They are extracted
byte-for-byte from the supported image, identified by SHA-256
`f0fe8031784c3d623be3b90424510a655ede79be595d9eaa68072213f2dfa60a`.
`SHA256SUMS` records each file and is checked during packaging.

- `modules/4.9.227/`: the complete set of customer modules referenced by the
  retained rootfs boot script, including SD/filesystem and network support.
- `8836_imx678_v6.bin`: the image tuning used by the A8 backend.
- `network_config.ini`: initial Ethernet configuration.

The kernel itself, MI modules, sensor driver and SDK libraries are already in
the camera's retained platform partitions and are not rewritten by our SD
image. No vendor firmware bundle, camera executable, Boa or user-interface
assets are needed during a build. The files here are platform components;
they do not inherit the license of the AP CameraGimbal application.
