#ifndef CAMERA_APP_SITL_TERRAIN_H
#define CAMERA_APP_SITL_TERRAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ca_sitl_terrain;
int ca_sitl_terrain_open(struct ca_sitl_terrain **out, const char *script,
                         const unsigned width[4], const unsigned height[4], unsigned fps);
/* Outputs: visible main, thermal/A8 sub, optional MT11 visible sub.
 * One request in flight: bounded memory and no accumulating video latency. */
int ca_sitl_terrain_frame(struct ca_sitl_terrain *terrain, uint64_t pts, uint64_t presentation_ms,
                          const float hfov[2], bool thermal_main, bool has_thermal, bool separate_recording,
                          uint8_t *data[4], size_t length[4], bool key[4]);
void ca_sitl_terrain_interrupt(struct ca_sitl_terrain *terrain);
void ca_sitl_terrain_close(struct ca_sitl_terrain *terrain);

#endif
