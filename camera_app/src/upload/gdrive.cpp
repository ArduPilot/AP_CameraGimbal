/* Google Drive upload backend: OAuth refresh-token exchange, folder lookup
 * and creation, resumable uploads and an MD5 check of every upload, over the
 * HTTPS client. The credentials are read from the rclone-format file the web
 * page writes (client_id, client_secret and a token holding a refresh_token). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/https.h"
#include "camera_app/upload_backend.h"
#include "upload_util.h"

#include <psa/crypto.h>

#include <errno.h>
#include <new>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define API_HOST "www.googleapis.com"
#define AUTH_HOST "oauth2.googleapis.com"
#define CHUNK_BYTES (4U * 1024U * 1024U) /* a multiple of 256 KiB, as Drive requires */
#define ATTEMPTS 4
#define FOLDER_CACHE 32
#define BODY_BYTES 16384
#define FOLDER_MIME "application/vnd.google-apps.folder"

/* A value in a Drive search query: backslash and quote are escaped. */
static bool query_escape(char *out, size_t size, const char *text)
{
    size_t used = 0;
    for (const char *p = text; *p; p++) {
        if ((*p == '\\' || *p == '\'') && used + 2 < size) out[used++] = '\\';
        if (used + 1 >= size) return false;
        out[used++] = *p;
    }
    out[used] = '\0';
    return true;
}

/* ---- MD5 of a local file ---- */

/* A large recording takes a while to read, so `stop` is checked on every read: `*stopped` is set and false returned. */
static bool file_md5(const char *path, char out[33], uint64_t *size_out, const std::atomic<bool> *stop, bool *stopped)
{
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_MD5) != PSA_SUCCESS) { close(fd); return false; }
    unsigned char buffer[16384];
    uint64_t total = 0;
    for (;;) {
        if (stop && stop->load()) { *stopped = true; psa_hash_abort(&hash); close(fd); return false; }
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { psa_hash_abort(&hash); close(fd); return false; }
        if (n == 0) break;
        if (psa_hash_update(&hash, buffer, (size_t)n) != PSA_SUCCESS) { psa_hash_abort(&hash); close(fd); return false; }
        total += (uint64_t)n;
    }
    close(fd);
    unsigned char digest[16];
    size_t length = 0;
    if (psa_hash_finish(&hash, digest, sizeof(digest), &length) != PSA_SUCCESS || length != 16) return false;
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
    if (size_out) *size_out = total;
    return true;
}

/* ---- the backend ---- */

namespace {

struct FolderEntry {
    char path[256];
    char id[80];
};

class APC_GDriveBackend : public APC_UploadBackend {
public:
    ~APC_GDriveBackend() override
    {
        ca_https_close(_api);
        explicit_bzero(_client_secret, sizeof(_client_secret));
        explicit_bzero(_refresh_token, sizeof(_refresh_token));
        explicit_bzero(_access_token, sizeof(_access_token));
    }
    bool init(const ca_upload_target *target, char *error, size_t error_size);
    ca_put_result put(const char *remote_dir, const char *name, const char *local_path,
                      void (*progress)(void *, uint64_t), void *user, char *error, size_t error_size) override;
    void set_bandwidth(unsigned kib_s) override
    {
        _bandwidth = kib_s;
        if (_api) ca_https_set_bandwidth(_api, kib_s);
    }

private:
    bool stopped() const { return _stop && _stop->load(); }
    bool pause_ms(unsigned ms);
    bool refresh_access_token(char *error, size_t error_size);
    int call(const char *method, const char *path, const char *headers, const void *body, size_t length,
             ca_https_response &response, char *error, size_t error_size, bool once = false);
    int resolve_folder(const char *path, char *id, size_t id_size, char *error, size_t error_size);
    int find_child(const char *parent, const char *name, bool folder, ca_https_response &response,
                   char *error, size_t error_size);
    int create_folder(const char *parent, const char *name, char *id, size_t id_size, char *error, size_t error_size);
    ca_put_result send_file(const char *parent, const char *existing_id, const char *name, int fd, uint64_t size,
                            void (*progress)(void *, uint64_t), void *user, ca_https_response &final,
                            char *error, size_t error_size);

