#ifndef CAMERA_APP_MAVLINK_SERVER_H
#define CAMERA_APP_MAVLINK_SERVER_H

#include "camera_app/config.h"

struct ca_backend;
struct ca_media;
struct ca_mavlink_server;

struct ca_mavlink_server_config {
    unsigned tcp_port;
    unsigned udp_port;
    const char *uart_device;
    unsigned rtsp_port;
    const char *capture_root;
    const char *config_path;
    enum ca_photo_scope photo_scope;
    struct ca_config settings;
    struct ca_backend *backend;
    struct ca_media *media;
};

int ca_mavlink_server_open(struct ca_mavlink_server **server,
                           const struct ca_mavlink_server_config *config);
int ca_mavlink_server_fd(const struct ca_mavlink_server *server);
int ca_mavlink_server_handle(struct ca_mavlink_server *server);
void ca_mavlink_server_periodic(struct ca_mavlink_server *server);
void ca_mavlink_server_close(struct ca_mavlink_server *server);

#endif
