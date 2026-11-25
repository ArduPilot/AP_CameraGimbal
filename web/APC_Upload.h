#pragma once
// Cloud upload on the Parameters page, included by mt11-web.cpp where the file,
// form and translation helpers it uses are already defined.
#include "APC_StringBuffer.h"

#if APCAM_HAVE_UPLOAD
/* Cloud upload: the camera app sends the files to the cloud service. This page
 * shows its status and stores the user's own Google Drive or Dropbox credentials in a
 * file in rclone's format, which only the camera software can read. */
#define UPLOAD_REMOTE_NAME APCAM_UPLOAD_REMOTE_NAME

/* explicit_bzero is not in every C library the cameras use (uClibc lacks it); volatile stores are not
 * optimised away, which a plain memset can be. */
static void upload_zero(void *memory, size_t length)
{
    volatile unsigned char *p = static_cast<volatile unsigned char *>(memory);
    while (length--) *p++ = 0;
}

static const char *upload_rclone_config_path(void)
{
    const char *path = getenv("CAMERA_APP_RCLONE_CONFIG");
    return path != NULL && *path ? path : APCAM_RCLONE_CONFIG;
}

static bool upload_status_value(const char *text, const char *key, char *out, size_t size)
{
    const size_t key_length = strlen(key);
    for (const char *line = text; line != NULL && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (strncmp(line, key, key_length) == 0 && line[key_length] == '=') {
            size_t length = strcspn(line + key_length + 1, "\n");
            if (length >= size) length = size - 1;
            memcpy(out, line + key_length + 1, length);
            out[length] = '\0';
            return true;
        }
    }
    out[0] = '\0';
    return false;
}

static bool camera_app_running(void)
{
    size_t size;
    char *ready = read_file(CAMERA_READY_PATH, 512, &size);
    const char *pid_line = ready != NULL ? strstr(ready, "\npid=") : NULL;
    long pid = 0;
    const bool running = pid_line != NULL && sscanf(pid_line, "\npid=%ld", &pid) == 1 &&
                         pid > 1 && kill((pid_t)pid, 0) == 0;
    free(ready);
    return running;
}

/* Asks the camera app to start the next upload scan at once, by creating the
 * file it watches for. Only the camera software and this server can write there. */
static bool upload_request_now(char *error, size_t error_size)
{
    char path[sizeof(CAMERA_READY_PATH) + 16];
    snprintf(path, sizeof(path), "%s.upload-now", CAMERA_READY_PATH);
    if (!camera_app_running()) {
        snprintf(error, error_size, "%s", T(S_UPLOAD_NO_STATUS));
        return false;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        snprintf(error, error_size, T(S_UPLOAD_NOW_FAILED), strerror(errno));
        return false;
    }
    close(fd);
    return true;
}

/* Why uploads are waiting, if the uploader says (the clock is not set, say), else false. */
static bool upload_waiting_reason(char *reason, size_t reason_size)
{
    char path[sizeof(CAMERA_READY_PATH) + 8], state[16];
    size_t size;
    snprintf(path, sizeof(path), "%s.upload", CAMERA_READY_PATH);
    char *text = camera_app_running() ? read_file(path, 2048, &size) : NULL;
    bool waiting = text != NULL && upload_status_value(text, "state", state, sizeof(state)) && !strcmp(state, "waiting") &&
                   upload_status_value(text, "reason", reason, reason_size) && reason[0];
    free(text);
    return waiting;
}

/* The status paragraph, also fetched on its own by the page every few seconds.
 * state is set to the uploader's state, or "none" when there is no status. */
