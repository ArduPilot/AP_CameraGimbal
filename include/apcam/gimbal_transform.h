#ifndef APCAM_GIMBAL_TRANSFORM_H
#define APCAM_GIMBAL_TRANSFORM_H

#include "target.h"
#include <stdbool.h>

/* Signed joint-coordinate mapping, in roll/pitch/yaw order. Matrices are
 * orthogonal reflections/permutations; offsets express joint zero positions
 * in degrees. These operate on joint angles, not on a spatial Euler vector.
 * Feedback maps wire -> canonical; commands map canonical -> wire.
 * Angular velocities use the same matrix without the zero-position offset. */
struct apcam_gimbal_transform {
    float matrix[9];
    float offset[3];
};
#define APCAM_TRANSFORM_PAIR(channel) { \
    {APCAM_GIMBAL_##channel##_UPRIGHT_MATRIX, APCAM_GIMBAL_##channel##_UPRIGHT_OFFSET}, \
    {APCAM_GIMBAL_##channel##_INVERTED_MATRIX, APCAM_GIMBAL_##channel##_INVERTED_OFFSET} }
static const struct apcam_gimbal_transform apcam_feedback[2] = APCAM_TRANSFORM_PAIR(FEEDBACK);
static const struct apcam_gimbal_transform apcam_private_feedback[2] = APCAM_TRANSFORM_PAIR(PRIVATE_FEEDBACK);
static const struct apcam_gimbal_transform apcam_angle_command[2] = APCAM_TRANSFORM_PAIR(ANGLE_COMMAND);
static const struct apcam_gimbal_transform apcam_rate_command[2] = APCAM_TRANSFORM_PAIR(RATE_COMMAND);
#undef APCAM_TRANSFORM_PAIR

static inline float apcam_wrap_degrees(float v)
{
    while (v > 180.0f) v -= 360.0f;
    while (v < -180.0f) v += 360.0f;
    return v;
}
static inline void apcam_transform(const struct apcam_gimbal_transform *t,
                                  const float input[3], float output[3], bool rates)
{
    float result[3];
    for (unsigned row = 0; row < 3; row++) {
        float v = rates ? 0 : t->offset[row];
        for (unsigned col = 0; col < 3; col++) v += t->matrix[3 * row + col] * input[col];
        result[row] = rates ? v : apcam_wrap_degrees(v);
    }
    for (unsigned i = 0; i < 3; i++) output[i] = result[i];
}
static inline void apcam_inverse_transform(const struct apcam_gimbal_transform *t,
                                          const float input[3], float output[3], bool rates)
{
    float result[3];
    for (unsigned row = 0; row < 3; row++) {
        float v = 0;
        for (unsigned col = 0; col < 3; col++)
            v += t->matrix[3 * col + row] * (input[col] - (rates ? 0 : t->offset[col]));
        result[row] = rates ? v : apcam_wrap_degrees(v);
    }
    for (unsigned i = 0; i < 3; i++) output[i] = result[i];
}
#endif
