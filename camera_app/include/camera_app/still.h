#ifndef CAMERA_APP_STILL_H
#define CAMERA_APP_STILL_H

#include <stddef.h>
#include <time.h>

int ca_still_write_jpeg(const char *root, char suffix,
                        const unsigned char *jpeg, size_t jpeg_length,
                        const struct timespec *captured_at,
                        char *path, size_t path_size);

#endif