    char _ca_file[256] = "", _root[256] = "";
    char _client_id[256] = "", _client_secret[256] = "", _refresh_token[512] = "", _access_token[2048] = "";
    char _api_host[128] = API_HOST, _auth_host[128] = AUTH_HOST, _server_name[128] = "";
    unsigned _api_port = 443, _auth_port = 443, _bandwidth = 0;
    const std::atomic<bool> *_stop = NULL;
    uint64_t _token_expires_ms = 0;
    bool _credentials_rejected = false; /* Google answered 400 or 401 to the token request */
    ca_https *_api = NULL;
    FolderEntry _folders[FOLDER_CACHE] = {};
    unsigned _folder_count = 0;
    char _body[BODY_BYTES] = "";
    char _scratch[BODY_BYTES] = "";
};

bool APC_GDriveBackend::init(const ca_upload_target *t, char *error, size_t error_size)
{
    if (!t->credentials_path || !t->credentials_section || !t->ca_file || !t->root_folder || !t->root_folder[0] ||
        strlen(t->ca_file) >= sizeof(_ca_file) || strlen(t->root_folder) >= sizeof(_root)) {
        set_error(error, error_size, "bad upload settings");
        return false;
    }
    snprintf(_ca_file, sizeof(_ca_file), "%s", t->ca_file);
    snprintf(_root, sizeof(_root), "%s", t->root_folder);
    _stop = t->stop;
    _bandwidth = t->bandwidth_kib_s;
    if (t->api_host && t->api_host[0]) snprintf(_api_host, sizeof(_api_host), "%s", t->api_host);
    if (t->auth_host && t->auth_host[0]) snprintf(_auth_host, sizeof(_auth_host), "%s", t->auth_host);
    if (t->server_name && t->server_name[0]) snprintf(_server_name, sizeof(_server_name), "%s", t->server_name);
    if (t->api_port) _api_port = t->api_port;
    if (t->auth_port) _auth_port = t->auth_port;

    char token[4352]; /* the web page accepts a token of up to 4096 characters */
    if (!config_value(t->credentials_path, t->credentials_section, "client_id", _client_id, sizeof(_client_id)) ||
        !config_value(t->credentials_path, t->credentials_section, "client_secret", _client_secret, sizeof(_client_secret)) ||
        !config_value(t->credentials_path, t->credentials_section, "token", token, sizeof(token)) ||
        !json_string(token, "refresh_token", _refresh_token, sizeof(_refresh_token)) || !_refresh_token[0]) {
        explicit_bzero(token, sizeof(token));
        set_error(error, error_size, "No Google Drive credentials saved");
        return false;
    }
    explicit_bzero(token, sizeof(token));
    return true;
}

/* Waits, noticing the stop flag. False if stopped. */
bool APC_GDriveBackend::pause_ms(unsigned ms)
{
    for (unsigned waited = 0; waited < ms; waited += 50) {
        if (stopped()) return false;
        usleep(50000);
    }
    return !stopped();
}

bool APC_GDriveBackend::refresh_access_token(char *error, size_t error_size)
{
    ca_https_options options = {};
    options.ca_file = _ca_file;
    options.server_name = _server_name[0] ? _server_name : NULL;
    options.stop = _stop;
    ca_https *auth = ca_https_connect(_auth_host, _auth_port, &options, error, error_size);
    if (!auth) return false;
    char form[1536];
    size_t used = 0;
    form[0] = '\0';
    bool ok = url_encode(form, sizeof(form), &used, "client_id") && (form[used++] = '=', true) &&
              url_encode(form, sizeof(form), &used, _client_id) && (form[used++] = '&', true) &&
              url_encode(form, sizeof(form), &used, "client_secret") && (form[used++] = '=', true) &&
              url_encode(form, sizeof(form), &used, _client_secret) && (form[used++] = '&', true) &&
              url_encode(form, sizeof(form), &used, "refresh_token") && (form[used++] = '=', true) &&
              url_encode(form, sizeof(form), &used, _refresh_token);
    static const char tail[] = "&grant_type=refresh_token";
    if (!ok || used + sizeof(tail) > sizeof(form)) {
        ca_https_close(auth);
        set_error(error, error_size, "credentials too long");
        return false;
    }
    memcpy(form + used, tail, sizeof(tail));
    ca_https_req q = {};
    q.method = "POST";
    q.path = "/token";
    q.headers = "Content-Type: application/x-www-form-urlencoded\r\n";
    q.body = form;
    q.length = strlen(form);
    q.fd = -1;
    ca_https_response r = {};
    r.body = _scratch;
    r.body_capacity = sizeof(_scratch);
    const int rc = ca_https_request(auth, &q, &r, error, error_size);
    ca_https_close(auth);
    explicit_bzero(form, sizeof(form));
    if (rc != CA_HTTPS_OK) return false;
    double expires = 3600;
    if (r.status != 200 || !json_string(_scratch, "access_token", _access_token, sizeof(_access_token))) {
        char code[40] = "", description[80] = "";
        json_string(_scratch, "error", code, sizeof(code));
        json_string(_scratch, "error_description", description, sizeof(description));
        _credentials_rejected = r.status == 400 || r.status == 401;
        if (!strcmp(code, "invalid_grant")) {
            /* The usual cause is an app left in Testing mode, whose tokens last 7 days. */
            set_error(error, error_size, "Google refused the credentials: the token has expired or been revoked. "
                      "Authorise again (an app in Testing mode expires tokens after 7 days)");
        } else {
            set_error(error, error_size, "Google %s the credentials (HTTP %d%s%s%s%s)",
                      _credentials_rejected ? "refused" : "could not check", r.status, code[0] ? ": " : "", code,
                      description[0] ? ": " : "", description);
        }
        return false;
    }
    json_number(_scratch, "expires_in", &expires);
    _credentials_rejected = false;
    _token_expires_ms = monotonic_ms() + (expires > 120 ? (uint64_t)(expires - 60) * 1000U : 60000U);
    return true;
}

/* One request to the API, with a valid token. Returns 0 with the response (any
 * status that is not worth retrying), or a negative ca_https_error. A 401 gets a
 * new token; 429, 5xx and lost connections are retried after a pause, unless `once`: a request that
 * creates something must not be repeated when its reply may only have been lost. */
int APC_GDriveBackend::call(const char *method, const char *path, const char *headers, const void *body,
                           size_t length, ca_https_response &response, char *error, size_t error_size, bool once)
{
    int last = CA_HTTPS_ERR_IO;
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        if (stopped()) return CA_HTTPS_ERR_STOPPED;
        if (attempt && !pause_ms(1000U << (attempt - 1))) return CA_HTTPS_ERR_STOPPED;
        if (!_access_token[0] || monotonic_ms() >= _token_expires_ms) {
            if (!refresh_access_token(error, error_size)) {
                if (stopped()) return CA_HTTPS_ERR_STOPPED;
                last = CA_HTTPS_ERR_TLS;
                /* Refused credentials will not get better by asking again. */
                if (_credentials_rejected) return last;
                continue;
            }
        }
        if (!_api) {
            ca_https_options options = {};
            options.ca_file = _ca_file;
            options.server_name = _server_name[0] ? _server_name : NULL;
            options.bandwidth_kib_s = _bandwidth;
            options.stop = _stop;
            options.timeout_ms = 60000;
            _api = ca_https_connect(_api_host, _api_port, &options, error, error_size);
            if (!_api) {
                if (stopped()) return CA_HTTPS_ERR_STOPPED;
                last = CA_HTTPS_ERR_CONNECT;
                continue;
            }
        }
        char head[2600];
        snprintf(head, sizeof(head), "Authorization: Bearer %s\r\n%s", _access_token, headers ? headers : "");
        ca_https_req q = {};
        q.method = method;
        q.path = path;
        q.headers = head;
        q.body = body;
        q.length = length;
        q.fd = -1;
        response = ca_https_response {};
        response.body = _body;
        response.body_capacity = sizeof(_body);
        const int rc = ca_https_request(_api, &q, &response, error, error_size);
        explicit_bzero(head, sizeof(head));
        if (rc == CA_HTTPS_ERR_STOPPED) return rc;
        if (rc != CA_HTTPS_OK) {
            ca_https_close(_api);
            _api = NULL;
            last = rc;
            if (once) return last;
            continue;
        }
        if (response.status == 401) {
            _access_token[0] = '\0';
            set_error(error, error_size, "Google rejected the access token");
            last = CA_HTTPS_ERR_TLS;
            continue;
        }
        if (response.status == 429 || response.status >= 500) {
            set_error(error, error_size, "Google Drive returned HTTP %d", response.status);
            last = CA_HTTPS_ERR_IO;
            if (once) return last;
            continue;
        }
        return 0;
    }
    return last;
}

