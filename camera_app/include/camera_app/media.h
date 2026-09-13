#ifndef CAMERA_APP_MEDIA_H
#define CAMERA_APP_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_app/config.h"
#include "camera_app/thermal.h"

struct ca_media;

enum ca_media_lens {
    CA_MEDIA_LENS_WIDE = 0,
    CA_MEDIA_LENS_ZOOM = 1,
};

struct ca_media_config {
    const char *backend;
    const char *record_root;
    const char *capture_root;
    unsigned rtsp_port;
    unsigned width;
    unsigned height;
    unsigned frame_rate;
    unsigned bit_rate_kbps;
    struct ca_config settings;
};

int ca_media_open(struct ca_media **media, const struct ca_media_config *config);
int ca_media_set_recording(struct ca_media *media, bool active);
bool ca_media_recording(const struct ca_media *media);
const char *ca_media_recording_path(const struct ca_media *media);
int ca_media_set_zoom(struct ca_media *media, float zoom);
float ca_media_zoom(const struct ca_media *media);
/* Effective FOV of the visible/thermal sensor, including current zoom. */
float ca_media_hfov(const struct ca_media *media, bool thermal);
unsigned ca_media_frame_rate(const struct ca_media *media, bool thermal);
int ca_media_set_lens(struct ca_media *media, enum ca_media_lens lens);
enum ca_media_lens ca_media_lens(const struct ca_media *media);
int ca_media_set_thermal_main(struct ca_media *media, bool thermal_main);
bool ca_media_thermal_main(const struct ca_media *media);
int ca_media_autofocus(struct ca_media *media, uint16_t x, uint16_t y);
int ca_media_manual_focus(struct ca_media *media, int direction);
int ca_media_set_focus_percent(struct ca_media *media, float percent);
bool ca_media_thermal_range(struct ca_media *media,
                            struct ca_thermal_range *range);
int ca_media_capture_photo(struct ca_media *media, enum ca_photo_scope scope);
int ca_media_get_thermal_gain(struct ca_media *media, uint8_t *gain);
int ca_media_set_thermal_gain(struct ca_media *media, uint8_t gain);
int ca_media_get_thermal_palette(struct ca_media *media, uint8_t *palette);
int ca_media_set_thermal_palette(struct ca_media *media, uint8_t palette);
int ca_media_set_inverted(struct ca_media *media, bool inverted);
void ca_media_close(struct ca_media *media);

#endif
