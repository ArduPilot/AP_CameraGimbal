#ifndef CAMERA_APP_XFROBOT_SERVER_H
#define CAMERA_APP_XFROBOT_SERVER_H
#include "camera_app/backend.h"
#include "camera_app/media.h"
struct ca_xfrobot_server;
int ca_xfrobot_server_open(struct ca_xfrobot_server **out, unsigned port, bool inverted);
void ca_xfrobot_server_update(struct ca_xfrobot_server *server, struct ca_backend *backend, struct ca_media *media);
void ca_xfrobot_server_close(struct ca_xfrobot_server *server);
#endif
