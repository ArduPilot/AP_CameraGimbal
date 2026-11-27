/* Chooses the upload backend for ca_upload_target::service. */
#include "camera_app/upload_backend.h"

#include <stdio.h>
#include <string.h>

APC_UploadBackend *ca_upload_backend_create(const ca_upload_target *target, char *error, size_t error_size)
{
    if (target && target->service) {
        if (!strcmp(target->service, "gdrive")) return ca_gdrive_backend_create(target, error, error_size);
        if (!strcmp(target->service, "dropbox")) return ca_dropbox_backend_create(target, error, error_size);
    }
    if (error && error_size) snprintf(error, error_size, "unknown upload service");
    return NULL;
}
