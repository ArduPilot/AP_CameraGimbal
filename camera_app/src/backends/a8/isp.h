#ifndef CAMERA_APP_A8_ISP_H
#define CAMERA_APP_A8_ISP_H

#include "camera_app/config.h"

/* Apply the [image] settings through the MI ISP IQ/AE/AWB API.
 * Call only once frames flow and after the ISP tuning bin is loaded,
 * since the bin load resets these. */
int ca_a8_apply_isp_config(const struct ca_config *config);

#endif