/* Looks for a child by name (and kind) in a parent folder. The reply is left in _body. */
int APC_GDriveBackend::find_child(const char *parent, const char *name, bool folder, ca_https_response &response,
                                 char *error, size_t error_size)
{
    char escaped[640], query[900], path[1800];
    if (!query_escape(escaped, sizeof(escaped), name)) {
        set_error(error, error_size, "name too long: %.40s", name);
        return CA_HTTPS_ERR_ARGS;
    }
    snprintf(query, sizeof(query), "name = '%s' and '%s' in parents and trashed = false%s", escaped, parent,
             folder ? " and mimeType = '" FOLDER_MIME "'" : " and mimeType != '" FOLDER_MIME "'");
    size_t used = (size_t)snprintf(path, sizeof(path), "/drive/v3/files?pageSize=5&spaces=drive&fields=");
    if (!url_encode(path, sizeof(path), &used, "files(id,name,md5Checksum,size)")) return CA_HTTPS_ERR_ARGS;
    used += (size_t)snprintf(path + used, sizeof(path) - used, "&q=");
    if (!url_encode(path, sizeof(path), &used, query)) {
        set_error(error, error_size, "name too long: %.40s", name);
        return CA_HTTPS_ERR_ARGS;
    }
    const int rc = call("GET", path, NULL, NULL, 0, response, error, error_size);
    if (rc == 0 && response.status != 200) {
        set_error(error, error_size, "Google Drive search returned HTTP %d", response.status);
        return CA_HTTPS_ERR_PROTOCOL;
    }
    return rc;
}

