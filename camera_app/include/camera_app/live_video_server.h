#ifndef CAMERA_APP_LIVE_VIDEO_SERVER_H
#define CAMERA_APP_LIVE_VIDEO_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private camera-app to mt11-web transport.  A client connects on loopback and
 * sends one byte (0 or 1). The server replies with zero for success (nonzero
 * means unavailable); the remainder of a successful connection is fMP4.
 */
struct ca_live_video_server;

int ca_live_video_server_open(struct ca_live_video_server **server,
                              unsigned port);
int ca_live_video_server_configure(struct ca_live_video_server *server,
                                   unsigned stream, unsigned width,
                                   unsigned height, unsigned frame_rate,
                                   bool h264);
int ca_live_video_server_publish(struct ca_live_video_server *server,
                                 unsigned stream, const uint8_t *annex_b,
                                 size_t length, uint64_t pts, bool key_frame, float hfov_deg);
unsigned ca_live_video_server_port(const struct ca_live_video_server *server);
void ca_live_video_server_close(struct ca_live_video_server *server);

#endif
