#ifndef CAMERA_APP_SIYI_SERVER_H
#define CAMERA_APP_SIYI_SERVER_H

#include <stddef.h>
#include <stdint.h>

struct ca_siyi_server;

typedef int (*ca_siyi_request_fn)(void *opaque, const uint8_t *packet,
                                  size_t length);

int ca_siyi_server_open(struct ca_siyi_server **server, unsigned port,
                        const char *uart_device);
int ca_siyi_server_fd(const struct ca_siyi_server *server);
int ca_siyi_server_handle(struct ca_siyi_server *server,
                          ca_siyi_request_fn request, void *opaque);
void ca_siyi_server_emit(void *opaque, const uint8_t *packet, size_t length);
void ca_siyi_server_close(struct ca_siyi_server *server);

#endif
