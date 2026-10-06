#ifndef APCAM_TARGET_H
#define APCAM_TARGET_H
#include "target_ids.h"
#ifndef APCAM_TARGET
#error "Set APCAM_TARGET in the build (for example APCAM_TARGET_MT11)"
#endif
#if APCAM_TARGET == APCAM_TARGET_MT11
#include "target_mt11.h"
#elif APCAM_TARGET == APCAM_TARGET_A8
#include "target_a8.h"
#elif APCAM_TARGET == APCAM_TARGET_ZR10
#include "target_zr10.h"
#elif APCAM_TARGET == APCAM_TARGET_Z1_MINI
#include "target_z1mini.h"
#elif APCAM_TARGET == APCAM_TARGET_RPI_LIBCAM_CADDX
#include "target_rpi_libcam_caddx.h"
#else
#error "Unsupported APCAM_TARGET"
#endif
#define APCAM_HAVE_SIYI (APCAM_VENDOR_PROTOCOL == APCAM_PROTOCOL_SIYI)
#define APCAM_HAVE_XFROBOT (APCAM_VENDOR_PROTOCOL == APCAM_PROTOCOL_XFROBOT)
#ifndef APCAM_HAVE_SITL
#define APCAM_HAVE_SITL 1
#endif
/* Targets whose gimbal link is optional set this to 1: without a gimbal
 * the camera advertises no gimbal component (ca_backend_has_gimbal()). */
#ifndef APCAM_GIMBAL_OPTIONAL
#define APCAM_GIMBAL_OPTIONAL 0
#endif
/* Targets with selectable photo sizes (needing a sensor mode switch) set this
 * to 1 and define APCAM_PHOTO_{BINNED,FULL}_{WIDTH,HEIGHT,NAME}. */
#ifndef APCAM_HAVE_PHOTO_RESOLUTION
#define APCAM_HAVE_PHOTO_RESOLUTION 0
#endif
/* Targets on a host OS which owns networking set this to 0. */
#ifndef APCAM_HAVE_NETWORK_CONFIG
#define APCAM_HAVE_NETWORK_CONFIG 1
#endif
#ifndef APCAM_LOG_ROOT
#define APCAM_LOG_ROOT "/mnt/logs"
#endif
#ifndef APCAM_TRACKING_RATE_I
#define APCAM_TRACKING_RATE_I 0.0f
#endif
#ifndef APCAM_TRACKING_MIN_PITCH_RATE
#define APCAM_TRACKING_MIN_PITCH_RATE 0.0f
#endif
#ifndef APCAM_TRACKING_MIN_YAW_RATE
#define APCAM_TRACKING_MIN_YAW_RATE 0.0f
#endif
#ifndef APCAM_SIM_OPTICAL_ZOOM_STEP
#define APCAM_SIM_OPTICAL_ZOOM_STEP 0.0f
#endif
/* String forms for configuration defaults; numeric IDs remain stable. */
#define APCAM_RESOLUTION_NAME(value) ((value) == APCAM_RES_720P ? "1280x720" : \
    (value) == APCAM_RES_1080P ? "1920x1080" : (value) == APCAM_RES_1440P ? "2560x1440" : "3840x2160")
#define APCAM_STRING_VALUE_(value) #value
#define APCAM_STRING_VALUE(value) APCAM_STRING_VALUE_(value)
#endif
