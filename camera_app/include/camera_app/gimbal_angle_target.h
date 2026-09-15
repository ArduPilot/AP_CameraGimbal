#ifndef CAMERA_APP_GIMBAL_ANGLE_TARGET_H
#define CAMERA_APP_GIMBAL_ANGLE_TARGET_H

#include "camera_app/backend.h"
#include "apcam/gimbal_transform.h"
#include <math.h>

/* Cache the transmitted target in wire units, so float roundoff does not
 * restart a controller that already holds the requested position. */
struct ca_angle_target {
    bool valid;
    int16_t yaw;
    int16_t pitch;
    uint64_t sent_ms;
};

static inline void ca_angle_target_invalidate_siyi(struct ca_angle_target *target,
                                                  uint8_t opcode,
                                                  const uint8_t *payload,
                                                  size_t length)
{
    if (opcode == 0x07U || opcode == 0x08U || opcode == 0x0eU || opcode == 0x40U ||
        (opcode == 0x0cU && length == 1U && payload[0] >= 3U && payload[0] <= 5U)) {
        target->valid = false;
    }
}

static inline bool ca_angle_target_suppress(const struct ca_angle_target *target,
                                           int16_t yaw, int16_t pitch,
                                           float target_yaw_deg, float target_pitch_deg,
                                           const struct ca_gimbal_attitude *attitude,
                                           uint64_t now)
{
    if (!APCAM_SUPPRESS_DUPLICATE_ANGLES || !target->valid ||
        target->yaw != yaw || target->pitch != pitch) return false;

    /* These controllers hold absolute targets without a keepalive; repeatedly
     * sending an identical command can cause hunting (measured on MT11).
     * Recover from lost commands/controller resets at most once a second.
     * The tolerance only controls retries, never changes to the target. */
    if (now - target->sent_ms < 1000U) return true;
    if (attitude != NULL && now - attitude->timestamp_ms < 1000U) {
        const float degrees = 180.0f / 3.14159265358979323846f;
        bool reached = fabsf(apcam_wrap_degrees(attitude->yaw_rad * degrees - target_yaw_deg)) <= 2.0f &&
                       fabsf(attitude->pitch_rad * degrees - target_pitch_deg) <= 2.0f;
        bool moving = fabsf(attitude->yaw_rate_rad_s * degrees) > 1.0f ||
                      fabsf(attitude->pitch_rate_rad_s * degrees) > 1.0f;
        if (reached || moving) return true;
    }
    return false;
}

/* Call only after the transport accepts the command. */
static inline void ca_angle_target_sent(struct ca_angle_target *target,
                                       int16_t yaw, int16_t pitch, uint64_t now)
{
    target->valid = true;
    target->yaw = yaw;
    target->pitch = pitch;
    target->sent_ms = now;
}

#endif
