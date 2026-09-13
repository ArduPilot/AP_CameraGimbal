#ifndef CAMERA_APP_THERMAL_H
#define CAMERA_APP_THERMAL_H

#include <stdint.h>

struct ca_thermal_range {
    uint16_t maximum_centi_c;
    uint16_t minimum_centi_c;
    uint16_t maximum_x;
    uint16_t maximum_y;
    uint16_t minimum_x;
    uint16_t minimum_y;
    uint64_t frame_sequence;
};

#endif
