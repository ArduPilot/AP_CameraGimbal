#ifndef CAMERA_APP_LOG_H
#define CAMERA_APP_LOG_H

void ca_log(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

#endif
