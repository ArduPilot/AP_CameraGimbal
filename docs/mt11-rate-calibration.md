# MT11 rate response and calibration

## Isolated upright bench sweeps, 2026-09-15

The raw SIYI rate command is a signed integer from -100 to +100. Tests on the
stationary MT11 measured both directions, first near the low-speed threshold,
then through full command. Each sample started near yaw zero, pitch -45 degrees.
The threshold region was repeated in descending magnitude order at -20 degrees
pitch. The MCU reported mounting direction 1 (upright).

| Raw command magnitude | Pitch speed, deg/s | Yaw speed, deg/s |
| --- | --- | --- |
| 1 through 5 | Approximately zero | Approximately zero |
| 6 | +6.05 / -5.96 | +5.42 / -6.42 |
| 7 | +7.04 / -7.03 | +6.64 / -6.77 |
| 10 | +10.12 / -10.09 | +9.47 / -10.11 |
| 20 | +20.07 / -20.05 | +19.51 / -20.20 |
| 40 | +39.96 / -40.19 | +40.91 / -39.71 |
| 60 | +60.68 / -60.47 | +61.28 / -58.83 |
| 80 | +79.98 / -79.95 | +82.84 / -78.21 |
| 100 | +99.87 / -100.33 | +103.50 / -97.64 |

Motion was measured from angle changes, not reported gyro rates. Those gyro
axes are not Euler yaw/pitch derivatives, especially when looking steeply down.
The table uses a straight-line fit after 0.5 seconds for small commands and
0.15 seconds for larger commands. High-speed samples are short because angular
travel is limited. These are individual runs, not statistical confidence bounds.
Yaw feedback is noisier and its observed directional asymmetry needs more
measurement before encoding a more elaborate curve.

The useful first approximation for **both axes** is therefore:

* Commands -5 through +5: no motion.
* Larger magnitudes: approximately one degree/second per command unit.
* A finite response time: pitch onset extrapolates to roughly 40–50 ms after
  sending the command. At command 100, several more degrees of motion occur
  after sending zero before settling. Yaw also shows delayed onset/stopping.

The simulator uses independent signed `APCAM_SIM_PITCH_RATE_CURVE` and
`APCAM_SIM_YAW_RATE_CURVE` tables in `target_mt11.h`, with a 40 ms first-order
rate response. It integrates motion through the response and deceleration,
including when a zero command arrives. This is an approximation of the combined
command/actuator response: transport and feedback latency have not been separated
from motor dynamics. Signed tables permit later asymmetric calibration. Other
targets retain their existing linear, immediate response.

Firmware requested-rate conversion is separate from the simulated physical
response. Both MT11 axes now use 100 deg/s per full-scale command. The old pitch
conversion assumed 60 and consequently commanded about 67% too much speed.
A requested speed below the hardware minimum remains unachievable as a constant
rate command. The realistic simulator exposes roughly 2.4 degrees of following
error in the 4 deg/s ROI test; the previous ideal model hid that effect. The
regression bound is 3 degrees, while still checking that no absolute angle
commands are sent. Low-speed compensation remains separate controller work.

After installing the corrected firmware, MAVLink +/-10 deg/s requests measured
+10.10/-10.09 deg/s pitch and +9.40/-9.91 deg/s yaw. This verifies the complete
MAVLink-to-vendor conversion path, not just raw-command behaviour.

## Comparison with ZT30

MAVProxy's `mavproxy_SIYI.rate_mapping()` uses a measured ZT30 table with commands
4/5/6/7/10 corresponding to 0/1.75/3/4.7/9 deg/s and command 70 corresponding to
98 deg/s. It interpolates the inverse map and quantises to an integer command.
That curve should not be copied onto MT11: its dead zone, minimum moving speed
and full-scale response differ substantially.

## Earlier overshoot fixes

MT11 yaw feedback wraps at +/-180 degrees while the gimbal rotates continuously.
Treating the seam as a bounded joint caused nearly 360-degree errors and large
spurious reversals. `APCAM_GIMBAL_YAW_CONTINUOUS` enables circular error handling
for MT11 while preserving bounded-joint handling on other targets. SITL tests
both seam directions, including a 60% plant gain error.

The original proportional error filter delayed braking. Its time constant was
reduced from 0.2 to 0.05 seconds, proportional gain from 2 to 1.2, and the MT11
yaw command conversion was corrected. The subsequent hardware tracking sample
had about 1 degree RMS yaw error after acquisition and no large seam-induced
reversals. The isolated sweeps then confirmed the pitch scale and both dead zones.

## Reproducing the measurements

Use `tools/mt11_rate_sweep.py` on a stationary bench, one axis at a time. Pause or
disconnect the flight controller during raw-command sweeps. ArduPlane switches
to streaming angle commands when camera-side geographic targeting is disabled;
even a fixed mount target can override the sweep. Check the BIN for competing
GCMD records. The successful isolated run in `00000006.BIN` had none and reported
zero dropped records and zero storage errors.

The tool temporarily suspends geographic tracking, enables disarmed BIN logging,
recentres before each sample, sends raw SIYI bytes and records CSV feedback. It
records feedback after each stop, checks travel and feedback timeouts, and
restores the previous parameters on exit. The MCU generates its own reply
sequence numbers, so queued feedback is discarded before polling rather than
matching request and response sequences. Absolute yaw may settle several degrees
off zero; the tool accepts a stable central pose and measures relative travel.

```sh
python3 tools/mt11_rate_sweep.py --host 192.168.144.25 --axis pitch --output pitch.csv
python3 tools/mt11_rate_sweep.py --host 192.168.144.25 --axis yaw --output yaw.csv
python3 tools/mt11_rate_sweep.py --host 192.168.144.25 --axis pitch --commands 4,5,6,7,10,12 --reverse --pitch -20 --output pitch-repeat.csv
```

Follow with `--commands 16,20,30,40,60,80,100`. Inspect actual settled intervals:
high speeds reach the travel limit before the requested hold duration. The CSV
`phase` distinguishes running from stopping; stopping records have command zero.

Download numbered BIN files through **Files → logs**, then run:

```sh
python3 tools/analyze_gimbal_rates.py flight.BIN --settle 0.15 --window 0.2 --samples windows.csv > response.csv
```

Remaining calibration: physically inverted mounting, repeatability across
headings/temperature, continuous ascending/descending commands without recentering
to measure hysteresis, and separation of transport delay from actuator dynamics.
The recentered ascending/descending sweeps do not establish absence of hysteresis.
