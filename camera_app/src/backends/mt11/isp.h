#ifndef CAMERA_APP_MT11_ISP_H
#define CAMERA_APP_MT11_ISP_H

#include "camera_app/config.h"
#include "sample_comm.h"

td_s32 ca_mt11_apply_isp_config(const struct ca_config *config, bool live);

#include "camera_app/exposure.h"
int ca_mt11_exposure(unsigned lens, struct ca_exposure *sample);
#endif
