#ifndef CAMERA_APP_RECORDER_H
#define CAMERA_APP_RECORDER_H

#include <stdbool.h>

struct ca_recorder {
    bool active;
    const char *state_path;
};

void ca_recorder_init(struct ca_recorder *recorder, const char *state_path);
int ca_recorder_set(struct ca_recorder *recorder, bool active);
bool ca_recorder_active(const struct ca_recorder *recorder);
void ca_recorder_close(struct ca_recorder *recorder);

#endif
