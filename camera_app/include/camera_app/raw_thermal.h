#ifndef CAMERA_APP_RAW_THERMAL_H
#define CAMERA_APP_RAW_THERMAL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct ca_raw_thermal;

int ca_raw_thermal_open(struct ca_raw_thermal **capture, const char *root);
int ca_raw_thermal_write(struct ca_raw_thermal *capture,
                         const uint16_t *pixels, uint32_t width,
                         uint32_t height);
int ca_raw_thermal_write_at(struct ca_raw_thermal *capture,
                            const uint16_t *pixels, uint32_t width,
                            uint32_t height,
                            const struct timespec *captured_at);
const char *ca_raw_thermal_path(const struct ca_raw_thermal *capture);
unsigned ca_raw_thermal_count(const struct ca_raw_thermal *capture);
void ca_raw_thermal_close(struct ca_raw_thermal *capture);

#endif
