# TODO

- Make parameter changes take effect immediately where possible.
- Re-calibrate thermal.
- Implement camera definition files.

## A8 mini backend

- Gimbal link follow-ups: the vendor app sends a 1 Hz status heartbeat
  (sub-command 0x3f) and 25 Hz per-frame data (0x02) to the gimbal MCU that
  the backend does not send yet; check whether anything (LED, motor
  behaviour) depends on them. Sub-command 0x14 is still undecoded.
