#include "camera_app/targeting.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define PI_F 3.14159265358979323846f

static float degrees(float radians)
{
    return radians * (180.0f / PI_F);
}

static void expect_angles(int32_t vehicle_lat, int32_t vehicle_lon,
                          float vehicle_alt, int32_t target_lat,
                          int32_t target_lon, float target_alt,
                          float expected_pitch, float expected_yaw,
                          float tolerance)
{
    float pitch;
    float yaw;
    assert(ca_targeting_global_angles(vehicle_lat, vehicle_lon, vehicle_alt,
                                      target_lat, target_lon, target_alt,
                                      &pitch, &yaw));
    assert(fabsf(degrees(pitch) - expected_pitch) < tolerance);
    assert(fabsf(degrees(yaw) - expected_yaw) < tolerance);
}

int main(void)
{
    const int32_t latitude = -353632620;
    const int32_t longitude = 1491652370;
    const int32_t north_100m = latitude + 8983;
    const int32_t east_100m = longitude + 10996;

    expect_angles(latitude, longitude, 600.0f,
                  north_100m, longitude, 550.0f,
                  -26.565f, 0.0f, 0.1f);
    expect_angles(latitude, longitude, 600.0f,
                  latitude, east_100m, 600.0f,
                  0.0f, 90.0f, 0.1f);
    expect_angles(latitude, 1799999500, 100.0f,
                  latitude, -1799999500, 100.0f,
                  0.0f, 90.0f, 0.1f);
    expect_angles(latitude, longitude, 100.0f,
                  latitude, longitude, 50.0f,
                  -90.0f, 0.0f, 0.1f);

    float pitch = 1.0f;
    float yaw = 2.0f;
    assert(!ca_targeting_global_angles(900000001, longitude, 100.0f,
                                       latitude, longitude, 100.0f,
                                       &pitch, &yaw));
    assert(pitch == 1.0f && yaw == 2.0f);
    assert(!ca_targeting_global_angles(latitude, longitude, NAN,
                                       latitude, longitude, 100.0f,
                                       &pitch, &yaw));

    puts("targeting tests passed");
    return 0;
}
