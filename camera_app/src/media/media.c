#define _GNU_SOURCE
#include "camera_app/media_impl.h"
#include "camera_app/log.h"
#include "apcam/target.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>

struct ca_media {
    struct ca_media_impl *impl;
    struct ca_media_config config;
    bool inverted;
};

int ca_media_open(struct ca_media **result, const struct ca_media_config *config)
{
    if (!result || !config) { errno = EINVAL; return -1; }
    struct ca_media *media = calloc(1, sizeof(*media));
    if (!media) return -1;
    media->config = *config;
    media->inverted = config->settings.orientation == CA_MOUNT_INVERTED;
    if (ca_media_impl_open(&media->impl, config) < 0) { free(media); return -1; }
    *result = media;
    return 0;
}

const struct ca_config *ca_media_settings(const struct ca_media *media)
{
    return &media->config.settings;
}

struct live_controls {
    float zoom;
    enum ca_media_lens lens;
    bool thermal_main;
    int gain, palette;
};

static int restore_controls(struct ca_media *media, const struct live_controls *state)
{
    int result = 0;
    if (ca_media_impl_set_inverted(media->impl, media->inverted) < 0) result = -1;
    if (APCAM_NUM_LENSES > 1 && ca_media_impl_set_lens(media->impl, state->lens) < 0) result = -1;
    if (APCAM_HAVE_ZOOM && isfinite(state->zoom) &&
        ca_media_impl_set_zoom(media->impl, state->zoom) < 0) result = -1;
    if (APCAM_HAVE_THERMAL) {
        if (ca_media_impl_set_thermal_main(media->impl, state->thermal_main) < 0) result = -1;
        if (state->gain >= 0 && ca_media_impl_set_thermal_gain(media->impl, (uint8_t)state->gain) < 0) result = -1;
        if (state->palette >= 0 && ca_media_impl_set_thermal_palette(media->impl, (uint8_t)state->palette) < 0) result = -1;
    }
    return result;
}

int ca_media_configure(struct ca_media *media, const struct ca_config *settings)
{
    if (!media || !settings) { errno = EINVAL; return -1; }
    const struct ca_config *old = &media->config.settings;
    bool pipeline = !media->impl || old->main_resolution != settings->main_resolution ||
        old->sub_resolution != settings->sub_resolution ||
        old->recording_resolution != settings->recording_resolution ||
        old->main_codec != settings->main_codec || old->sub_codec != settings->sub_codec;
    if (pipeline) {
        if (media->impl && ca_media_impl_recording(media->impl)) { errno = EBUSY; return -1; }
        struct live_controls state = {.zoom = 1, .gain = -1, .palette = -1};
        if (media->impl) {
            state.zoom = ca_media_impl_zoom(media->impl);
            state.lens = ca_media_impl_lens(media->impl);
            state.thermal_main = ca_media_impl_thermal_main(media->impl);
            uint8_t value;
            if (APCAM_HAVE_THERMAL && ca_media_impl_get_thermal_gain(media->impl, &value) == 0) state.gain = value;
            if (APCAM_HAVE_THERMAL && ca_media_impl_get_thermal_palette(media->impl, &value) == 0) state.palette = value;
        }
        struct ca_media_config next = media->config;
        next.settings = *settings;
        ca_media_impl_close(media->impl);
        media->impl = NULL;
        ca_log("reconfiguring media pipeline without restarting camera app");
        if (ca_media_impl_open(&media->impl, &next) < 0 || restore_controls(media, &state) < 0) {
            int saved_errno = errno;
            ca_media_impl_close(media->impl);
            media->impl = NULL;
            if (ca_media_impl_open(&media->impl, &media->config) < 0 || restore_controls(media, &state) < 0)
                ca_log("media configuration rollback failed; retry configuration");
            errno = saved_errno ? saved_errno : EIO;
            return -1;
        }
        media->config = next;
        return 0;
    }
    if (!ca_config_image_equal(old, settings)) {
        if (ca_media_impl_apply_image(media->impl, settings) < 0) {
            int saved_errno = errno;
            if (ca_media_impl_apply_image(media->impl, old) < 0)
                ca_log("image configuration rollback failed");
            errno = saved_errno;
            return -1;
        }
    }
    media->config.settings = *settings;
    return 0;
}

