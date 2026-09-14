#ifndef CAMERA_APP_CAMERA_FTP_H
#define CAMERA_APP_CAMERA_FTP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Read-only MAVFTP service for one immutable camera definition. No host
 * filesystem paths are ever opened, and clients have independent sessions. */
struct ca_camera_ftp_session {
    uint8_t system, component, id;
    bool active;
    uint64_t last_ms;
};
struct ca_camera_ftp {
    struct ca_camera_ftp_session sessions[4];
};
void ca_camera_ftp_reply(struct ca_camera_ftp *ftp, const char *xml, size_t length,
                         uint8_t system, uint8_t component, uint64_t now_ms,
                         const uint8_t request[251], uint8_t response[251]);
#endif
