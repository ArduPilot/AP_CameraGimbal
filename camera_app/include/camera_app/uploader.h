#ifndef CAMERA_APP_UPLOADER_H
#define CAMERA_APP_UPLOADER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "camera_app/upload_backend.h"

/* Copies photos and recordings to a cloud service (see upload_backend.h) from a
 * worker thread. It never blocks: call ca_uploader_periodic() from the main loop.
 * Uploads run while the camera is recording, but not while the vehicle is armed. The file being
 * recorded to is left until the recording ends, and so is any file changed in the last few seconds. */

enum ca_upload_state {
    CA_UPLOAD_DISABLED, /* off, or the settings are not usable (see reason) */
    CA_UPLOAD_WAITING,  /* idle: armed, or waiting for the next scan */
    CA_UPLOAD_RUNNING,
    CA_UPLOAD_STOPPING, /* asked the worker to stop; waiting for it to finish */
    CA_UPLOAD_BACKOFF   /* the last run failed; waiting before retrying */
};

enum ca_upload_after { CA_UPLOAD_KEEP, CA_UPLOAD_DELETE, CA_UPLOAD_DELETE_WHEN_LOW };

#define CA_UPLOAD_PHOTOS 1U
#define CA_UPLOAD_RECORDINGS 2U

/* What the uploader tells its owner, for a short message to the ground station. */
enum ca_upload_event {
    CA_UPLOAD_EVENT_STARTED,  /* work to do: `files` is how many files need uploading */
    CA_UPLOAD_EVENT_COMPLETE, /* all of it is done */
    CA_UPLOAD_EVENT_FAILED    /* it failed; retries after this one are not announced */
};

struct ca_upload_settings {
    bool enable;
    char disabled_reason[96]; /* if set, uploads are off, and the status says why */
    char service[16];       /* "gdrive" (the default if blank) or "dropbox" */
    char config_path[256];  /* credentials file, in rclone's format */
    char credentials_section[32]; /* if set, config_path must have a [section] by this name or nothing is run */
    char ca_file[256];      /* PEM bundle of the roots the service's certificate must chain to */
    const char *ca_pem;     /* or those roots built into the program, as NUL-terminated PEM text (static); wins over ca_file */
    char folder[160];       /* the folder in the cloud account that holds photos/ and recordings/ */
    char cache_path[256];   /* where it remembers what is already uploaded; blank to check everything every time */
    char capture_root[256]; /* photos, sent to <remote>/photos */
    char record_root[256];  /* recordings, sent to <remote>/recordings */
    unsigned what;          /* CA_UPLOAD_PHOTOS | CA_UPLOAD_RECORDINGS */
    enum ca_upload_after after_upload;
    unsigned low_space_percent; /* DELETE_WHEN_LOW: a scan that starts below this free space deletes what it uploads */
    unsigned bandwidth_kib_s;   /* KiB/s; 0 = unlimited */
    unsigned scan_minutes;      /* idle time between scans, at least 1 */
};

struct ca_upload_status {
    enum ca_upload_state state;
    char service[16];    /* "gdrive" or "dropbox": the service in use, once configured */
    char reason[96];     /* why DISABLED */
    time_t last_success; /* wall clock; 0 = none yet */
    uint64_t bytes_done, bytes_total;
    unsigned files_done, files_total;
    char last_error[160];
};

struct ca_uploader;

struct ca_uploader *ca_uploader_create(void);
/* Stops a running transfer (briefly blocking) and frees everything. */
void ca_uploader_destroy(struct ca_uploader *uploader);
/* Takes effect the next time the worker starts, except that disabling stops it. */
void ca_uploader_configure(struct ca_uploader *uploader, const struct ca_upload_settings *settings);
/* now_ms is any monotonic millisecond clock. */
void ca_uploader_periodic(struct ca_uploader *uploader, uint64_t now_ms, bool armed);
/* Whether the camera is recording, and to which files (some cameras write two; none if it does not say; a file
 * changed in the last few seconds is left alone in any case). Call before ca_uploader_periodic(). */
void ca_uploader_set_recording(struct ca_uploader *uploader, bool recording, const char *const *files, unsigned count);
/* Starts the next scan at once, without waiting for the scan interval or a retry delay, and even
 * while armed: the scan it starts runs to its end. Does nothing while a scan is running, stopping
 * or the uploader is disabled. */
void ca_uploader_scan_now(struct ca_uploader *uploader, uint64_t now_ms);
const struct ca_upload_status *ca_uploader_status(const struct ca_uploader *uploader);
/* The status as key=value lines for the web page, for example
 * "state=running\nfiles_done=1\n". It never contains credentials. Returns the
 * length, or 0 if size is too small. */
size_t ca_uploader_format_status(const struct ca_upload_status *status, char *out, size_t size);
/* Called from ca_uploader_periodic() (so on the caller's thread) when a batch of uploads starts
 * (only if something needs uploading), completes, or fails. One batch can take several attempts:
 * a failure is announced once, and retries are silent until it completes. */
typedef void (*ca_uploader_notify)(void *user, enum ca_upload_event event, unsigned files);
void ca_uploader_set_notify(struct ca_uploader *uploader, ca_uploader_notify notify, void *user);
/* For tests: makes the backend, instead of ca_upload_backend_create(). */
typedef APC_UploadBackend *(*ca_uploader_backend_factory)(const struct ca_upload_target *target, char *error, size_t error_size);
void ca_uploader_set_backend_factory(struct ca_uploader *uploader, ca_uploader_backend_factory factory);
#endif
