// Cloud upload, included just before the server is
// opened. The server's periodic step runs the upload supervisor, telling it
// whether the camera is recording and whether the vehicle is armed, and writes
// the supervisor's state for the web page. The settings are those of the saved
// configuration, so they take effect without a restart.
#if APCAM_HAVE_UPLOAD
#include "apcam/network.h"
#include "camera_app/uploader.h"
#if APCAM_UPLOAD_EMBED_CA
#include "upload_ca_bundle.h" /* upload_embedded_ca, made by tools/embed_ca.py */
#endif

static_assert((int)CA_UPLOAD_WHAT_PHOTOS == (int)CA_UPLOAD_PHOTOS && (int)CA_UPLOAD_WHAT_RECORDINGS == (int)CA_UPLOAD_RECORDINGS &&
              (int)CA_UPLOAD_AFTER_KEEP == (int)CA_UPLOAD_KEEP && (int)CA_UPLOAD_AFTER_DELETE == (int)CA_UPLOAD_DELETE &&
              (int)CA_UPLOAD_AFTER_DELETE_WHEN_LOW == (int)CA_UPLOAD_DELETE_WHEN_LOW,
              "config and uploader enumerations must agree");

// No upload starts until this long after the camera app starts. The vehicle's
// armed state is not known until the autopilot's first heartbeat, which can take
// a while, and a restart in flight must not start uploading. CAMERA_APP_UPLOAD_HOLDOFF_MS
// shortens it for tests.
#define UPLOAD_HOLDOFF_MS 30000U

static void upload_copy(char *to, size_t size, const char *from)
{
    size_t length = strlen(from);
    if (length >= size) length = size - 1U;
    memcpy(to, from, length);
    to[length] = '\0';
}

// A path that does not fit is refused rather than truncated: a truncated media
// root could name another directory, which a move would then empty. That turns
// uploads off, with the reason in their status; it must not stop the camera.
static bool upload_path(char *to, size_t size, const char *from)
{
    if (strlen(from) >= size) return false;
    memcpy(to, from, strlen(from) + 1U);
    return true;
}

/* Short messages to the ground station: a batch of uploads starting (only when files need uploading),
 * completing, or failing, all at INFO severity. */
static void upload_notify(void *user, enum ca_upload_event event, unsigned files)
{
    struct ca_mavlink_server *server = (struct ca_mavlink_server *)user;
    /* The service the uploader is using, so the message says where the files are going. A MAVLink
     * STATUSTEXT holds 50 characters; the longest of these is 43. */
    const char *service = strcmp(ca_uploader_status(server->uploader)->service, APCAM_UPLOAD_DROPBOX_NAME) == 0 ? "Dropbox"
                                                                                                                 : "Google Drive";
    char text[50];
    switch (event) {
    case CA_UPLOAD_EVENT_STARTED:
        snprintf(text, sizeof(text), files == 1 ? "Upload to %s started (%u file)" : "Upload to %s started (%u files)",
                 service, files);
        break;
    case CA_UPLOAD_EVENT_COMPLETE:
        snprintf(text, sizeof(text), "Upload to %s complete", service);
        break;
    case CA_UPLOAD_EVENT_FAILED:
        snprintf(text, sizeof(text), "Upload to %s failed", service);
        break;
    default:
        return;
    }
    send_status_text(server, MAV_SEVERITY_INFO, text);
}

static bool upload_open(struct ca_mavlink_server *server, const struct ca_mavlink_server_config *config)
{
    const char *ready = getenv("CAMERA_APP_READY_PATH");
    if (!ready || !*ready) ready = "/run/camera-app.ready";
    if (!upload_path(server->upload_ca_file, sizeof(server->upload_ca_file), config->upload_ca_file ? config->upload_ca_file : "") ||
        !upload_path(server->rclone_config, sizeof(server->rclone_config), config->rclone_config ? config->rclone_config : "") ||
        !upload_path(server->record_root, sizeof(server->record_root), config->record_root ? config->record_root : "") ||
        strlen(server->capture_root) >= sizeof(ca_upload_settings::capture_root)) {
        upload_copy(server->upload_problem, sizeof(server->upload_problem), "A folder or file path is too long to upload from");
        ca_log("upload: %s", server->upload_problem);
    }
    if (snprintf(server->upload_status_path, sizeof(server->upload_status_path), "%s.upload", ready) >=
        (int)sizeof(server->upload_status_path)) { errno = ENAMETOOLONG; return false; }
    if (snprintf(server->upload_trigger_path, sizeof(server->upload_trigger_path), "%s.upload-now", ready) >=
        (int)sizeof(server->upload_trigger_path)) { errno = ENAMETOOLONG; return false; }
    const char *holdoff = getenv("CAMERA_APP_UPLOAD_HOLDOFF_MS");
    server->upload_holdoff_ms = holdoff && *holdoff ? strtoull(holdoff, NULL, 10) : UPLOAD_HOLDOFF_MS;
    (void)unlink(server->upload_status_path); /* from an earlier run */
    (void)unlink(server->upload_trigger_path);
    server->uploader = ca_uploader_create();
    if (server->uploader != NULL) ca_uploader_set_notify(server->uploader, upload_notify, server);
    return server->uploader != NULL;
}

