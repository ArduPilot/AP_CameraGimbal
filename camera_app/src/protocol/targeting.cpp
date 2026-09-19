#include "camera_app/targeting.h"

#include <math.h>
#include <stddef.h>

#define EARTH_RADIUS_M 6378137.0
#define PI_D 3.14159265358979323846
#define DEG_TO_RAD (PI_D / 180.0)

static bool valid_location(int32_t lat_e7, int32_t lon_e7, float alt_m)
{
    return lat_e7 >= -900000000 && lat_e7 <= 900000000 &&
           lon_e7 >= -1800000000 && lon_e7 <= 1800000000 &&
           isfinite(alt_m);
}

bool ca_targeting_predict_position(int32_t *lat_e7, int32_t *lon_e7, float *alt_m,
                                   float vn, float ve, float vd, float seconds)
{
    if (!lat_e7 || !lon_e7 || !alt_m || !valid_location(*lat_e7, *lon_e7, *alt_m) ||
        !isfinite(vn) || !isfinite(ve) || !isfinite(vd) ||
        !isfinite(seconds) || seconds < 0) return false;
    double lat = *lat_e7 * 1.0e-7 * DEG_TO_RAD;
    double lon = *lon_e7 * 1.0e-7 * DEG_TO_RAD;
    double distance = hypot(vn, ve) * seconds / EARTH_RADIUS_M;
    double bearing = atan2(ve, vn);
    double sine_lat = sin(lat) * cos(distance) + cos(lat) * sin(distance) * cos(bearing);
    double predicted_lat = asin(fmax(-1.0, fmin(1.0, sine_lat)));
    double predicted_lon = lon + atan2(sin(bearing) * sin(distance) * cos(lat),
                                        cos(distance) - sin(lat) * sin(predicted_lat));
    float predicted_alt = *alt_m - vd * seconds;
    if (!isfinite(predicted_alt)) return false;
    *lat_e7 = (int32_t)llround(predicted_lat / DEG_TO_RAD * 1.0e7);
    *lon_e7 = (int32_t)llround(remainder(predicted_lon, 2.0 * PI_D) / DEG_TO_RAD * 1.0e7);
    *alt_m = predicted_alt;
    return true;
}

bool ca_targeting_global_angles(int32_t vehicle_lat_e7,
                                int32_t vehicle_lon_e7,
                                float vehicle_alt_amsl_m,
                                int32_t target_lat_e7,
                                int32_t target_lon_e7,
                                float target_alt_amsl_m,
                                float *pitch_rad,
                                float *yaw_earth_rad)
{
    if (pitch_rad == NULL || yaw_earth_rad == NULL ||
        !valid_location(vehicle_lat_e7, vehicle_lon_e7,
                        vehicle_alt_amsl_m) ||
        !valid_location(target_lat_e7, target_lon_e7, target_alt_amsl_m)) {
        return false;
    }

    double lat1 = (double)vehicle_lat_e7 * 1.0e-7 * DEG_TO_RAD;
    double lon1 = (double)vehicle_lon_e7 * 1.0e-7 * DEG_TO_RAD;
    double lat2 = (double)target_lat_e7 * 1.0e-7 * DEG_TO_RAD;
    double lon2 = (double)target_lon_e7 * 1.0e-7 * DEG_TO_RAD;
    double delta_lon = remainder(lon2 - lon1, 2.0 * PI_D);
    double bearing_y = sin(delta_lon) * cos(lat2);
    double bearing_x = cos(lat1) * sin(lat2) -
                       sin(lat1) * cos(lat2) * cos(delta_lon);
    double angular_y = hypot(bearing_y, bearing_x);
    double angular_x = sin(lat1) * sin(lat2) +
                       cos(lat1) * cos(lat2) * cos(delta_lon);
    double horizontal_m = EARTH_RADIUS_M * atan2(angular_y, angular_x);
    double altitude_m = (double)target_alt_amsl_m - vehicle_alt_amsl_m;

    *yaw_earth_rad = (float)atan2(bearing_y, bearing_x);
    *pitch_rad = (float)atan2(altitude_m, horizontal_m);
    return true;
}
