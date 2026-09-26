# UniGCS / private SIYI protocol

The shared UniGCS service is built for MT11, A8 mini and ZR10, on hardware
and in SITL. It provides discovery, a private TCP session and camera controls.
It is **not yet full UniGCS feature parity**. The simulator emulates startup
queries, pan/tilt rates and four preset movements. Native mode, rangefinder
and telemetry subscription commands still need decoding and tests.

MT11 forwards private v3 gimbal frames unchanged to its MCU. A8/ZR10 convert
the v3 network frames to their v2 MCU framing and convert replies back to v3,
preserving source/destination IDs, sequence, link, command and payload. Their
MCU payload limit is 255 bytes; longer requests are rejected. Camera replies
use network address 34 for the v3 endpoint queried by UniGCS, independently
of the A8/ZR10 MCU transport's 2C address. Product IDs are MT11: 89, A8: 72,
ZR10: 6B (hexadecimal; the ZR10 identity still needs vendor confirmation).
The network service accepts v3 framing and the older `55 66 AA BB` format
selected by UniGCS for A8. Each TCP session keeps the format of its first
valid request, and replies and heartbeats use that format. MCU UART parsers
do not accept the older network framing.

The A8 scan capture shows UniGCS querying `34/F0/01` and ignoring our initial
`2C/F0/01` reply with product 73. Discovery was confirmed in UniGCS after
changing the source to 34 and the camera product ID to 72.
Stock A8 `cardv` v0.3.7's `camera_sdk_get_ver_action` at 0x62370 returns
`07 03 00 72`, confirming that 72 is the camera's version product ID; 73 is
the gimbal identity.
The inspected `cardv` SHA256 is
`5c33c92acf12b2145327f6a86082110c5b9c4fcdf0ac52a2a29a419580737f50`.

Real A8 testing also found an Ethernet receive-filter issue: unicast discovery
worked, but multicast discovery failed despite the correct `224.0.0.1`
membership and `01:00:5e:00:00:01` MAC entry. Enabling `ALLMULTI` immediately
restored multicast replies. Startup scripts for all three SIYI cameras now
enable it on `eth0` so discovery does not depend on vendor multicast filters.
The receive-filter failure and fix are hardware-verified on A8; MT11 and
ZR10 still need that hardware check. SITL does not change host interface flags.
The service retries multicast membership every five seconds to handle interfaces
that become ready after startup or are recreated. Discovery uses the interface
MAC on Linux, Cygwin and BSD; SITL adapters without one use a stable locally
administered address derived from their IPv4 address.

MT11 and A8 SITL H.265 display have been confirmed in the UniGCS GUI.
Discovery, connection and H.265 display are also confirmed on the real A8
after enabling multicast reception. All three models have isolated
protocol/video tests; ZR10 GUI interoperability and the remaining hardware
controls still require validation. Their shared camera handlers use the
MT11 capture as a starting point, not an assertion that all vendor command
layouts match.

The older network header consists of four magic bytes, flags (request 1,
reply 2), a 32-bit little-endian payload length, 16-bit sequence, command
byte, and a 32-bit header checksum. Payload starts at offset 16 and is
followed by a second 32-bit checksum. Both checksums use polynomial 04C11DB7,
initial zero, MSB first, no final XOR, stored little-endian. The first covers
bytes 0–11; the second covers the complete header and payload. Captured A8
status request: `5566aabb01000000000000802d977a34b7ad40eb`.
Old-format pan/tilt 9A and preset 9B are translated to native gimbal link 11;
other decoded camera commands share the camera handlers. Unimplemented A8
version/configuration and image-control variations are logged, not treated
as implemented merely because their frame checksum is valid.

## Running and testing

Build with `make sitl`, `make a8_sitl` or `make zr10_sitl`, then launch the
selected model as the first SITL instance.
UniGCS must discover the **SITL host's LAN address**, not the physical stock
camera at 192.168.144.25. Allow UDP 37258, TCP 37256 and TCP 8554 through the
host firewall. Video uses the existing `/video1` RTSP service. For UniGCS,
select H.265 for the main stream (`codec = h265` in `[stream.main]`). This is
the configuration confirmed to display video in the tested UniGCS client.
The server also supports H.264, but that client remained blank with H.264
even after the RTCP fix; the cause is not yet established.