void ca_media_close(struct ca_media *media)
{
    if (!media) return;
    ca_media_impl_close(media->impl);
    free(media);
}

/* The stable handle is retained by gimbal callbacks and the MAVLink server;
 * only the private implementation changes during pipeline reconfiguration. */
#define IMPL (media ? media->impl : NULL)
#define REQUIRE_IMPL do { if (!IMPL) { errno = ENODEV; return -1; } } while (0)
bool ca_media_ready(const struct ca_media *media)
{ return IMPL && ca_media_impl_ready(IMPL); }
int ca_media_set_recording(struct ca_media *media, bool active)
{ REQUIRE_IMPL; return ca_media_impl_set_recording(IMPL, active); }
bool ca_media_recording(const struct ca_media *media)
{ return IMPL && ca_media_impl_recording(IMPL); }
const char *ca_media_recording_path(const struct ca_media *media)
{ return IMPL ? ca_media_impl_recording_path(IMPL) : NULL; }
int ca_media_set_zoom(struct ca_media *media, float zoom)
{ REQUIRE_IMPL; return ca_media_impl_set_zoom(IMPL, zoom); }
float ca_media_zoom(const struct ca_media *media)
{ return IMPL ? ca_media_impl_zoom(IMPL) : 1; }
float ca_media_hfov(const struct ca_media *media, bool thermal)
{ return IMPL ? ca_media_impl_hfov(IMPL, thermal) : NAN; }
unsigned ca_media_frame_rate(const struct ca_media *media, bool thermal)
{ return IMPL ? ca_media_impl_frame_rate(IMPL, thermal) : 0; }
int ca_media_set_lens(struct ca_media *media, enum ca_media_lens lens)
{ REQUIRE_IMPL; return ca_media_impl_set_lens(IMPL, lens); }
enum ca_media_lens ca_media_lens(const struct ca_media *media)
{ return IMPL ? ca_media_impl_lens(IMPL) : CA_MEDIA_LENS_WIDE; }
int ca_media_set_thermal_main(struct ca_media *media, bool thermal_main)
{ REQUIRE_IMPL; return ca_media_impl_set_thermal_main(IMPL, thermal_main); }
bool ca_media_thermal_main(const struct ca_media *media)
{ return IMPL && ca_media_impl_thermal_main(IMPL); }
int ca_media_autofocus(struct ca_media *media, uint16_t x, uint16_t y)
{ REQUIRE_IMPL; return ca_media_impl_autofocus(IMPL, x, y); }
int ca_media_manual_focus(struct ca_media *media, int direction)
{ REQUIRE_IMPL; return ca_media_impl_manual_focus(IMPL, direction); }
int ca_media_set_focus_percent(struct ca_media *media, float percent)
{ REQUIRE_IMPL; return ca_media_impl_set_focus_percent(IMPL, percent); }
bool ca_media_thermal_range(struct ca_media *media, struct ca_thermal_range *range)
{ return IMPL && ca_media_impl_thermal_range(IMPL, range); }
int ca_media_capture_photo(struct ca_media *media, enum ca_photo_scope scope)
{ REQUIRE_IMPL; return ca_media_impl_capture_photo(IMPL, scope); }
int ca_media_get_thermal_gain(struct ca_media *media, uint8_t *gain)
{ REQUIRE_IMPL; return ca_media_impl_get_thermal_gain(IMPL, gain); }
int ca_media_set_thermal_gain(struct ca_media *media, uint8_t gain)
{ REQUIRE_IMPL; return ca_media_impl_set_thermal_gain(IMPL, gain); }
int ca_media_get_thermal_palette(struct ca_media *media, uint8_t *palette)
{ REQUIRE_IMPL; return ca_media_impl_get_thermal_palette(IMPL, palette); }
int ca_media_set_thermal_palette(struct ca_media *media, uint8_t palette)
{ REQUIRE_IMPL; return ca_media_impl_set_thermal_palette(IMPL, palette); }
int ca_media_set_inverted(struct ca_media *media, bool inverted)
{
    REQUIRE_IMPL;
    if (ca_media_impl_set_inverted(IMPL, inverted) < 0) return -1;
    media->inverted = inverted;
    return 0;
}
