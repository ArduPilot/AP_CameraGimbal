#pragma once
// Cloud upload on the Parameters page, included by mt11-web.cpp where the file,
// form and translation helpers it uses are already defined.
#include "APC_StringBuffer.h"

#if APCAM_HAVE_UPLOAD
/* Cloud upload: the camera app runs rclone. This page
 * shows its status and stores the user's own Google Drive credentials in the
 * rclone config file, which only the camera software can read. */
#define UPLOAD_REMOTE_NAME APCAM_UPLOAD_REMOTE_NAME

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

/* The status paragraph, also fetched on its own by the page every few seconds.
 * state is set to the uploader's state, or "none" when there is no status. */
static void append_upload_status_text(APC_StringBuffer *page, char state[16])
{
    char path[sizeof(CAMERA_READY_PATH) + 8], reason[128], success[24], error[192];
    char done[16], total[16], bytes_done[24], bytes_total[24];
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
            /* rclone has not reported any files to send (yet): it is comparing with the remote. */
            page->append_html(T(S_UPLOAD_CHECKING));
        }
    } else {
        for (const auto &entry : states) {
            if (!strcmp(state, entry.name)) page->append_html(T(entry.label));
        }
    }
    if (!strcmp(state, "disabled") && reason[0] && strcmp(reason, "Disabled") != 0) {
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

static bool upload_credentials_saved(void)
{
    size_t size;
    char *config = read_file(upload_rclone_config_path(), 65536, &size);
    const bool saved = config != NULL &&
        (strncmp(config, "[" UPLOAD_REMOTE_NAME "]", sizeof("[" UPLOAD_REMOTE_NAME "]") - 1) == 0 ||
         strstr(config, "\n[" UPLOAD_REMOTE_NAME "]") != NULL);
    free(config);
    return saved;
}

/* The client ID saved in the remote's section, if it is plain enough to put in
 * the page unescaped (the save path only allows these characters). */
static void upload_saved_client_id(char *out, size_t size)
{
    size_t file_size;
    char *config = read_file(upload_rclone_config_path(), 65536, &file_size);
    out[0] = '\0';
    bool inside = false;
    for (const char *line = config; line != NULL && *line;) {
        const size_t length = strcspn(line, "\n");
        if (line[0] == '[') {
            inside = strncmp(line, "[" UPLOAD_REMOTE_NAME "]", sizeof("[" UPLOAD_REMOTE_NAME "]") - 1) == 0;
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

/* The inputs belong to the separate upload-credentials form, which sits
 * before the parameters form because forms cannot be nested. */
static void append_upload_credentials(APC_StringBuffer *page)
{
    char id[208] = "";
    const bool saved = upload_credentials_saved();
    if (saved) upload_saved_client_id(id, sizeof(id));
    const char *replace = saved ? T(S_UPLOAD_CRED_REPLACE) : "";
    page->appendf("<h3>%s</h3><p class=help>", T(S_UPLOAD_CRED_HEADING));
    page->append_html(T(S_UPLOAD_CRED_HELP));
    page->append("</p><p class=notice id=upload-credentials-status>");
    page->append_html(T(saved ? S_UPLOAD_CRED_SAVED_FOR : S_UPLOAD_CRED_NONE));
    /* The secret and token are never sent back. The client ID is not secret, so it is shown.
     * new-password keeps password managers from filling the login into these boxes. */
    page->appendf("</p><p><label>%s<br><input form=upload-credentials name=client_id size=60 maxlength=200 "
                  "autocomplete=off spellcheck=false value=\"%s\"></label></p>", T(S_UPLOAD_CRED_ID), id);
    page->appendf("<p><label>%s<br><input form=upload-credentials name=client_secret type=password size=60 "
                  "maxlength=200 autocomplete=new-password placeholder=\"", T(S_UPLOAD_CRED_SECRET));
    page->append_html(replace);
    page->appendf("\"></label></p><p><label>%s<br><textarea form=upload-credentials name=token rows=4 cols=60 "
                  "style=\"min-height:0;height:6.5em\" maxlength=4096 autocomplete=off spellcheck=false placeholder=\"", T(S_UPLOAD_CRED_TOKEN));
    page->append_html(replace);
    page->append("\"></textarea></label></p>");
    page->appendf("<p><button type=submit form=upload-credentials name=action value=save>%s</button> "
                  "<button type=submit form=upload-credentials name=action value=remove formnovalidate>%s</button></p>",
                  T(S_UPLOAD_CRED_SAVE), T(S_UPLOAD_CRED_REMOVE));
}

static bool upload_credential_valid(const char *text, size_t length, bool token)
{
    if (token) {
        if (length < 2 || length > 4096 || text[0] != '{' || text[length - 1] != '}' ||
            strstr(text, "\"access_token\"") == NULL) return false;
        for (size_t i = 0; i < length; i++) {
            if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e) return false;
        }
        return true;
    }
    return length >= 1 && length <= 200 &&
           strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._~-") == length;
}

/* Replace (or with a NULL section remove) one section of the rclone config,
 * keeping any others. The file is written 0600 and swapped in atomically. */
static bool upload_replace_section(const char *path, const char *name, const char *section,
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
        explicit_bzero(old, strlen(old));
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
    explicit_bzero(old, strlen(old));
    explicit_bzero((void *)out.data(), out.size());
    free(old);
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
    size_t action_length = 0, id_length = 0, secret_length = 0, token_length = 0;
    char *action = form_value(request, "action", &action_length);
    char *id = form_value(request, "client_id", &id_length);
    char *secret = form_value(request, "client_secret", &secret_length);
    char *token = form_value(request, "token", &token_length);
    bool ok = false;
    if (id != NULL) upload_trim(id, &id_length);
    if (secret != NULL) upload_trim(secret, &secret_length);
    if (token != NULL) upload_trim(token, &token_length);
    *removed = action != NULL && action_length == strlen("remove") && memcmp(action, "remove", action_length) == 0;
    if (*removed) {
        ok = upload_replace_section(upload_rclone_config_path(), UPLOAD_REMOTE_NAME, NULL, error, error_size);
    } else if (id == NULL || secret == NULL || token == NULL ||
               !upload_credential_valid(id, id_length, false) ||
               !upload_credential_valid(secret, secret_length, false) ||
               !upload_credential_valid(token, token_length, true)) {
        snprintf(error, error_size, "%s", T(S_UPLOAD_CRED_INVALID));
    } else {
        char section[4096 + 512];
        snprintf(section, sizeof(section), "[" UPLOAD_REMOTE_NAME "]\ntype = drive\nclient_id = %s\n"
                 "client_secret = %s\ntoken = %s\n\n", id, secret, token);
        ok = upload_replace_section(upload_rclone_config_path(), UPLOAD_REMOTE_NAME, section, error, error_size);
        explicit_bzero(section, sizeof(section));
    }
    if (secret != NULL) explicit_bzero(secret, secret_length);
    if (token != NULL) explicit_bzero(token, token_length);
    free(action);
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
                  "<input type=hidden name=csrf value=\"%s\"></form>"
                  "<form id=upload-now method=post action=/upload-now data-return-tab>"
                  "<input type=hidden name=csrf value=\"%s\"></form>", csrf_token, csrf_token);
}

/* The status and then the credentials come first in the panel, across the whole width of the fields grid. */
static void append_upload_panel_top(APC_StringBuffer *page)
{
    page->append("<div style=\"grid-column:1/-1\">");
    append_upload_status(page);
    append_upload_credentials(page);
    page->append("</div>");
}

#endif