Run `python3 sitl/test_unigcs.py build/sitl/camera-app` for an isolated test of
wire framing, discovery, startup, gimbal routing, reconnects, camera operations,
persistence and coexistence with the public SDK. It starts its own simulated
MCU and camera on temporary ports and does not access the physical camera.
Use `--backend a8` with `build/a8-sitl/camera-app`, or `--backend zr10` with
`build/zr10-sitl/camera-app`, for the other SIYI cameras. These tests also run
in CI and under the corresponding `sitl-test`, `a8_sitl-test` and
`zr10_sitl-test` make targets.
Add `--video-codec h264` or `--video-codec h265` to include live video tests.
These validate decoder parameters in SDP before PLAY, RTSP aliases, replies
on two local addresses, FFmpeg decoding, and initial and periodic RTCP sender
reports against the received RTP timestamps, SSRC and counters. CI runs both
codecs. The encoder is initially paused to check that DESCRIBE and PLAY succeed
before any parameter sets exist, and that the first frame carries those sets.
Run `sudo unshare -n python3 sitl/test_unigcs.py build/a8-sitl/camera-app --backend a8 --late-interface`
to check discovery on an interface created after startup and then replaced.
This test creates interfaces only inside the isolated network namespace.

The protocol test also restarts the UDP gimbal simulator while the camera is
running and verifies that native commands work again after it returns. SITL
consumes transient UDP port-unreachable errors so a simulator restart does
not terminate the camera and video services.

RTSP descriptions include H.264 SPS/PPS and packetization mode 1, or H.265
VPS/SPS/PPS, taken from the encoder. Parameter sets are collected even without
viewers, and aliases share them. SDP is regenerated for each DESCRIBE, avoiding
stale codec headers or an address cached from another interface.
Before the encoder supplies its first parameter sets, DESCRIBE returns the
basic codec description and clients obtain headers from the stream. Later
DESCRIBEs include the complete decoder configuration.

The server sends compound RTCP sender reports and SDES/CNAME on the negotiated
TCP channel or UDP port, initially and every five seconds while streaming.
Reports pair the wall clock with the transmitted RTP timestamp and count the
packets and payload bytes sent to that client. The stock camera also sends
these reports. UniGCS video display was confirmed with H.265 after adding
the reports; the same stream had remained blank without them. Automated
decoding and transport checks pass for both H.264 and H.265. The subsequent
H.264 test still produced no picture in UniGCS despite correct H.264 codec
advertisement in both private command `16/83` and SDP. H.265 was restored for
continued testing. This does not establish full control compatibility or
hardware parity.

The private service is enabled for all three SIYI models using the standard
SDK port 37260.
Other SDK ports leave it disabled, avoiding collisions between cameras on the
same SITL host. `CAMERA_APP_UNIGCS_PORT` explicitly enables/overrides the TCP
listener; `CAMERA_APP_DISCOVERY_PORT` overrides UDP discovery. These overrides
are useful for isolated tests; UniGCS itself expects the standard ports.
Only one private TCP client is accepted at a time: MCU replies do not echo
request sequence numbers, so clients sharing source D0 cannot be safely
multiplexed. Five seconds without a valid request releases the session.

If UDP 37258 is occupied, the application leaves discovery to the existing
owner and logs this. On physical stock-derived MT11 systems, `product_upgrade`
usually owns that socket. Our application still provides TCP 37256. Kernel,
updater and gimbal firmware are not replaced by this change.
TCP 37256 must be available: a bind failure stops camera-app with an error,
as it does for the public SIYI listener. This prevents a competing process from
silently owning private controls while our application serves the camera.

## Time synchronisation

Stock firmware requests time from its connected TCP client using command `91`
with one-byte payload `01`, while its time-initialised flag is clear. On MT11
this is camera link `16`. The misleadingly named `get_utc_time_info_action`
receives calendar time and sets the system clock; it is not a query of the
camera's current time. Our private service requests time once per second after
the first valid client command establishes its routing ID and wire format,
while the camera date is earlier than 1 September 2026 UTC. It supports both
MT11 v3 and A8/ZR10 legacy TCP framing. Replies must come from the same client
and address the camera's time command while a request is outstanding.

The service accepts packed seven-byte or padded eight-byte calendar payloads,
with response, request or no-ACK control flags, without replying to the time
reply. Dates are interpreted as UTC independently of the configured display
timezone. Invalid dates, dates before the validity threshold, years after
2099 and dates the platform's `time_t` cannot represent are rejected. Clock
setting errors are logged and retried. Once the clock is valid, including if
MAVLink or public SDK `30` sets it first, private requests stop and delayed
replies cannot step the clock. Existing files are not renamed or retimestamped.