static void append_upload_status_text(APC_StringBuffer *page, char state[16])
{
    char path[sizeof(CAMERA_READY_PATH) + 8], reason[128], success[24], error[192];
    char done[16], total[16], bytes_done[24], bytes_total[24], service[16];
    size_t size;
    snprintf(path, sizeof(path), "%s.upload", CAMERA_READY_PATH);
    char *text = camera_app_running() ? read_file(path, 2048, &size) : NULL;
    if (text == NULL || !upload_status_value(text, "state", state, 16)) {
        snprintf(state, 16, "none");
        page->append("<p class=help id=upload-status>");
        page->append_html(T(S_UPLOAD_NO_STATUS));
        page->append("</p>");
        free(text);
        return;
    }
    upload_status_value(text, "service", service, sizeof(service));
    upload_status_value(text, "reason", reason, sizeof(reason));
    upload_status_value(text, "last_success", success, sizeof(success));
    upload_status_value(text, "last_error", error, sizeof(error));
    upload_status_value(text, "files_done", done, sizeof(done));
    upload_status_value(text, "files_total", total, sizeof(total));
    upload_status_value(text, "bytes_done", bytes_done, sizeof(bytes_done));
    upload_status_value(text, "bytes_total", bytes_total, sizeof(bytes_total));
    const struct { const char *name; enum string_id label; } states[] = {
        {"disabled", S_UPLOAD_STATE_DISABLED}, {"waiting", S_UPLOAD_STATE_WAITING},
        {"running", S_UPLOAD_STATE_RUNNING}, {"stopping", S_UPLOAD_STATE_STOPPING},
        {"backoff", S_UPLOAD_STATE_BACKOFF},
    };
    page->append("<p class=notice id=upload-status>");
    /* The service the camera is really using, which is what the saved settings say, not what the
     * dropdown shows right now. */
    if (service[0]) {
        const char *name = T(!strcmp(service, "dropbox") ? S_UPLOAD_SERVICE_DROPBOX : S_UPLOAD_SERVICE_GDRIVE);
        char line[96];
        snprintf(line, sizeof(line), T(S_UPLOAD_ACTIVE_SERVICE), name);
        page->append_html(line);
        page->append("<br>");
    }
    if (!strcmp(state, "running")) {
        /* "Uploading 4 of 10": the file being sent is the one after those done. */
        unsigned long finished = strtoul(done, NULL, 10), all = strtoul(total, NULL, 10);
        unsigned long long sent = strtoull(bytes_done, NULL, 10), bytes = strtoull(bytes_total, NULL, 10);
        if (all > 0 && bytes > 0) {
            const unsigned percent = sent >= bytes ? 100U : (unsigned)(sent * 100ULL / bytes);
            page->appendf(T(S_UPLOAD_PROGRESS_PERCENT), finished < all ? finished + 1 : all, all, percent);
        } else if (all > 0) {
            page->appendf(T(S_UPLOAD_PROGRESS), finished < all ? finished + 1 : all, all);
        } else {
            /* No files to send have been reported yet: the uploader is checking what is already uploaded. */
            page->append_html(T(S_UPLOAD_CHECKING));
        }
    } else {
        for (const auto &entry : states) {
            if (!strcmp(state, entry.name)) page->append_html(T(entry.label));
        }
    }
    if ((!strcmp(state, "disabled") || !strcmp(state, "waiting")) && reason[0] && strcmp(reason, "Disabled") != 0) {
        page->append(": ");
        page->append_html(reason);
    }
    time_t when = (time_t)atoll(success);
    struct tm local;
    if (when > 0 && localtime_r(&when, &local) != NULL) {
        char when_text[40];
        strftime(when_text, sizeof(when_text), "%Y-%m-%d %H:%M:%S", &local);
        page->appendf("<br>%s: ", T(S_UPLOAD_LAST_SUCCESS));
        page->append_html(when_text);
    }
    if (error[0]) {
        page->appendf("<br>%s: ", T(S_UPLOAD_LAST_ERROR));
        page->append_html(error);
    }
    page->append("</p>");
    free(text);
}

static void append_upload_status(APC_StringBuffer *page)
{
    char state[16];
    page->appendf("<h3>%s</h3>", T(S_UPLOAD_STATUS));
    append_upload_status_text(page, state);
    /* Pressing it while uploading does nothing; it is only unavailable when
     * there is nothing to start. The page keeps this up to date. */
    const bool unavailable = !strcmp(state, "none") || !strcmp(state, "disabled");
    /* The form itself is outside the parameters form (forms cannot be nested). */
    page->appendf("<p><button type=submit form=upload-now%s>%s</button></p>",
                  unavailable ? " disabled" : "", T(S_UPLOAD_NOW));
}

/* GET /upload-status: the uploader's state on the first line, then the status paragraph. */
static void send_upload_status(int fd)
{
    char state[16];
    APC_StringBuffer body;
    APC_StringBuffer text;
    append_upload_status_text(&text, state);
    body.appendf("%s\n", state);
    body.append_n(text.data(), text.size());
    send_response(fd, 200, "OK", "text/plain; charset=utf-8", body.data(), body.size(), "Cache-Control: no-store\r\n");
}

static bool upload_credential_valid(const char *text, size_t length, bool token);

/* The cloud services with credentials of their own. Each has a section in the credentials
 * file, a rclone remote type, a form, and its own words on the page. */
