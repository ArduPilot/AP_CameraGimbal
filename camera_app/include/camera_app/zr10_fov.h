#ifndef CAMERA_APP_ZR10_FOV_H
#define CAMERA_APP_ZR10_FOV_H

#include "camera_app/video_fov.h"

/* ZR10 lens specifications supplied by the operator. The endpoint angles
 * imply a different focal-length ratio from the nominal 10x zoom label.
 * Estimate intermediate focal lengths by interpolating between both known
 * endpoints; a measured zoom/FOV table can replace this approximation later.
 * Diagonal FOV is 79.5 degrees at 1x and 7.7 degrees at 10x; telemetry uses HFOV.
 */
#define CA_ZR10_WIDE_HFOV_DEG APCAM_LENS1_FOV_H
#define CA_ZR10_TELE_HFOV_DEG APCAM_LENS1_FOV_H_TELE
#define CA_ZR10_OPTICAL_MAX_ZOOM APCAM_LENS1_OPTICAL_ZOOM_MAX

static inline float ca_zr10_hfov(float zoom)
{
    return ca_lens1_hfov(zoom);
}

#endif