int APC_GDriveBackend::create_folder(const char *parent, const char *name, char *id, size_t id_size,
                                    char *error, size_t error_size)
{
    char body[900];
    size_t used = 0;
    used += (size_t)snprintf(body, sizeof(body), "{\"mimeType\":\"" FOLDER_MIME "\",\"parents\":[\"%s\"],\"name\":\"", parent);
    if (!json_escape(body, sizeof(body) - 3, &used, name)) {
        set_error(error, error_size, "name too long: %.40s", name);
        return CA_HTTPS_ERR_ARGS;
    }
    memcpy(body + used, "\"}", 3);
    ca_https_response r;
    /* Once: if the reply is lost after Drive made the folder, resolve_folder() looks for it before trying again. */
    const int rc = call("POST", "/drive/v3/files?fields=id", "Content-Type: application/json; charset=UTF-8\r\n", body,
                        strlen(body), r, error, error_size, true);
    if (rc != 0) return rc;
    if ((r.status != 200 && r.status != 201) || !json_string(_body, "id", id, id_size)) {
        set_error(error, error_size, "Google Drive could not create folder %.40s (HTTP %d)", name, r.status);
        return CA_HTTPS_ERR_PROTOCOL;
    }
    return 0;
}

/* The id of a folder path below My Drive, creating what is missing. The ids are cached. */
int APC_GDriveBackend::resolve_folder(const char *path, char *id, size_t id_size, char *error, size_t error_size)
{
    for (unsigned i = 0; i < _folder_count; i++) {
        if (!strcmp(_folders[i].path, path)) {
            snprintf(id, id_size, "%s", _folders[i].id);
            return 0;
        }
    }
    char parent[80] = "root", prefix[256] = "";
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const size_t length = strcspn(p, "/");
        char name[200];
        if (length >= sizeof(name)) { set_error(error, error_size, "folder name too long"); return CA_HTTPS_ERR_ARGS; }
        memcpy(name, p, length);
        name[length] = '\0';
        p += length;
        const size_t prefix_length = strlen(prefix);
        if (prefix_length + length + 2 > sizeof(prefix)) { set_error(error, error_size, "folder path too long"); return CA_HTTPS_ERR_ARGS; }
        snprintf(prefix + prefix_length, sizeof(prefix) - prefix_length, "%s%s", prefix_length ? "/" : "", name);

        char found[80] = "";
        for (unsigned i = 0; i < _folder_count && !found[0]; i++)
            if (!strcmp(_folders[i].path, prefix)) snprintf(found, sizeof(found), "%s", _folders[i].id);
        if (!found[0]) {
            ca_https_response r;
            int rc = find_child(parent, name, true, r, error, error_size);
            if (rc != 0) return rc;
            if (!json_string(_body, "id", found, sizeof(found))) {
                rc = create_folder(parent, name, found, sizeof(found), error, error_size);
                if (rc != 0) {
                    /* A reply may have been lost after Drive made the folder: use it
                     * rather than leave a second one behind on the retry. */
                    if (rc == CA_HTTPS_ERR_STOPPED || find_child(parent, name, true, r, error, error_size) != 0 ||
                        !json_string(_body, "id", found, sizeof(found))) return rc ? rc : CA_HTTPS_ERR_IO;
                }
            }
            if (_folder_count < FOLDER_CACHE) {
                snprintf(_folders[_folder_count].path, sizeof(_folders[_folder_count].path), "%s", prefix);
                snprintf(_folders[_folder_count].id, sizeof(_folders[_folder_count].id), "%s", found);
                _folder_count++;
            }
        }
        snprintf(parent, sizeof(parent), "%s", found);
    }
    snprintf(id, id_size, "%s", parent);
    return 0;
}