static void upload_close(struct ca_mavlink_server *server)
{
    ca_uploader_destroy(server->uploader);
    if (server->upload_status_path[0]) (void)unlink(server->upload_status_path);
    if (server->upload_trigger_path[0]) (void)unlink(server->upload_trigger_path);
}

static void upload_configure(struct ca_mavlink_server *server)
{
    const struct ca_config *config = &server->parameters;
    struct ca_upload_settings settings = {};
    settings.enable = config->upload_enable;
    upload_copy(settings.disabled_reason, sizeof(settings.disabled_reason), server->upload_problem);
    upload_copy(settings.config_path, sizeof(settings.config_path), server->rclone_config);
    const char *service = config->upload_service == CA_UPLOAD_SERVICE_DROPBOX ? APCAM_UPLOAD_DROPBOX_NAME : APCAM_UPLOAD_REMOTE_NAME;
    upload_copy(settings.ca_file, sizeof(settings.ca_file), server->upload_ca_file);
#if APCAM_UPLOAD_EMBED_CA
    settings.ca_pem = upload_embedded_ca; /* built in: no file to install */
#endif
    /* What is already uploaded is remembered beside the credentials, one file per service. */
    const char *slash = strrchr(server->rclone_config, '/');
    if (slash != NULL) {
        const int dir_length = (int)(slash - server->rclone_config);
        if (snprintf(settings.cache_path, sizeof(settings.cache_path), "%.*s/upload-cache-%s.txt", dir_length,
                     server->rclone_config, service) >= (int)sizeof(settings.cache_path))
            settings.cache_path[0] = '\0'; /* too long: check everything every time */
    }
    /* The web page stores the credentials under the name gdrive. A blank folder is
     * left blank, which the uploader reports, rather than uploading into the root
     * of the Drive. */
    upload_copy(settings.service, sizeof(settings.service), service);
    upload_copy(settings.credentials_section, sizeof(settings.credentials_section), service);
    upload_copy(settings.folder, sizeof(settings.folder), config->upload_folder);
    upload_copy(settings.capture_root, sizeof(settings.capture_root), server->capture_root);
    upload_copy(settings.record_root, sizeof(settings.record_root), server->record_root);
    settings.what = (unsigned)config->upload_what;
    settings.after_upload = (enum ca_upload_after)config->upload_after;
    settings.low_space_percent = config->upload_low_space_percent;
    settings.bandwidth_kib_s = config->upload_bandwidth_kib_s;
    settings.scan_minutes = config->upload_scan_minutes;
    ca_uploader_configure(server->uploader, &settings);
}

// Written at most once a second, and only when it changes.
static void upload_write_status(struct ca_mavlink_server *server, uint64_t now)
{
    char text[sizeof(server->upload_status_text)], temporary[sizeof(server->upload_status_path) + 5];
    if (ca_uploader_format_status(ca_uploader_status(server->uploader), text, sizeof(text)) == 0) return;
    if (strcmp(text, server->upload_status_text) == 0) return;
    if (server->upload_status_ms && now - server->upload_status_ms < 1000U) return;
    server->upload_status_ms = now;
    snprintf(temporary, sizeof(temporary), "%s.tmp", server->upload_status_path);
    FILE *file = fopen(temporary, "w");
    if (!file) return;
    bool ok = fputs(text, file) >= 0;
    if (fclose(file) != 0) ok = false;
    if (ok && rename(temporary, server->upload_status_path) == 0) memcpy(server->upload_status_text, text, strlen(text) + 1U);
    else unlink(temporary);
}

static void upload_periodic(struct ca_mavlink_server *server, uint64_t now)
{
    if (server->uploader == NULL) return;
    // After the start-up hold-off, no flight controller seen yet counts as
    // disarmed, so a bench camera uploads. Once one has been seen, a lost heartbeat counts as armed.
    if (!server->upload_started_ms) server->upload_started_ms = now ? now : 1U;
    // This runs on every pass of the main loop; the uploader needs it ten times a second.
    if (server->upload_tick_ms && now - server->upload_tick_ms < 100U) return;
    server->upload_tick_ms = now ? now : 1U;
    const bool starting = now - server->upload_started_ms < server->upload_holdoff_ms;
    const bool armed = starting || (server->have_vehicle_armed &&
                       (server->vehicle_armed || now - server->vehicle_heartbeat_ms > 5000U));
    upload_configure(server);
    // The web page's "Upload now" button creates this file. Checked four times a second.
    if (now - server->upload_trigger_ms >= 250U) {
        server->upload_trigger_ms = now;
        if (unlink(server->upload_trigger_path) == 0) ca_uploader_scan_now(server->uploader, now);
    }
    const bool recording = ca_media_recording(server->media);
    const char *recording_files[3] = {}; /* the videos and the raw thermal file */
    unsigned recording_count = 0;
    for (unsigned i = 0; recording && i < 3U; i++) {
        const char *file = ca_media_recording_path_at(server->media, i);
        if (file && file[0]) recording_files[recording_count++] = file;
    }
    ca_uploader_set_recording(server->uploader, recording, recording_files, recording_count);
    ca_uploader_periodic(server->uploader, now, armed);
    upload_write_status(server, now);
}
#else
static inline bool upload_open(struct ca_mavlink_server *, const struct ca_mavlink_server_config *) { return true; }
static inline void upload_close(struct ca_mavlink_server *) {}
static inline void upload_periodic(struct ca_mavlink_server *, uint64_t) {}
#endif