This is confirmed by stock executable analysis: MT11 V1.0.5
`get_utc_time_ontick` at `0x5f21f0` sends the request; handler `0x63d680` copies
the received data and calls `update_utc_time` at `0x5f2100`, which calls
`mktime` and `settimeofday`. A8 `cardv` v0.3.7 has the same sequence at
`0x52408` and `0x522b4`; its TCP dispatch table at `0xe39d0` maps command `91`
to wrapper `0x65575`. Calendar fields are little-endian uint16 year followed
by month, day, hour, minute and second bytes. A8 copies eight payload bytes,
but uses only the first seven for the calendar. The eighth byte is ignored.
The implementation is tested with both wire formats and simulated clocks;
a real UniGCS exchange still needs capture confirmation, including UTC semantics.
Run `make -C camera_app unigcs-time-test` to check the handshake, malformed
dates, leap years, non-UTC camera timezones, retries and existing clock protection
without altering the host clock.

The stock MT11 capture in `analysis/MT11/unigcs-20260925/stock-camera.pcap0`
does not contain that private exchange: packet 1691 already sets its clock
through public SDK UDP 37260, command `30`, from `192.168.144.15:35136`.
Its payload `81f79408445c0600` is epoch microseconds `1790297131775873`
(`2026-09-25 00:45:31.775873 UTC`). Packet 1692 contains the success reply
`30/01` and has corrected capture time `2026-09-25 00:45:31.001678 UTC`.
UniGCS on `.69` connects about two minutes later, after time was initialised.
Our A8/MT11 firmware also supports public SDK `30` and MAVLink `SYSTEM_TIME`;
the private request/response provides a clock source when using UniGCS alone.

## Confirmed transport

Captured using stock MT11 V1.0.5 on 2026-09-25:

- UniGCS `.69:37258` sends discovery to `224.0.0.1:37258`.
- Camera `.25:37258` replies unicast with a 19-byte identity payload:
  device ID 0x89; IPv4; netmask; broadcast; six-byte MAC address.
- UniGCS connects to camera TCP 37256 using private v3 framing.
- Client ID is D0, camera ID 34, gimbal ID 2E. Camera link is 16, gimbal
  link 11, discovery link F0. Numbers here are hexadecimal.
- The camera emits `16/F0` payload `01` once per second. UniGCS polls `16/80`
  recording status about once per second, also keeping the stock session alive.
- A bounded UART trace with a read-only `11/A0` request confirmed that the
  stock app forwards the entire private network frame unchanged to UART3,
  and forwards the MCU's reply unchanged back to TCP.

```
AA | control | 03 | payload_length_u16_le | CRC8 |
sequence_u16_le | source | destination | link | command |
payload | CRC16_u16_le
```

CRC8/MAXIM covers the first five bytes; CRC16/CCITT with initial zero covers
all bytes before the final checksum. Requests observed use control 09 (ACK
requested), and responses use 0A. Control 08 requests are also accepted.
The TCP parser handles fragmentation, concatenation and corrupt-frame recovery.
A bad header CRC is rejected before waiting for its advertised payload length.
Network/private UART frames are included in the existing packet BIN logging,
including source and destination IDs in their original wire headers.

A8 and ZR10 report a single RGB sensor and do not expose thermal controls or
an MT11 second lens. Zoom limits come from each target's supported control
range. MT11/A8 use stepped absolute zoom; ZR10 uses its native rate control,
with an explicit stop on release or disconnect and asynchronous MCU feedback.
Image-setting writes fail on targets without image-control support (ZR10).

## Native gimbal controls in SITL

Link `11`, destination `2E`:

| Command | Payload | Behaviour |
| --- | --- | --- |
| `9A` | Signed yaw and pitch bytes, each -100 to 100 | Same rate control as public SDK `07`; zero stops motion |
| `9B` | `01` | Centre yaw and pitch |
| `9B` | `02` | Centre yaw and look down |
| `9B` | `03` | Centre yaw, preserving pitch |
| `9B` | `04` | Look down, preserving yaw |

Valid motion requests return one-byte `01` when an acknowledgement is
requested. Invalid preset IDs return `00`; malformed lengths are ignored.
Control `08` applies the command without an acknowledgement. Motion shares
the simulator's public SDK state and motor response. Presets target a physical
downward pitch of -90 degrees, clamped to the simulated gimbal's limits.