/* Sends the file through a resumable session and leaves the final reply in `final`. */
ca_put_result APC_GDriveBackend::send_file(const char *parent, const char *existing_id, const char *name, int fd,
                                          uint64_t size, void (*progress)(void *, uint64_t), void *user,
                                          ca_https_response &final, char *error, size_t error_size)
{
    for (int session_try = 0; session_try < 2; session_try++) {
        /* Start the session: new files name their folder, updates name the file. */
        char meta[900], path[160];
        size_t used = 0;
        if (existing_id) {
            snprintf(path, sizeof(path), "/upload/drive/v3/files/%s?uploadType=resumable&fields=id,size,md5Checksum", existing_id);
            snprintf(meta, sizeof(meta), "{}");
        } else {
            snprintf(path, sizeof(path), "/upload/drive/v3/files?uploadType=resumable&fields=id,size,md5Checksum");
            used = (size_t)snprintf(meta, sizeof(meta), "{\"parents\":[\"%s\"],\"name\":\"", parent);
            if (!json_escape(meta, sizeof(meta) - 3, &used, name)) {
                set_error(error, error_size, "name too long: %.40s", name);
                return CA_PUT_REJECTED;
            }
            memcpy(meta + used, "\"}", 3);
        }
        char headers[200];
        snprintf(headers, sizeof(headers), "Content-Type: application/json; charset=UTF-8\r\nX-Upload-Content-Length: %llu\r\n",
                 (unsigned long long)size);
        ca_https_response r;
        int rc = call(existing_id ? "PATCH" : "POST", path, headers, meta, strlen(meta), r, error, error_size);
        if (rc == CA_HTTPS_ERR_STOPPED) return CA_PUT_STOPPED;
        if (rc != 0) return CA_PUT_FAILED;
        if (r.status != 200 || !r.location[0]) {
            set_error(error, error_size, "Google Drive refused the upload (HTTP %d)", r.status);
            return CA_PUT_FAILED;
        }
        /* The session address is https://host/path; only the path is used, on this host. */
        const char *session = strstr(r.location, "://");
        session = session ? strchr(session + 3, '/') : NULL;
        if (!session) {
            set_error(error, error_size, "Google Drive gave an unusable upload address");
            return CA_PUT_FAILED;
        }
        char session_path[512];
        snprintf(session_path, sizeof(session_path), "%s", session);

        uint64_t offset = 0;
        int failures = 0;
        bool restart = false;
        while (!restart) {
            const uint64_t count = size - offset < CHUNK_BYTES ? size - offset : CHUNK_BYTES;
            char range[96];
            if (count == 0) snprintf(range, sizeof(range), "Content-Range: bytes */%llu\r\n", (unsigned long long)size);
            else snprintf(range, sizeof(range), "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long)offset,
                          (unsigned long long)(offset + count - 1), (unsigned long long)size);
            struct Progress { void (*fn)(void *, uint64_t); void *user; uint64_t base; } p = {progress, user, offset};
            ca_https_req q = {};
            q.method = "PUT";
            q.path = session_path;
            q.headers = range;
            q.fd = fd;
            q.offset = (off_t)offset;
            q.length = count;
            if (progress) {
                q.progress = [](void *context, uint64_t sent) {
                    Progress *s = (Progress *)context;
                    s->fn(s->user, s->base + sent);
                };
                q.progress_user = &p;
            }
            ca_https_response chunk = {};
            chunk.body = _body;
            chunk.body_capacity = sizeof(_body);
            if (!_api) {
                ca_https_options options = {};
                options.ca_file = _ca_file;
                options.server_name = _server_name[0] ? _server_name : NULL;
                options.bandwidth_kib_s = _bandwidth;
                options.stop = _stop;
                options.timeout_ms = 60000;
                _api = ca_https_connect(_api_host, _api_port, &options, error, error_size);
            }
            rc = _api ? ca_https_request(_api, &q, &chunk, error, error_size) : CA_HTTPS_ERR_CONNECT;
            if (rc == CA_HTTPS_ERR_STOPPED) return CA_PUT_STOPPED;
            if (rc == CA_HTTPS_ERR_FILE) return CA_PUT_FAILED;
            if (rc == CA_HTTPS_OK && (chunk.status == 200 || chunk.status == 201)) {
                final = chunk;
                return CA_PUT_UPLOADED;
            }
            if (rc == CA_HTTPS_OK && chunk.status == 308) {
                uint64_t next = 0;
                const char *dash = strrchr(chunk.range, '-');
                if (dash) next = strtoull(dash + 1, NULL, 10) + 1;
                if (next < offset || next > size) next = offset;
                if (next > offset) {
                    offset = next;
                    failures = 0;
                    continue;
                }
                /* Nothing more was kept: count it, so this cannot repeat for ever. */
                set_error(error, error_size, "Google Drive did not accept the data");
                if (++failures >= ATTEMPTS) return CA_PUT_FAILED;
                if (!pause_ms(1000U << (failures - 1))) return CA_PUT_STOPPED;
                continue;
            }
            if (rc == CA_HTTPS_OK && (chunk.status == 404 || chunk.status == 410)) {
                restart = true; /* the session is gone: start over once */
                set_error(error, error_size, "upload session expired");
                break;
            }
            if (rc == CA_HTTPS_OK && chunk.status >= 400 && chunk.status < 500 && chunk.status != 429) {
                set_error(error, error_size, "Google Drive refused the data (HTTP %d)", chunk.status);
                return CA_PUT_FAILED;
            }
            /* A lost connection or a server error: ask how much arrived, then go on from there. */
            if (rc != CA_HTTPS_OK) {
                ca_https_close(_api);
                _api = NULL;
            } else {
                set_error(error, error_size, "Google Drive returned HTTP %d", chunk.status);
            }
            if (++failures >= ATTEMPTS) return CA_PUT_FAILED;
            if (!pause_ms(1000U << (failures - 1))) return CA_PUT_STOPPED;
            ca_https_req ask = {};
            char ask_range[96];
            snprintf(ask_range, sizeof(ask_range), "Content-Range: bytes */%llu\r\n", (unsigned long long)size);
            ask.method = "PUT";
            ask.path = session_path;
            ask.headers = ask_range;
            ask.fd = -1;
            ca_https_response status = {};
            status.body = _body;
            status.body_capacity = sizeof(_body);
            if (!_api) {
                ca_https_options options = {};
                options.ca_file = _ca_file;
                options.server_name = _server_name[0] ? _server_name : NULL;
                options.bandwidth_kib_s = _bandwidth;
                options.stop = _stop;
                options.timeout_ms = 60000;
                _api = ca_https_connect(_api_host, _api_port, &options, error, error_size);
                if (!_api) continue;
            }
            rc = ca_https_request(_api, &ask, &status, error, error_size);
            if (rc == CA_HTTPS_ERR_STOPPED) return CA_PUT_STOPPED;
            if (rc != CA_HTTPS_OK) {
                ca_https_close(_api);
                _api = NULL;
                continue;
            }
            if (status.status == 200 || status.status == 201) {
                final = status;
                return CA_PUT_UPLOADED;
            }
            if (status.status == 308) {
                const char *dash = strrchr(status.range, '-');
                offset = dash ? strtoull(dash + 1, NULL, 10) + 1 : 0;
                if (offset > size) offset = 0;
            } else if (status.status == 404 || status.status == 410) {
                restart = true;
            }
        }
        if (!restart) break;
    }
    return CA_PUT_FAILED;
}