struct upload_service_ui {
    const char *section, *type, *form;
    const char *guide_anchor; /* the part of the setup guide for this service */
    string_id heading, help, saved_for, id_label, secret_label;
};
static const struct upload_service_ui upload_services[] = {
    {UPLOAD_REMOTE_NAME, "drive", "upload-credentials", "google-drive", S_UPLOAD_CRED_HEADING, S_UPLOAD_CRED_HELP,
     S_UPLOAD_CRED_SAVED_FOR, S_UPLOAD_CRED_ID, S_UPLOAD_CRED_SECRET},
    {APCAM_UPLOAD_DROPBOX_NAME, "dropbox", "upload-credentials-dropbox", "dropbox", S_UPLOAD_DBX_HEADING, S_UPLOAD_DBX_HELP,
     S_UPLOAD_DBX_SAVED_FOR, S_UPLOAD_DBX_ID, S_UPLOAD_DBX_SECRET},
};

static const struct upload_service_ui *upload_service_named(const char *name, size_t length)
{
    for (const struct upload_service_ui &service : upload_services)
        if (strlen(service.section) == length && memcmp(service.section, name, length) == 0) return &service;
    return NULL;
}

static bool upload_credentials_saved(const struct upload_service_ui *service)
{
    size_t size;
    char *config = read_file(upload_rclone_config_path(), 65536, &size);
    char header[32], later[34];
    snprintf(header, sizeof(header), "[%s]", service->section);
    snprintf(later, sizeof(later), "\n[%s]", service->section);
    const bool saved = config != NULL && (strncmp(config, header, strlen(header)) == 0 || strstr(config, later) != NULL);
    free(config);
    return saved;
}

/* The client ID saved in the service's section, if it is plain enough to put in
 * the page unescaped (the save path only allows these characters). */
static void upload_saved_client_id(const struct upload_service_ui *service, char *out, size_t size)
{
    size_t file_size;
    char *config = read_file(upload_rclone_config_path(), 65536, &file_size);
    char header[32];
    snprintf(header, sizeof(header), "[%s]", service->section);
    out[0] = '\0';
    bool inside = false;
    for (const char *line = config; line != NULL && *line;) {
        const size_t length = strcspn(line, "\n");
        if (line[0] == '[') {
            inside = strncmp(line, header, strlen(header)) == 0;
        } else if (inside && strncmp(line, "client_id = ", 12) == 0 && length - 12 < size &&
                   upload_credential_valid(line + 12, length - 12, false)) {
            memcpy(out, line + 12, length - 12);
            out[length - 12] = '\0';
            break;
        }
        line += length;
        if (*line == '\n') line++;
    }
    free(config);
}

/* The inputs belong to a separate upload-credentials form per service, which sit
 * before the parameters form because forms cannot be nested. */
static void append_upload_credentials_block(APC_StringBuffer *page, const struct upload_service_ui *service)
{
    char id[208] = "";
    const bool saved = upload_credentials_saved(service);
    if (saved) upload_saved_client_id(service, id, sizeof(id));
    const char *replace = saved ? T(S_UPLOAD_CRED_REPLACE) : "";
    page->appendf("<h3>%s</h3><p class=help>", T(service->heading));
    page->append_html(T(service->help));
    page->appendf("</p><p class=help><a href=\"%s#%s\" target=_blank rel=\"noopener noreferrer\">", APCAM_UPLOAD_GUIDE_URL,
                  service->guide_anchor);
    page->append_html(T(S_UPLOAD_SETUP_GUIDE));
    page->appendf("</a></p><p class=notice id=%s-status>", service->form);
    page->append_html(T(saved ? service->saved_for : S_UPLOAD_CRED_NONE));
    /* The secret and token are never sent back. The client ID is not secret, so it is shown.
     * new-password keeps password managers from filling the login into these boxes. */
    page->appendf("</p><p><label>%s<br><input form=%s name=client_id size=60 maxlength=200 "
                  "autocomplete=off spellcheck=false value=\"%s\"></label></p>", T(service->id_label), service->form, id);
    page->appendf("<p><label>%s<br><input form=%s name=client_secret type=password size=60 "
                  "maxlength=200 autocomplete=new-password placeholder=\"", T(service->secret_label), service->form);
    page->append_html(replace);
    page->appendf("\"></label></p><p><label>%s<br><textarea form=%s name=token rows=4 cols=60 "
                  "style=\"min-height:0;height:6.5em\" maxlength=4096 autocomplete=off spellcheck=false placeholder=\"",
                  T(S_UPLOAD_CRED_TOKEN), service->form);
    page->append_html(replace);
    page->append("\"></textarea></label></p>");
    page->appendf("<p><button type=submit form=%s name=action value=save>%s</button> "
                  "<button type=submit form=%s name=action value=remove formnovalidate>%s</button></p>",
                  service->form, T(S_UPLOAD_CRED_SAVE), service->form, T(S_UPLOAD_CRED_REMOVE));
}

