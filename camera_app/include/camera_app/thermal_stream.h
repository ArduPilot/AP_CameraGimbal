#pragma once
#include <stdint.h>
#include <time.h>
#include "camera_app/config.h"

// Experimental discovery value; not an upstream MAVLink enum allocation.
#define CA_VIDEO_STREAM_TYPE_HTTP_MATROSKA 200U
#define CA_RAW_THERMAL_STREAM_ID 3U
struct ca_thermal_stream;
int ca_thermal_stream_open(ca_thermal_stream **result, unsigned rtsp_port, bool simulated,
                           const ca_config *settings);
int ca_thermal_stream_configure(ca_thermal_stream *, const ca_config *settings);
// The video backend owns the session and supplies its existing recording path.
int ca_thermal_stream_recording(ca_thermal_stream *, bool active, const char *video_path);
void ca_thermal_stream_publish(ca_thermal_stream *, const uint16_t *pixels,
                               const timespec *captured_at, uint8_t gain, bool rotated_180);
void ca_thermal_stream_publish_terrain(ca_thermal_stream *, const uint16_t *pixels,
                                      uint64_t capture_us, const char *telemetry, uint8_t gain, float hfov);
void ca_thermal_stream_close(ca_thermal_stream *);
unsigned ca_thermal_stream_port();
bool ca_thermal_stream_available();
unsigned ca_thermal_stream_fps();
bool ca_thermal_stream_enabled();
void ca_thermal_stream_enable(bool enabled);
// Deterministic full-depth fixture shared by SITL and codec validation.
void ca_thermal_test_pattern(uint16_t *pixels, uint64_t frame_id);
