#ifndef CAMERA_APP_UPLOAD_BACKEND_H
#define CAMERA_APP_UPLOAD_BACKEND_H
#include <atomic>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A cloud service the uploader can send files to. The calls block, so they
 * are made from the uploader's worker thread; setting `stop` ends one quickly.
 * Another service is one more class behind ca_upload_backend_create(). */

enum ca_put_result {
    CA_PUT_UPLOADED, /* sent and checked against the local file */
    CA_PUT_SKIPPED,  /* already there with the same content */
    CA_PUT_LEFT,     /* not sent, and the local file must not be deleted (an empty file where the service has content) */
    CA_PUT_REJECTED, /* see `error`; this file cannot be sent (it cannot be read, or its name is unusable): the next may be */
    CA_PUT_FAILED,   /* see `error`; worth retrying later */
    CA_PUT_STOPPED   /* the stop flag was set */
};

struct ca_upload_target {
    const char *service;              /* "gdrive" */
    const char *credentials_path;     /* rclone-format config file */
    const char *credentials_section;  /* its section, for example "gdrive" */
    const char *root_folder;          /* folders below the service's root, "/" separated; created if missing */
    const char *ca_file;              /* PEM bundle of trusted roots */
    unsigned bandwidth_kib_s;         /* 0 = unlimited */
    const std::atomic<bool> *stop;
    /* Tests point these at a local server; blank means the real service. */
    const char *api_host, *auth_host, *server_name;
    unsigned api_port, auth_port;
};

class APC_UploadBackend {
public:
    virtual ~APC_UploadBackend() {}
    /* Sends `local_path` as `name` in `remote_dir` (folders below the root,
     * "/" separated, created if missing; may be empty). `progress` reports bytes
     * of this file sent so far. */
    virtual ca_put_result put(const char *remote_dir, const char *name, const char *local_path,
                              void (*progress)(void *user, uint64_t sent), void *user,
                              char *error, size_t error_size) = 0;
    virtual void set_bandwidth(unsigned kib_s) = 0;
};

/* The services themselves: "gdrive" and "dropbox". */
APC_UploadBackend *ca_gdrive_backend_create(const ca_upload_target *target, char *error, size_t error_size);
APC_UploadBackend *ca_dropbox_backend_create(const ca_upload_target *target, char *error, size_t error_size);

/* NULL with a message in `error` if the service is unknown or the credentials
 * cannot be read. No network is used until the first put(). */
APC_UploadBackend *ca_upload_backend_create(const ca_upload_target *target, char *error, size_t error_size);

#endif
