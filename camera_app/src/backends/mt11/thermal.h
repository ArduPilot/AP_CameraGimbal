#ifndef CAMERA_APP_MT11_THERMAL_H
#define CAMERA_APP_MT11_THERMAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "camera_app/thermal.h"

#define CA_MT11_THERMAL_WIDTH 640U
#define CA_MT11_THERMAL_HEIGHT 512U
#define CA_MT11_THERMAL_FRAME_BYTES \
    (CA_MT11_THERMAL_WIDTH * CA_MT11_THERMAL_HEIGHT * 4U)

typedef void (*ca_mt11_thermal_frame_cb)(const uint8_t *display_yuyv,
                                         const uint16_t *radiometric_y16,
                                         const struct timespec *captured_at,
                                         void *opaque);

struct ca_mt11_uvc_assembler {
    uint8_t *frame;
    size_t used;
    int fid;
    struct timespec last_received_at;
    bool have_received_at;
    ca_mt11_thermal_frame_cb callback;
    void *opaque;
};

struct ca_mt11_thermal;

int ca_mt11_uvc_assembler_init(struct ca_mt11_uvc_assembler *assembler,
                               ca_mt11_thermal_frame_cb callback,
                               void *opaque);
void ca_mt11_uvc_assembler_destroy(struct ca_mt11_uvc_assembler *assembler);
int ca_mt11_uvc_assembler_consume(struct ca_mt11_uvc_assembler *assembler,
                                  const uint8_t *payload, size_t length,
                                  const struct timespec *received_at);

void ca_mt11_yuyv_to_nv12(const uint8_t *source, uint32_t width,
                          uint32_t height, uint8_t *y, size_t y_stride,
                          uint8_t *uv, size_t uv_stride);
void ca_mt11_yuyv_to_nv12_rotated_180(const uint8_t *source, uint32_t width,
                                      uint32_t height, uint8_t *y,
                                      size_t y_stride, uint8_t *uv,
                                      size_t uv_stride);
bool ca_mt11_thermal_range_from_y16(const uint16_t *pixels, uint32_t width,
                                    uint32_t height,
                                    struct ca_thermal_range *range);
void ca_mt11_thermal_gain_get_command(uint8_t command[16]);
void ca_mt11_thermal_gain_set_command(uint8_t command[16], uint8_t gain);
void ca_mt11_thermal_palette_get_command(uint8_t command[16]);
void ca_mt11_thermal_palette_set_command(uint8_t command[16], uint8_t palette);

int ca_mt11_thermal_open(struct ca_mt11_thermal **result,
                         ca_mt11_thermal_frame_cb callback, void *opaque);
int ca_mt11_thermal_get_gain(struct ca_mt11_thermal *thermal, uint8_t *gain);
int ca_mt11_thermal_set_gain(struct ca_mt11_thermal *thermal, uint8_t gain);
int ca_mt11_thermal_get_palette(struct ca_mt11_thermal *thermal,
                               uint8_t *palette);
int ca_mt11_thermal_set_palette(struct ca_mt11_thermal *thermal,
                               uint8_t palette);
void ca_mt11_thermal_close(struct ca_mt11_thermal *thermal);

#endif
