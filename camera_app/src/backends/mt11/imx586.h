#ifndef MT11_IMX586_H
#define MT11_IMX586_H

#include "ot_sns_ctrl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MT11_IMX586_SENSOR_ID 586
#define MT11_IMX586_I2C_ADDR  0x1a
#define MT11_IMX586_WIDTH     3840
#define MT11_IMX586_HEIGHT    2160
#define MT11_IMX586_VMAX      3064
#define MT11_IMX586_MAX_FPS   30.0f
#define MT11_IMX586_MIN_FPS   1.4f

/* Fixed 3840x2160 mode used by the MT11 wide camera. */
extern ot_isp_sns_obj g_sns_mt11_imx586_obj;

td_s32 mt11_imx586_write_register(ot_vi_pipe vi_pipe, td_u32 addr, td_u32 data);
td_s32 mt11_imx586_read_register(ot_vi_pipe vi_pipe, td_u32 addr);
td_void mt11_imx586_standby(ot_vi_pipe vi_pipe);
td_void mt11_imx586_restart(ot_vi_pipe vi_pipe);

#ifdef MT11_IMX586_TEST_API
const ot_isp_sns_state *mt11_imx586_test_state(ot_vi_pipe vi_pipe);
td_s32 mt11_imx586_test_create(ot_vi_pipe vi_pipe);
void mt11_imx586_test_destroy(ot_vi_pipe vi_pipe);
#endif

#ifdef __cplusplus
}
#endif

#endif
