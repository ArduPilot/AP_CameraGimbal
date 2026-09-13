#ifndef CAMERA_APP_TARGETING_H
#define CAMERA_APP_TARGETING_H

#include <stdbool.h>
#include <stdint.h>

bool ca_targeting_global_angles(int32_t vehicle_lat_e7,
                                int32_t vehicle_lon_e7,
                                float vehicle_alt_amsl_m,
                                int32_t target_lat_e7,
                                int32_t target_lon_e7,
                                float target_alt_amsl_m,
                                float *pitch_rad,
                                float *yaw_earth_rad);

#endif