/* Which service's credentials the page shows: the one just saved or removed, then the one in the
 * parameters being edited, then the saved setting. */
static const struct upload_service_ui *upload_selected_service(const char *config, const APC_HTTPRequest *submitted)
{
    static const char *const fields[] = {"service", "upload_service"};
    for (const char *name : fields) {
        size_t length = 0;
        char *value = submitted != NULL ? form_value(submitted, name, &length) : NULL;
        const struct upload_service_ui *service = value != NULL ? upload_service_named(value, length) : NULL;
        free(value);
        if (service != NULL) return service;
    }
    char value[32] = "";
    ini_get_value(config, "upload", "service", value, sizeof(value));
    const struct upload_service_ui *service = upload_service_named(value, strlen(value));
    return service != NULL ? service : &upload_services[0];
}

/* Both services' credential blocks are in the page, and only the selected one is shown; the
 * page's script switches them when the Cloud service setting changes. They sit right below
 * that setting. Without the script the saved setting decides. */
static void append_upload_credentials(APC_StringBuffer *page, const struct upload_service_ui *selected)
{
    for (const struct upload_service_ui &service : upload_services) {
        page->appendf("<div class=upload-credentials data-service=%s style=\"grid-column:1/-1\"%s>", service.section,
                      &service == selected ? "" : " hidden");
        append_upload_credentials_block(page, &service);
        page->append("</div>");
    }
}

static bool upload_credential_valid(const char *text, size_t length, bool token)
{
    if (token) {
        if (length < 2 || length > 4096 || text[0] != '{' || text[length - 1] != '}' ||
            strstr(text, "\"access_token\"") == NULL || strstr(text, "\"refresh_token\"") == NULL) return false;
        for (size_t i = 0; i < length; i++) {
            if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e) return false;
        }
        return true;
    }
    return length >= 1 && length <= 200 &&
           strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._~-") == length;
}

/* Replace (or with a NULL section remove) one section of the rclone config,
 * keeping any others. The file is written 0600 and swapped in atomically.
 * The caller holds the lock (see upload_replace_section()). */
static bool upload_replace_section_locked(const char *path, const char *name, const char *section,
                                          char *error, size_t error_size)
{
    size_t size = 0;
    char *old = read_file(path, 65536, &size);
    if (old == NULL && errno != ENOENT) {
        snprintf(error, error_size, T(S_UPLOAD_CRED_FAILED), strerror(errno));
        return false;
    }
    if (old == NULL) old = strdup("");
    char header[64], temporary[PATH_MAX];
    snprintf(header, sizeof(header), "[%s]", name);
    APC_StringBuffer out;
    out.reset();
    bool skipping = false;
    for (const char *line = old; *line;) {
        const size_t length = strcspn(line, "\n");
        if (line[0] == '[') skipping = strncmp(line, header, strlen(header)) == 0;
        if (!skipping) {
            out.append_n(line, length);
            out.append("\n");
        }
        line += length;
        if (*line == '\n') line++;
    }
    if (section != NULL) out.append(section);
    if (!out.valid()) {
        upload_zero(old, strlen(old));
        free(old);
        snprintf(error, error_size, T(S_UPLOAD_CRED_FAILED), strerror(ENOMEM));
        return false;
    }
    bool ok = false;
    int fd = -1;
    if (snprintf(temporary, sizeof(temporary), "%s.web.new.%ld", path, (long)getpid()) < (int)sizeof(temporary)) {
        (void)unlink(temporary);
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        ok = fd >= 0 && write_all(fd, out.data(), out.size()) && fsync(fd) == 0 && close(fd) == 0 &&
             rename(temporary, path) == 0;
        if (!ok) {
            snprintf(error, error_size, T(S_UPLOAD_CRED_FAILED), strerror(errno));
            if (fd >= 0) close(fd);
            (void)unlink(temporary);
        }
    } else {
        snprintf(error, error_size, T(S_UPLOAD_CRED_FAILED), strerror(ENAMETOOLONG));
    }
    /* The copies hold the old and the new secret and token. */
    upload_zero(old, strlen(old));
    upload_zero((void *)out.data(), out.size());
    free(old);
    return ok;
}

