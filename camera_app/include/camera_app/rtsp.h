#ifndef CAMERA_APP_RTSP_H
#define CAMERA_APP_RTSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "camera_app/config.h"

struct ca_rtsp;

#ifdef __cplusplus
extern "C" {
#endif

int ca_rtsp_open(struct ca_rtsp **server, unsigned port, const char *path,
                 enum ca_video_codec codec, unsigned frame_rate);
int ca_rtsp_add_video(struct ca_rtsp *server, const char *path,
                      enum ca_video_codec codec, unsigned frame_rate,
                      unsigned *stream_id);
int ca_rtsp_push_h264(struct ca_rtsp *server, const uint8_t *data,
                      size_t length, bool key_frame, float hfov_deg);
int ca_rtsp_push_h264_stream(struct ca_rtsp *server, unsigned stream_id,
                             const uint8_t *data, size_t length,
                             bool key_frame, float hfov_deg);
int ca_rtsp_push_video(struct ca_rtsp *server, unsigned stream_id,
                       const uint8_t *data, size_t length, bool key_frame, float hfov_deg);
/* For sources with variable cadence (for example thermal throttling), preserve
 * their presentation clock instead of stepping the nominal frame rate. */
int ca_rtsp_push_video_timed(struct ca_rtsp *server, unsigned stream_id,
                             const uint8_t *data, size_t length, bool key_frame,
                             float hfov_deg, uint64_t pts_us);
int ca_rtsp_set_frame_rate(struct ca_rtsp *server, unsigned stream_id,
                           unsigned frame_rate);
int ca_rtsp_support_proxy(struct ca_rtsp *server, const struct ca_support_config *config);
void ca_rtsp_close(struct ca_rtsp *server);

#ifdef __cplusplus
}
#endif

#endif
