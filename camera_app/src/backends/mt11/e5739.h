#ifndef CAMERA_APP_MT11_E5739_H
#define CAMERA_APP_MT11_E5739_H

struct ca_e5739;

/* The stock MT11 expresses zoom relative to the wide sensor.  It crops that
 * sensor through 3.44x, then switches to the E5739-equipped tele sensor and
 * scales its optical factor relative to the crossover. */
#include "apcam/lens.h"
#define CA_E5739_SYSTEM_CROSSOVER APCAM_ZOOM_LENS_BASE
#define ca_e5739_system_uses_tele apcam_uses_zoom_lens
#define ca_e5739_system_to_optical apcam_zoom_lens_optical

int ca_e5739_open(struct ca_e5739 **result, const char *calibration_path);
int ca_e5739_set_zoom(struct ca_e5739 *motor, float requested_zoom,
                       float *optical_zoom);
float ca_e5739_zoom(const struct ca_e5739 *motor);
int ca_e5739_focus_range(const struct ca_e5739 *motor, int *minimum,
                         int *maximum, int *current);
int ca_e5739_set_focus_position(struct ca_e5739 *motor, int position);
void ca_e5739_close(struct ca_e5739 *motor);

/* Kept separate from hardware access so calibration policy is host-testable. */
int ca_e5739_lookup_calibration(const char *path, float requested_zoom,
                                float *factor, int *zoom_position,
                                int *infinity_focus_position);
int ca_e5739_lookup_focus_range(const char *path, float requested_zoom,
                                int *minimum, int *maximum);

#endif
