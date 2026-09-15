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
#else
#error "Unsupported APCAM_TARGET"
#endif
#define APCAM_HAVE_SIYI (APCAM_VENDOR_PROTOCOL == APCAM_PROTOCOL_SIYI)
#define APCAM_HAVE_XFROBOT (APCAM_VENDOR_PROTOCOL == APCAM_PROTOCOL_XFROBOT)
#ifndef APCAM_LOG_ROOT
#define APCAM_LOG_ROOT "/mnt/logs"
#endif
#ifndef APCAM_TRACKING_RATE_I
#define APCAM_TRACKING_RATE_I 0.0f
#endif
/* String forms for configuration defaults; numeric IDs remain stable. */
#define APCAM_RESOLUTION_NAME(value) ((value) == APCAM_RES_720P ? "1280x720" : \
    (value) == APCAM_RES_1080P ? "1920x1080" : (value) == APCAM_RES_1440P ? "2560x1440" : "3840x2160")
#define APCAM_STRING_VALUE_(value) #value
#define APCAM_STRING_VALUE(value) APCAM_STRING_VALUE_(value)
#endif
