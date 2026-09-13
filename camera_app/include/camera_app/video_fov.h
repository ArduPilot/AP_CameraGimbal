#ifndef CAMERA_APP_VIDEO_FOV_H
#define CAMERA_APP_VIDEO_FOV_H

#include <math.h>
#include "apcam/target.h"

/* Nominal horizontal lens calibration. Zoom scales focal length, not angle. */
#define CA_VISIBLE_HFOV_DEG APCAM_LENS1_FOV_H
#if APCAM_HAVE_THERMAL
#define CA_THERMAL_HFOV_DEG APCAM_LENS3_FOV_H
#else
#define CA_THERMAL_HFOV_DEG 0.0f
#endif

static inline float ca_video_hfov(float native_hfov_deg, float magnification)
{
    const float radians = 0.01745329251994329577f;
    if (!isfinite(native_hfov_deg) || native_hfov_deg <= 0.0f ||
        native_hfov_deg >= 180.0f || !isfinite(magnification) ||
        magnification <= 0.0f) return 0.0f;
    return 2.0f * atanf(tanf(native_hfov_deg * radians * 0.5f) /
                         magnification) / radians;
}

static inline float ca_lens1_hfov(float zoom)
{
#if APCAM_LENS1_FOV_MODEL == APCAM_FOV_ENDPOINTS
    const float radians = 0.01745329251994329577f;
    if (!isfinite(zoom) || zoom < 1.0f || zoom > APCAM_LENS1_OPTICAL_ZOOM_MAX)
        return 0.0f;
    float ratio = tanf(APCAM_LENS1_FOV_H * radians * 0.5f) /
                  tanf(APCAM_LENS1_FOV_H_TELE * radians * 0.5f);
    zoom = 1.0f + (ratio - 1.0f) * (zoom - 1.0f) / (APCAM_LENS1_OPTICAL_ZOOM_MAX - 1.0f);
#endif
    return ca_video_hfov(APCAM_LENS1_FOV_H, zoom);
}

/* The secondary RGB lens has its own calibration, independent of the
 * system-zoom ratio used to select it. */
static inline float ca_zoom_lens_hfov(float optical_zoom, float digital_zoom)
{
#if APCAM_NUM_LENSES > 1 && APCAM_LENS2_TYPE == APCAM_LENS_TYPE_RGB
    return ca_video_hfov(APCAM_LENS2_FOV_H, optical_zoom * digital_zoom);
#else
    (void)optical_zoom; (void)digital_zoom;
    return 0.0f;
#endif
}

#endif
