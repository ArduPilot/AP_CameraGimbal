# RC joystick mount rate regression (issue #3)

[Issue #3](https://github.com/ArduPilot/AP_CameraGimbal/issues/3) reports no
pitch/yaw response when `MNT1_RC_RATE` is nonzero and RC options 213/214 are used.
The current firmware passed this test on a real A8 through an already-running
ArduPlane SITL instance on 2026-09-16. No additional firmware fix was required.

The test sends `RC_CHANNELS_OVERRIDE` to ArduPilot, which runs its real mount
backend and sends MAVLink gimbal rate commands to the camera. It does not send
rate requests directly to the camera to establish success. Vendor feedback is
used to measure actual movement independently of ArduPilot's requested target.

Temporary settings:

- `MNT1_RC_RATE=90`, `RC9_OPTION=213`, `RC10_OPTION=214`.
- RC9/10 range 1000–2000, trim 1500, dead zone zero, no reversal.
- Mount RC targeting mode, with each sample starting near pitch −10°, yaw 0°.
- Deflection ±100 PWM gives ±18°/s. Each deflection lasts 0.65 seconds.

The assertions require movement in the requested direction on both axes,
stopping when centered, and retaining the new angle rather than returning to
the original pointing angle. Bounded travel checks stop the test on unexpected
motion. Changed parameters are restored and channel overrides released on exit.

## Hardware result

The A8 reported inverted mounting and was connected over ArduPilot NET/UDP to
`192.168.144.25:16001`. The test controlled the existing Plane through
`udpin:127.0.0.1:14550`; it did not restart or arm that vehicle.

| Input | Motion during deflection | Drift during centered hold |
| --- | ---: | ---: |
| Pitch positive | +9.6° | 0.0° |
| Pitch negative | −8.7° | 0.0° |
| Yaw positive | +8.9° | −0.3° |
| Yaw negative | −9.9° | +0.1° |

The camera BIN confirmed requested pitch/yaw rates of ±18°/s and zero-rate
stops. Wire commands were ±25 for pitch and +20/−26 for yaw, using the calibrated
A8 conversion. No log records were dropped and no storage errors were reported.

## CI and reproduction

The **ArduPilot camera integration** workflow invokes
`tests/run_ardupilot_rc_mount_test.sh` after building ArduPilot. It covers A8
inverted and MT11 upright over both NET/TCP and NET/UDP. Each case creates
isolated camera/gimbal and ArduCopter processes, uses the same joystick exercise
as the hardware test, and checks the BIN for both signs of both rate commands
and zero-rate stops. Process logs, measurement JSON and BIN logs are uploaded
as CI artifacts, including on failure.

```sh
tests/run_ardupilot_rc_mount_test.sh /path/to/ardupilot
```

For a bench camera already connected to a running ArduPilot instance:

```sh
python3 tests/test_rc_mount_rate.py --host 192.168.144.25 --backend a8 \
    --inverted --master udpin:127.0.0.1:14550 --output /tmp/a8-rc-rate
```

Keep other gimbal control clients idle during the test. Channels 9/10 must be
available for temporary overrides. `LOG_DISARMED=1` on the camera enables a
hardware BIN capture; restore it afterward. CI enables logging automatically.