ca_put_result APC_GDriveBackend::put(const char *remote_dir, const char *name, const char *local_path,
                                    void (*progress)(void *, uint64_t), void *user, char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    if (stopped()) return CA_PUT_STOPPED;
    char folder_path[512];
    snprintf(folder_path, sizeof(folder_path), "%s%s%s", _root, remote_dir && remote_dir[0] ? "/" : "",
             remote_dir ? remote_dir : "");
    char parent[80];
    int rc = resolve_folder(folder_path, parent, sizeof(parent), error, error_size);
    if (rc == CA_HTTPS_ERR_STOPPED) return CA_PUT_STOPPED;
    if (rc != 0) return rc == CA_HTTPS_ERR_ARGS ? CA_PUT_REJECTED : CA_PUT_FAILED; /* a folder name it cannot take: skip */

    const int fd = open(local_path, O_RDONLY | O_CLOEXEC);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        if (fd >= 0) close(fd);
        set_error(error, error_size, "cannot read %.60s", name);
        return CA_PUT_REJECTED;
    }
    const uint64_t size = (uint64_t)info.st_size;

    /* Is it already there? */
    ca_https_response found;
    rc = find_child(parent, name, false, found, error, error_size);
    if (rc != 0) {
        close(fd);
        return rc == CA_HTTPS_ERR_STOPPED ? CA_PUT_STOPPED : rc == CA_HTTPS_ERR_ARGS ? CA_PUT_REJECTED : CA_PUT_FAILED;
    }
    char existing[80] = "", remote_md5[40] = "", remote_size[24] = "";
    if (json_string(_body, "id", existing, sizeof(existing))) {
        json_string(_body, "md5Checksum", remote_md5, sizeof(remote_md5));
        json_string(_body, "size", remote_size, sizeof(remote_size));
    }
    char local_md5[33];
    uint64_t hashed = 0;
    bool hash_stopped = false;
    if (!file_md5(local_path, local_md5, &hashed, _stop, &hash_stopped)) {
        close(fd);
        if (hash_stopped) return CA_PUT_STOPPED;
        set_error(error, error_size, "cannot read %.60s", name);
        return CA_PUT_REJECTED;
    }
    if (existing[0] && !strcasecmp(remote_md5, local_md5) && strtoull(remote_size, NULL, 10) == hashed) {
        close(fd);
        return CA_PUT_SKIPPED;
    }

    ca_put_result result;
    ca_https_response final = {};
    if (size == 0) {
        /* Nothing to stream: a metadata-only create (or nothing to change for an empty existing file). */
        char body[900];
        size_t used = (size_t)snprintf(body, sizeof(body), "{\"parents\":[\"%s\"],\"name\":\"", parent);
        if (existing[0]) {
            /* The identical empty file was skipped above, so what is there differs. An empty local file (an
             * interrupted capture, say) must not replace it, and is not worth failing for: leave both. */
            close(fd);
            return CA_PUT_LEFT;
        }
        if (!json_escape(body, sizeof(body) - 3, &used, name)) {
            close(fd);
            set_error(error, error_size, "name too long: %.40s", name);
            return CA_PUT_REJECTED;
        }
        memcpy(body + used, "\"}", 3);
        /* Once: if the reply is lost after Drive made the file, the next scan finds it rather than making another. */
        rc = call("POST", "/drive/v3/files?fields=id,size,md5Checksum", "Content-Type: application/json; charset=UTF-8\r\n",
                  body, strlen(body), final, error, error_size, true);
        close(fd);
        if (rc == CA_HTTPS_ERR_STOPPED) return CA_PUT_STOPPED;
        if (rc != 0 || (final.status != 200 && final.status != 201)) {
            if (rc == 0) set_error(error, error_size, "Google Drive refused the file (HTTP %d)", final.status);
            return CA_PUT_FAILED;
        }
        return CA_PUT_UPLOADED;
    }
    result = send_file(parent, existing[0] ? existing : NULL, name, fd, size, progress, user, final, error, error_size);
    close(fd);
    if (result != CA_PUT_UPLOADED) return result;

    /* The reply carries what Drive stored. Delete nothing locally until it matches. */
    char got_md5[40] = "", got_size[24] = "";
    json_string(_body, "md5Checksum", got_md5, sizeof(got_md5));
    json_string(_body, "size", got_size, sizeof(got_size));
    if (strcasecmp(got_md5, local_md5) != 0 || strtoull(got_size, NULL, 10) != hashed) {
        set_error(error, error_size, "%.60s did not match after upload (the file may have changed)", name);
        return CA_PUT_FAILED;
    }
    return CA_PUT_UPLOADED;
}

} // namespace

APC_UploadBackend *ca_gdrive_backend_create(const ca_upload_target *target, char *error, size_t error_size)
{
    APC_GDriveBackend *backend = new (std::nothrow) APC_GDriveBackend();
    if (!backend) {
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    if (!backend->init(target, error, error_size)) {
        delete backend;
        return NULL;
    }
    return backend;
}
