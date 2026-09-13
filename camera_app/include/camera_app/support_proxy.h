#ifndef CAMERA_APP_SUPPORT_PROXY_H
#define CAMERA_APP_SUPPORT_PROXY_H

#include "camera_app/config.h"
#include "camera_app/mavlink.h"
#include <stdbool.h>

struct ca_support_mavlink;
int ca_support_mavlink_open(struct ca_support_mavlink **result,
                            const struct ca_support_config *config);
void ca_support_mavlink_send(struct ca_support_mavlink *proxy,
                             const mavlink_message_t *message);
bool ca_support_mavlink_receive(struct ca_support_mavlink *proxy,
                                mavlink_message_t *message);
void ca_support_mavlink_close(struct ca_support_mavlink *proxy);
#endif
