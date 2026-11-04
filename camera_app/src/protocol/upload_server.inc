// Cloud upload, included just before the server is
// opened. The server's periodic step runs the rclone supervisor, telling it
// whether the camera is recording and whether the vehicle is armed, and writes
// the supervisor's state for the web page. The settings are those of the saved
// configuration, so they take effect without a restart.
#if APCAM_HAVE_UPLOAD
#include "apcam/network.h"
#include "camera_app/uploader.h"

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

// Refuses a path that does not fit rather than truncating it: a truncated
// media root could name another directory, which rclone move would then empty.
static bool upload_path(char *to, size_t size, const char *from)
{
    if (strlen(from) >= size) { errno = ENAMETOOLONG; return false; }
    memcpy(to, from, strlen(from) + 1U);
    return true;
}

static bool upload_open(struct ca_mavlink_server *server, const struct ca_mavlink_server_config *config)
{
    static_assert(sizeof(server->rclone_path) == sizeof(ca_upload_settings::executable) &&
                  sizeof(server->rclone_config) == sizeof(ca_upload_settings::config_path) &&
                  sizeof(server->record_root) == sizeof(ca_upload_settings::record_root),
                  "uploader paths are checked against the server's copies");
    const char *ready = getenv("CAMERA_APP_READY_PATH");
    if (!ready || !*ready) ready = "/run/camera-app.ready";
    if (!upload_path(server->rclone_path, sizeof(server->rclone_path), config->rclone_path ? config->rclone_path : "") ||
        !upload_path(server->rclone_config, sizeof(server->rclone_config), config->rclone_config ? config->rclone_config : "") ||
        !upload_path(server->record_root, sizeof(server->record_root), config->record_root ? config->record_root : ""))
        return false;
    if (strlen(server->capture_root) >= sizeof(ca_upload_settings::capture_root)) { errno = ENAMETOOLONG; return false; }
    if (snprintf(server->upload_status_path, sizeof(server->upload_status_path), "%s.upload", ready) >=
        (int)sizeof(server->upload_status_path)) { errno = ENAMETOOLONG; return false; }
    if (snprintf(server->upload_trigger_path, sizeof(server->upload_trigger_path), "%s.upload-now", ready) >=
        (int)sizeof(server->upload_trigger_path)) { errno = ENAMETOOLONG; return false; }
    const char *holdoff = getenv("CAMERA_APP_UPLOAD_HOLDOFF_MS");
    server->upload_holdoff_ms = holdoff && *holdoff ? strtoull(holdoff, NULL, 10) : UPLOAD_HOLDOFF_MS;
    (void)unlink(server->upload_status_path); /* from an earlier run */
    (void)unlink(server->upload_trigger_path);
    server->uploader = ca_uploader_create();
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
    upload_copy(settings.executable, sizeof(settings.executable), server->rclone_path);
    upload_copy(settings.config_path, sizeof(settings.config_path), server->rclone_config);
    /* The web page stores the credentials as remote gdrive. A blank folder is
     * left as a blank remote, which the uploader reports, rather than uploading
     * into the root of the Drive. */
    upload_copy(settings.credentials_section, sizeof(settings.credentials_section), APCAM_UPLOAD_REMOTE_NAME);
    if (config->upload_folder[0])
        snprintf(settings.remote, sizeof(settings.remote), "%s:%s", APCAM_UPLOAD_REMOTE_NAME, config->upload_folder);
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
    if (server->upload_status_text[0] && now - server->upload_status_ms < 1000U) return;
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
    ca_uploader_periodic(server->uploader, now, ca_media_recording(server->media), armed);
    upload_write_status(server, now);
}
#else
static inline bool upload_open(struct ca_mavlink_server *, const struct ca_mavlink_server_config *) { return true; }
static inline void upload_close(struct ca_mavlink_server *) {}
static inline void upload_periodic(struct ca_mavlink_server *, uint64_t) {}
#endif
