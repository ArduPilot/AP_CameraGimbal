# Exposure diagnostics

While BIN logging is active, a background worker samples exposure feedback at
approximately 5 Hz per lens. This follows the existing armed/disarmed logging
policy (`LOG_DISARMED` enables bench logging). No new logging parameter is needed.
The worker is separate from thermal USB polling and joins before the ISP is
closed or reconfigured. It does not change the exposure settings.

Open the numbered SD-card BIN file with MAVExplorer or another DataFlash reader
and graph the `AE` messages, selecting `Lens` when there is more than one sensor.
The self-describing FMT accompanies every log.

| Field | Meaning |
| --- | --- |
| TimeUS | Monotonic sample timestamp, microseconds; cached samples retain their acquisition time |
| Lens | Zero-based target lens: MT11 0=wide RGB, 1=zoom RGB, 2=thermal; other targets 0=RGB |
| Src | 0=hardware ISP, 1=SITL image model |
| Valid | Bitmask below; a clear bit means unavailable, not zero |
| Mode | 0=auto, 1=manual, 2=fixed gain/auto shutter, 3=fixed shutter/auto gain, 255=unknown |
| State | Bit 0=stable, bit 1=at an exposure boundary; only interpret if Valid allows it |
| Result | Zero=successful read, otherwise negative errno or SDK error; partial fields can still be valid |
| US | Actual shutter time in microseconds |
| AG | Analogue gain multiplier; on SigmaStar this is the **combined sensor gain** |
| DG | Sensor digital gain multiplier, where separately available |
| IG | ISP gain multiplier |
| Y | Measured luminance, weighted where available: MT11/AX/SITL 0–255; SigmaStar native AE units |
| Targ | Current AE luminance target, on the same scale as Y, where available |
| Err | Targ−Y on SigmaStar/AX; **raw SDK histogram error on MT11**, not interchangeable |

`Valid`: 1=US, 2=AG, 4=DG, 8=IG, 16=Y, 32=Targ, 64=Err,
128=Mode, 256=stable flag, 512=boundary flag. Unavailable floating-point fields
are NaN. A failed read never reuses old values as a fresh successful sample.

## Hardware support

- **A8 / ZR10:** SigmaStar `MI_ISP_AE_QueryExposureInfo` and
  `MI_ISP_AE_GetExpoMode`. Includes weighted brightness, target, convergence and
  boundary status. The combined sensor gain cannot be separated into AG/DG.
  Y/Targ retain native SDK units: A8 bench feedback reports values around 333,
  so these must not be interpreted as 8-bit pixel luminance.
- **MT11:** reads both RGB ISP pipes through `ss_mpi_isp_query_exposure_info`
  and `ss_mpi_isp_get_exposure_attr`. Includes shutter, separate gains, average
  brightness, histogram error and maximum-exposure status. This SDK does not
  provide a current target or convergence flag. Its boundary flag identifies
  maximum exposure only. Thermal AGC is not RGB AE and is marked unsupported.
- **Z1-Mini:** the native AX capture helper reads `AX_ISP_IQ_GetAeStatus` and
  `AX_ISP_IQ_GetAeParam`. Samples travel over the existing private socket and
  are cached by the camera app. The helper polls continuously at 5 Hz; BIN
  writes still follow the logging policy. Includes shutter, separate gains, weighted
  brightness and target. Convergence/boundary flags are unavailable. Legacy
  vendor RTSP-only capture has no exposure feedback and reports unavailable.

Diagnostics are optional: a missing query symbol must not stop video. SDK errors
are recorded, without generating repeated text-log messages. Z1 cached samples
older than one second are marked unavailable, as are stalled SITL samples.
Initial A8 bench readback has been verified (Auto, stable, approximately 30 ms
shutter and 8x sensor gain). Illumination-response testing is still needed, as is
physical verification on MT11, ZR10 and Z1-Mini. Host tests and cross-compilation
cannot establish that the running ISP responds correctly to illumination changes.

## SITL

The renderer supplies measurements from the same RGB image and gain calculation
used for the outgoing video, for both fixture and 3D terrain sources. US/AG/DG/IG
are the effective simulated settings; Y is sampled at the exposure stage before
brightness/contrast/white-balance processing. The sample identifies the active
RGB lens. Inactive lenses and thermal return unavailable rather than another
lens's measurements.

SITL currently approximates exposure visually; it does **not** implement a
sensor-calibrated, time-dependent AE feedback loop. Consequently it reports
neither a target nor convergence/boundary flags. `Mode` identifies the selected
simulated auto/manual combination. Default simulated exposure is 10 ms at 1x;
EV and metering factors are accounted for in IG. Logs describe the existing
visual model, not a claim of physical AE fidelity.

## Tests

`make -C camera_app exposure-test` checks SigmaStar field layout/unit conversion,
unknown fields, fragmented Z1 exposure/video IPC, invalid packet rejection and
continued exposure polling during blocked thermal I/O.

`make sitl-image-controls-test` changes live image controls on all four targets,
decodes actual BIN files with pymavlink, and verifies the AE source, validity,
units, manual shutter/ISO changes, unsupported thermal feedback and sampling
rate. Both targets run in the firmware CI workflow.