The UniGCS capture on 2026-09-25 contains all four `9B` values. The stock
MCU image (`5c78d9cc854ba9ff6f49697c761367cc8d447976f451fe727ed408d306c5b038`)
dispatch table at file offset `0x3BAA0` maps `9A`/`9B` to wrappers at
`0x80037A04`/`0x80037A24`, then handlers at `0x80039810`/`0x80039994`.
The former shares the public SDK rate handler; the latter calls ILM routines
`0x6D94`, `0x6DB0`, `0x6E04`, `0x6E1C` for the four presets. The startup
copy maps file `0x3CE10` to ILM `0xEE0`. These establish payload semantics
independently of the simulator tests. Physical-camera commands continue to
be forwarded to the MCU, with the framing conversion described above.

## Camera command coverage

Settings use existing media/backend operations. Image controls, palette,
and automatic recording persist through the shared parameter
metadata/INI machinery. Other live controls retain their existing persistence
semantics. Unsupported operations are logged once per command and are not
acknowledged as successful. AI availability is reported as off. The startup
catalogue query returns zero classes and an empty name list, so a client can
complete the query without being told that stock tracking models are present.

`16/94` currently advertises the captured 1.0.12 compatibility version and
the selected model's device ID, not the ArduPilot build version. `16/E1` preserves the stock
V1.0.5 layout, including its duplicate ISO byte. Encoder queries report current
sizes/codecs/frame rates and the MT11 encoder's nominal bitrate policy; the
SITL renderer does not use that hardware bitrate policy. Encoder *setters* are
not enabled yet. RGB/thermal selection supports the implemented single-image
modes, not compositing or independent secondary sensor selection.

The table below comes from the stock executable dispatch table, not guessed
opcode correspondence with the public SDK. Stock SHA-256:
`bf6a0440e83c9ab3cd666474d3cc37afbda0140073699849db73b1b05e8f4fa3`.
`get_mini4_tcp_server_camera_action_func` at 0x638130 searches 73 ten-byte
entries at 0x19db1d0. Each entry maps link/command to a wrapper calling the
named handler. These mappings alone do not establish complete payload layouts.

