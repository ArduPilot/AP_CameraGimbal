# TODO

- Re-calibrate thermal.
- Allow configuring the camera's main IP address, a secondary IP address,
  and the default route.
- Check compliance with the [MAVLink camera protocol](https://mavlink.io/en/services/camera.html).
- Ensure `cam_definition_version` changes whenever the camera definition XML
  changes; consider deriving it from a 16-bit hash of the XML.
- Investigate jerky MT11 video during bench testing.

## A8 mini backend

- Gimbal link follow-ups: the vendor app sends a 1 Hz status heartbeat
  (sub-command 0x3f) and 25 Hz per-frame data (0x02) to the gimbal MCU that
  the backend does not send yet; check whether anything (LED, motor
  behaviour) depends on them. Sub-command 0x14 is still undecoded.