/* Each request is its own process, so two saves at once (the Google and the Dropbox form, say) would each
 * read the file, change their own section and rename, and one section would be lost. A lock on a separate
 * file, held for the whole read, change and rename, makes the second wait for the first. */
static bool upload_replace_section(const char *path, const char *name, const char *section,
                                   char *error, size_t error_size)
{
    char lock_path[PATH_MAX];
    int lock_fd = -1;
    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", path) < (int)sizeof(lock_path))
        lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX) != 0) {
        snprintf(error, error_size, T(S_UPLOAD_CRED_FAILED), strerror(errno));
        if (lock_fd >= 0) close(lock_fd);
        return false;
    }
    const bool ok = upload_replace_section_locked(path, name, section, error, error_size);
    close(lock_fd); /* releases the lock */
    return ok;
}

/* Pasted values often carry a trailing space or line break. */
static void upload_trim(char *text, size_t *length)
{
    while (*length > 0 && (text[*length - 1] == ' ' || text[*length - 1] == '\t' || text[*length - 1] == '\r' || text[*length - 1] == '\n'))
        (*length)--;
    size_t skip = 0;
    while (skip < *length && (text[skip] == ' ' || text[skip] == '\t' || text[skip] == '\r' || text[skip] == '\n')) skip++;
    memmove(text, text + skip, *length - skip);
    *length -= skip;
    text[*length] = '\0';
}

static bool upload_credentials_update(const APC_HTTPRequest *request, bool *removed,
                                      char *error, size_t error_size)
{
    size_t action_length = 0, id_length = 0, secret_length = 0, token_length = 0, service_length = 0;
    char *action = form_value(request, "action", &action_length);
    char *service_name = form_value(request, "service", &service_length);
    /* No service named means Google Drive, as before the form had one. */
    const struct upload_service_ui *service = service_name == NULL ? &upload_services[0]
                                                                   : upload_service_named(service_name, service_length);
    char *id = form_value(request, "client_id", &id_length);
    char *secret = form_value(request, "client_secret", &secret_length);
    char *token = form_value(request, "token", &token_length);
    bool ok = false;
    if (id != NULL) upload_trim(id, &id_length);
    if (secret != NULL) upload_trim(secret, &secret_length);
    if (token != NULL) upload_trim(token, &token_length);
    *removed = action != NULL && action_length == strlen("remove") && memcmp(action, "remove", action_length) == 0;
    if (service == NULL) {
        snprintf(error, error_size, "%s", T(S_UPLOAD_CRED_INVALID));
    } else if (*removed) {
        ok = upload_replace_section(upload_rclone_config_path(), service->section, NULL, error, error_size);
    } else if (id == NULL || secret == NULL || token == NULL ||
               !upload_credential_valid(id, id_length, false) ||
               !upload_credential_valid(secret, secret_length, false) ||
               !upload_credential_valid(token, token_length, true)) {
        snprintf(error, error_size, "%s", T(S_UPLOAD_CRED_INVALID));
    } else {
        char section[4096 + 512];
        snprintf(section, sizeof(section), "[%s]\ntype = %s\nclient_id = %s\n"
                 "client_secret = %s\ntoken = %s\n\n", service->section, service->type, id, secret, token);
        ok = upload_replace_section(upload_rclone_config_path(), service->section, section, error, error_size);
        upload_zero(section, sizeof(section));
    }
    if (secret != NULL) upload_zero(secret, secret_length);
    if (token != NULL) upload_zero(token, token_length);
    free(action);
    free(service_name);
    free(id);
    free(secret);
    free(token);
    return ok;
}

/* The credentials inputs sit in the Cloud Upload tab but belong to this form,
 * which comes before the parameters form because forms cannot be nested. */
static void append_upload_credentials_form(APC_StringBuffer *page)
{
    page->appendf("<form id=upload-credentials method=post action=/upload-credentials data-return-tab>"
                  "<input type=hidden name=csrf value=\"%s\"><input type=hidden name=service value=gdrive></form>"
                  "<form id=upload-credentials-dropbox method=post action=/upload-credentials data-return-tab>"
                  "<input type=hidden name=csrf value=\"%s\"><input type=hidden name=service value=dropbox></form>"
                  "<form id=upload-now method=post action=/upload-now data-return-tab>"
                  "<input type=hidden name=csrf value=\"%s\"></form>", csrf_token, csrf_token, csrf_token);
}

/* The status comes first in the panel, across the whole width of the fields grid. */
static void append_upload_panel_top(APC_StringBuffer *page)
{
    page->append("<div style=\"grid-column:1/-1\">");
    append_upload_status(page);
    page->append("</div>");
}

#endif