| Link/command | Stock handler suffix | Implementation |
| --- | --- | --- |
| `f0/3b` | `reset_dev_param_action` | Not implemented |
| `f0/07` | `shell_action` | Not implemented |
| `16/f0` | `get_local_info_action` | Heartbeat; client location payload accepted but not used |
| `16/94` | `get_camera_ver_action` | Compatibility version / MT11 identification |
| `16/91` | `get_utc_time_info_action` | Receives validated UTC calendar time following camera's `91/01` request |
| `16/74` | `get_ip_action` | Pending payload/backend implementation |
| `16/75` | `set_ip_action` | Pending payload/backend implementation |
| `16/eb` | `zoom_switch_mode_action` | Pending payload/backend implementation |
| `16/ec` | `zoom_control_action` | Pending payload/backend implementation |
| `16/ef` | `zoom_focus_test_action` | Pending payload/backend implementation |
| `16/b2` | `dehaze_type_action` | Pending payload/backend implementation |
| `16/e1` | `get_isp_all_param_action` | Image settings query |
| `16/e3` | `set_isp_one_param_action` | Validated, persistent image parameter setting |
| `16/e5` | `set_get_isp_mode_action` | Exposure mode query only |
| `16/83` | `get_encode_param_action` | Encoder query: 0 recording, 1 main RTSP, 2 secondary RTSP |
| `16/84` | `set_encode_param_action` | Pending payload/backend implementation |
| `16/92` | `get_vdieo_splice_action` | Image selection query |
| `16/93` | `set_vdieo_splice_action` | Single RGB/thermal selection; no picture-in-picture |
| `16/ca` | `dis_stat_action` | Pending payload/backend implementation |
| `16/c9` | `hnr_stat_action` | Pending payload/backend implementation |
| `16/a7` | `set_ircut_mode_action` | Pending payload/backend implementation |
| `16/a8` | `get_ircut_mode_action` | Pending payload/backend implementation |
| `16/9c` | `night_led_stat_action` | Pending payload/backend implementation |
| `16/c8` | `night_frame_rate` | Pending payload/backend implementation |
| `16/85` | `get_auto_record_sta_action` | Automatic recording query |
| `16/86` | `set_auto_record_sta_action` | Automatic recording setting (reply is 85) |
| `16/80` | `get_sd_record_status_action` | Recording status and elapsed seconds |
| `16/81` | `set_sd_record_action` | Start/stop recording |
| `16/82` | `sd_format_action` | Pending payload/backend implementation |
| `16/c6` | `capture_action` | Photo capture; accepts UniGCS payload 01 and empty requests |
| `16/fe` | `advance_capture_action` | Pending payload/backend implementation |
| `16/b3` | `set_capture_screen` | Pending per-sensor capture enable support; two-byte sensor/enable request and B3 echo |
| `16/b4` | `get_capture_screen` | Photo scope query |
| `16/c2` | `set_record_screen` | Pending payload/backend implementation |
| `16/c3` | `get_record_screen` | Pending payload/backend implementation |
| `16/b1` | `capture_resolution` | Pending payload/backend implementation |
| `16/ed` | `get_sd_information_action` | Pending payload/backend implementation |
| `16/bd` | `set_thermal_gain_mode_action` | Thermal gain selection |
| `16/bc` | `get_thermal_gain_mode_action` | Thermal gain query |
| `16/8a` | `get_thermal_point_info_action` | Pending payload/backend implementation |
| `16/8b` | `get_thermal_rect_temp_action` | Pending payload/backend implementation |
| `16/a4` | `get_thermal_pseudo_color_action` | Thermal palette query |
| `16/a5` | `set_thermal_pseudo_color_action` | Persistent thermal palette selection |
| `16/cf` | `set_thermal_shutter_action` | Pending payload/backend implementation |
| `16/cb` | `get_thermal_zoom_mode_action` | Pending payload/backend implementation |
| `16/cc` | `set_thermal_zoom_mode_action` | Pending payload/backend implementation |
| `16/be` | `get_thermal_envcorrect_action` | Pending payload/backend implementation |
| `16/bf` | `set_thermal_envcorrect_action` | Pending payload/backend implementation |
| `16/e9` | `get_thermal_envcorrect_param_action` | Pending payload/backend implementation |
| `16/ea` | `set_thermal_envcorrect_param_action` | Pending payload/backend implementation |
| `16/8d` | `set_thermal_temp_frame_action` | Pending payload/backend implementation |
| `16/8c` | `get_thermal_temp_frame_action` | Pending payload/backend implementation |
| `16/e6` | `start_thermal_calibration_action` | Pending payload/backend implementation |
| `16/e7` | `get_thermal_calib_param_action` | Pending payload/backend implementation |
| `16/e8` | `set_thermal_calib_param_action` | Pending payload/backend implementation |
| `16/a0` | `get_set_ir_sr_state_action` | Pending payload/backend implementation |
| `16/a2` | `get_ai_target_tracking_switch_action` | Reports AI tracking unavailable/off |
| `16/a3` | `set_ai_target_tracking_switch_action` | Pending payload/backend implementation |
| `16/aa` | `select_ai_target_tarcking_action` | Pending payload/backend implementation |
| `16/ab` | `ai_detect_data_action` | Pending payload/backend implementation |
| `16/ac` | `cancel_ai_target_tracking_action` | Pending payload/backend implementation |
| `16/ad` | `set_get_ai_threshold_action` | Pending payload/backend implementation |
| `16/a9` | `get_set_ai_model_id_action` | Pending payload/backend implementation |
| `16/af` | `get_set_plate_rec_mode_action` | Pending payload/backend implementation |
| `16/ae` | `load_or_unload_model_action` | Pending payload/backend implementation |
| `16/d5` | `get_set_object_count_or_mask_action` | Query 03 returns an empty AI catalogue; setters pending |
| `16/f1` | `ai_model_manage_action` | Pending payload/backend implementation |
| `16/98` | `zoom_action` | Continuous zoom, stopped by zero or disconnect |
| `16/97` | `auto_focus_action` | Point autofocus (1920x1080 coordinates) |
| `16/99` | `manual_focus_action` | Manual focus, stopped by zero or disconnect |
| `16/d0` | `get_zoom_range_action` | Supported visible zoom range (1–10x) |
| `16/d1` | `get_zoom_value_action` | Visible zoom query |
| `16/d2` | `set_zoom_value_action` | Absolute visible zoom |

## Remaining work before full GUI acceptance

- Decode remaining native link-11 modes, rangefinder and telemetry subscriptions;
  add stateful simulator handling with captured command/reply fixtures.
- Verify the complete UniGCS GUI startup and exercise each implemented control.
- Implement encoder setters, remaining image/composition controls, temperature
  point/rectangle requests, thermal shutter/calibration and storage operations.
- Implement the underlying AI capabilities before exposing tracking/model
  controls; protocol replies alone cannot provide these functions.
- Capture media browsing/download, network configuration and any additional
  services used by those GUI screens. The initial session only exercised
  discovery, private control and RTSP.

The stock MT11 remains available for read-only queries and controlled captures.
Testing the new application on hardware comes after the initial SITL GUI pass.
