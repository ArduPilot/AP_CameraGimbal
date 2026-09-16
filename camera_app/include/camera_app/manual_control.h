#ifndef CAMERA_APP_MANUAL_CONTROL_H
#define CAMERA_APP_MANUAL_CONTROL_H
#include <stdbool.h>
#include "apcam/manual_control.h"
struct ca_backend;
struct ca_mavlink_server;
struct ca_manual_control {
    int fd;
    unsigned port;
    bool active, executing;
    uint8_t token[16];
    uint64_t expires_ms, stop_ms;
    struct ca_backend *backend;
    struct ca_mavlink_server *mavlink;
};
int ca_manual_control_open(struct ca_manual_control *control, struct ca_backend *backend,
                           struct ca_mavlink_server *mavlink);
void ca_manual_control_update(struct ca_manual_control *control);
void ca_manual_control_close(struct ca_manual_control *control);
#endif
