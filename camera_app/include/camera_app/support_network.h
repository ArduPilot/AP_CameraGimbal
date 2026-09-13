#ifndef CAMERA_APP_SUPPORT_NETWORK_H
#define CAMERA_APP_SUPPORT_NETWORK_H
#include "camera_app/config.h"
/* Adds a secondary IPv4 address and, when requested, replaces the default
 * gateway on the selected interface. Existing connected addresses survive. */
int ca_support_network_configure(const struct ca_support_config *config);
#endif
