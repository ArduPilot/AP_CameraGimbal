/* Dropbox upload backend: OAuth refresh-token exchange, uploads in 4 MiB
 * pieces through upload sessions, and a check of every upload against
 * Dropbox's content hash. The app should have the "App folder" permission, so
 * paths are below the app's own folder. The credentials are read from the
 * rclone-format file the web page writes (client_id and client_secret are the
 * Dropbox app key and secret, and the token holds a refresh_token). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/https.h"
#include "camera_app/upload_backend.h"
#include "upload_util.h"

#include <psa/crypto.h>

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define API_HOST "api.dropboxapi.com"
#define CONTENT_HOST "content.dropboxapi.com"
#define CHUNK_BYTES (4U * 1024U * 1024U) /* Dropbox hashes in 4 MiB blocks */
#define ATTEMPTS 4
#define BODY_BYTES 8192
#define MAX_PATH_BYTES 700 /* leaves room for the header in the HTTPS client's request buffer */
#define MAX_ESCAPED_PATH_BYTES 1100 /* the same, once non-ASCII characters are \uXXXX in the header */

/* ---- the content hash ---- */

/* Dropbox's content_hash: SHA-256 of the concatenated SHA-256 digests of each 4 MiB block. */
/* A large recording takes a while to read, so `stop` is checked on every read: `*stopped` is set and false returned. */
static bool content_hash(int fd, char out[65], uint64_t *size_out, const std::atomic<bool> *stop, bool *stopped)
{
    psa_hash_operation_t total = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&total, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
    unsigned char buffer[16384];
    uint64_t size = 0, in_block = 0;
    psa_hash_operation_t block = PSA_HASH_OPERATION_INIT;
    bool block_open = false;
    auto finish_block = [&]() -> bool {
        unsigned char digest[32];
        size_t length = 0;
        block_open = false;
        return psa_hash_finish(&block, digest, sizeof(digest), &length) == PSA_SUCCESS && length == 32 &&
               psa_hash_update(&total, digest, sizeof(digest)) == PSA_SUCCESS;
    };
    for (off_t offset = 0;;) {
        if (stop && stop->load()) {
            *stopped = true;
            psa_hash_abort(&total);
            if (block_open) psa_hash_abort(&block);
            return false;
        }
        const ssize_t n = pread(fd, buffer, sizeof(buffer), offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { psa_hash_abort(&total); if (block_open) psa_hash_abort(&block); return false; }
        if (n == 0) break;
        offset += n;
        if (!block_open) {
            block = PSA_HASH_OPERATION_INIT;
            if (psa_hash_setup(&block, PSA_ALG_SHA_256) != PSA_SUCCESS) { psa_hash_abort(&total); return false; }
            block_open = true;
        }
        if (psa_hash_update(&block, buffer, (size_t)n) != PSA_SUCCESS) { psa_hash_abort(&total); psa_hash_abort(&block); return false; }
        size += (uint64_t)n;
        in_block += (uint64_t)n;
        if (in_block == CHUNK_BYTES) {
            in_block = 0;
            if (!finish_block()) { psa_hash_abort(&total); return false; }
        }
    }
    if (block_open && !finish_block()) { psa_hash_abort(&total); return false; }
    unsigned char digest[32];
    size_t length = 0;
    if (psa_hash_finish(&total, digest, sizeof(digest), &length) != PSA_SUCCESS || length != 32) return false;
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
    if (size_out) *size_out = size;
    return true;
}

/* Appends `text` as the inside of a JSON string in a header: Dropbox wants the
 * Dropbox-API-Arg header to be ASCII, so anything else becomes \uXXXX. */
static bool json_ascii(std::string &out, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t c = *p++;
        if (c >= 0x80) {
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
            if (more < 0) return false;
            c &= (0x3Fu >> more);
            for (int i = 0; i < more; i++) {
                if ((*p & 0xC0) != 0x80) return false;
                c = (c << 6) | (*p++ & 0x3F);
            }
        }
        char piece[16];
        if (c == '"' || c == '\\') snprintf(piece, sizeof(piece), "\\%c", (char)c);
        else if (c < 0x20 || c == 0x7F) snprintf(piece, sizeof(piece), "\\u%04x", c);
        else if (c < 0x80) snprintf(piece, sizeof(piece), "%c", (char)c);
        else if (c < 0x10000) snprintf(piece, sizeof(piece), "\\u%04x", c);
        else {
            c -= 0x10000;
            snprintf(piece, sizeof(piece), "\\u%04x\\u%04x", 0xD800 + (c >> 10), 0xDC00 + (c & 0x3FF));
        }
        out += piece;
    }
    return true;
}

namespace {

class APC_DropboxBackend : public APC_UploadBackend {
public:
    ~APC_DropboxBackend() override
    {
        ca_https_close(_api);
        ca_https_close(_content);
        explicit_bzero(_secret, sizeof(_secret));
        explicit_bzero(_refresh_token, sizeof(_refresh_token));
        explicit_bzero(_access_token, sizeof(_access_token));
    }
    bool init(const ca_upload_target *target, char *error, size_t error_size);
    ca_put_result put(const char *remote_dir, const char *name, const char *local_path,
                      void (*progress)(void *, uint64_t), void *user, char *error, size_t error_size) override;
    void set_bandwidth(unsigned kib_s) override
    {
        _bandwidth = kib_s;
        if (_content) ca_https_set_bandwidth(_content, kib_s);
    }

private:
    enum Host { API, CONTENT };
    bool stopped() const { return _stop && _stop->load(); }
    bool pause_ms(unsigned ms);
    bool refresh_access_token(char *error, size_t error_size);
    ca_https *&connection(Host host) { return host == API ? _api : _content; }
    int call(Host host, const char *path, const char *headers, const void *body, size_t length, int fd, off_t offset,
             void (*progress)(void *, uint64_t), void *progress_user, ca_https_response &response,
             char *error, size_t error_size);
    ca_put_result upload(const std::string &path, int fd, uint64_t size, void (*progress)(void *, uint64_t), void *user,
                         ca_https_response &final, char *error, size_t error_size);
    ca_put_result upload_session(const std::string &path, int fd, uint64_t size, void (*progress)(void *, uint64_t),
                                 void *user, ca_https_response &final, char *error, size_t error_size);
    int metadata(const std::string &path, ca_https_response &response, char *error, size_t error_size);

    char _ca_file[256] = "", _root[256] = "";
    /* Dropbox access tokens are about 1.3 KB, so these are generous. */
    char _key[128] = "", _secret[128] = "", _refresh_token[512] = "", _access_token[2560] = "";
    char _api_host[128] = API_HOST, _content_host[128] = CONTENT_HOST, _server_name[128] = "";
    unsigned _api_port = 443, _content_port = 443, _bandwidth = 0;
    const std::atomic<bool> *_stop = NULL;
    uint64_t _token_expires_ms = 0;
    bool _credentials_rejected = false;
    ca_https *_api = NULL, *_content = NULL;
    char _body[BODY_BYTES] = "";
    char _scratch[4096] = "";
};

bool APC_DropboxBackend::init(const ca_upload_target *t, char *error, size_t error_size)
{
    if (!t->credentials_path || !t->credentials_section || !t->ca_file || strlen(t->ca_file) >= sizeof(_ca_file) ||
        (t->root_folder && strlen(t->root_folder) >= sizeof(_root))) {
        set_error(error, error_size, "bad upload settings");
        return false;
    }
    snprintf(_ca_file, sizeof(_ca_file), "%s", t->ca_file);
    /* The root folder is below the app's folder: no leading or trailing slash. */
    const char *root = t->root_folder ? t->root_folder : "";
    while (*root == '/') root++;
    snprintf(_root, sizeof(_root), "%s", root);
    for (size_t n = strlen(_root); n > 0 && _root[n - 1] == '/'; n--) _root[n - 1] = '\0';
    _stop = t->stop;
    _bandwidth = t->bandwidth_kib_s;
    if (t->api_host && t->api_host[0]) {
        /* A test server answers for both hosts. */
        snprintf(_api_host, sizeof(_api_host), "%s", t->api_host);
        snprintf(_content_host, sizeof(_content_host), "%s", t->api_host);
    }
    if (t->server_name && t->server_name[0]) snprintf(_server_name, sizeof(_server_name), "%s", t->server_name);
    if (t->api_port) _api_port = _content_port = t->api_port;

    char token[4352]; /* the web page accepts a token of up to 4096 characters */
    if (!config_value(t->credentials_path, t->credentials_section, "client_id", _key, sizeof(_key)) ||
        !config_value(t->credentials_path, t->credentials_section, "client_secret", _secret, sizeof(_secret)) ||
        !config_value(t->credentials_path, t->credentials_section, "token", token, sizeof(token)) ||
        !json_string(token, "refresh_token", _refresh_token, sizeof(_refresh_token)) || !_refresh_token[0]) {
        explicit_bzero(token, sizeof(token));
        set_error(error, error_size, "No Dropbox credentials saved");
        return false;
    }
    explicit_bzero(token, sizeof(token));
    return true;
}

bool APC_DropboxBackend::pause_ms(unsigned ms)
{
    for (unsigned waited = 0; waited < ms; waited += 50) {
        if (stopped()) return false;
        usleep(50000);
    }
    return !stopped();
}

bool APC_DropboxBackend::refresh_access_token(char *error, size_t error_size)
{
    if (!_api) {
        ca_https_options options = {};
        options.ca_file = _ca_file;
        options.server_name = _server_name[0] ? _server_name : NULL;
        options.stop = _stop;
        _api = ca_https_connect(_api_host, _api_port, &options, error, error_size);
        if (!_api) return false;
    }
    char form[1536];
    size_t used = 0;
    form[0] = '\0';
    bool built = true;
    auto add = [&](const char *key, const char *value) {
        /* Each separator needs room for itself and the terminator: a value that fills the buffer is refused. */
        if (!built || used + 2 >= sizeof(form)) { built = false; return; }
        if (used) form[used++] = '&';
        built = url_encode(form, sizeof(form), &used, key) && used + 2 < sizeof(form) && (form[used++] = '=', true) &&
                url_encode(form, sizeof(form), &used, value);
    };
    add("grant_type", "refresh_token");
    add("refresh_token", _refresh_token);
    add("client_id", _key);
    add("client_secret", _secret);
    if (!built || used + 1 >= sizeof(form)) {
        explicit_bzero(form, sizeof(form));
        set_error(error, error_size, "credentials too long");
        return false;
    }
    ca_https_req q = {};
    q.method = "POST";
    q.path = "/oauth2/token";
    q.headers = "Content-Type: application/x-www-form-urlencoded\r\n";
    q.body = form;
    q.length = strlen(form);
    q.fd = -1;
    ca_https_response r = {};
    r.body = _scratch;
    r.body_capacity = sizeof(_scratch);
    const int rc = ca_https_request(_api, &q, &r, error, error_size);
    explicit_bzero(form, sizeof(form));
    if (rc != CA_HTTPS_OK) {
        ca_https_close(_api);
        _api = NULL;
        return false;
    }
    double expires = 14400;
    /* A token that does not fit would be cut short, and Dropbox would then refuse it. */
    if (r.status == 200 && (r.truncated || (!json_string(_scratch, "access_token", _access_token, sizeof(_access_token)) &&
                                            strstr(_scratch, "\"access_token\"") != NULL))) {
        explicit_bzero(_access_token, sizeof(_access_token));
        set_error(error, error_size, "Dropbox sent an access token that is too long to use");
        return false;
    }
    if (r.status != 200 || !json_string(_scratch, "access_token", _access_token, sizeof(_access_token))) {
        char code[48] = "", description[96] = "";
        json_string(_scratch, "error", code, sizeof(code));
        json_string(_scratch, "error_description", description, sizeof(description));
        _credentials_rejected = r.status == 400 || r.status == 401;
        if (!strcmp(code, "invalid_grant")) {
            set_error(error, error_size, "Dropbox refused the credentials: the token has been revoked or is not valid. Authorise again");
        } else {
            set_error(error, error_size, "Dropbox %s the credentials (HTTP %d%s%s%s%s)",
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

/* One request, with a valid token. Returns 0 with the response (any status not worth
 * retrying: a 409 is for the caller to read), or a negative ca_https_error. A 401 gets a
 * new token; 429, 5xx and lost connections are retried after a pause. */
int APC_DropboxBackend::call(Host host, const char *path, const char *headers, const void *body, size_t length, int fd,
                             off_t offset, void (*progress)(void *, uint64_t), void *progress_user,
                             ca_https_response &response, char *error, size_t error_size)
{
    int last = CA_HTTPS_ERR_IO;
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        if (stopped()) return CA_HTTPS_ERR_STOPPED;
        if (attempt && !pause_ms(1000U << (attempt - 1))) return CA_HTTPS_ERR_STOPPED;
        if (!_access_token[0] || monotonic_ms() >= _token_expires_ms) {
            if (!refresh_access_token(error, error_size)) {
                if (stopped()) return CA_HTTPS_ERR_STOPPED;
                last = CA_HTTPS_ERR_TLS;
                if (_credentials_rejected) return last;
                continue;
            }
        }
        ca_https *&c = connection(host);
        if (!c) {
            ca_https_options options = {};
            options.ca_file = _ca_file;
            options.server_name = _server_name[0] ? _server_name : NULL;
            options.bandwidth_kib_s = host == CONTENT ? _bandwidth : 0;
            options.stop = _stop;
            options.timeout_ms = 60000;
            c = ca_https_connect(host == API ? _api_host : _content_host, host == API ? _api_port : _content_port,
                                 &options, error, error_size);
            if (!c) {
                if (stopped()) return CA_HTTPS_ERR_STOPPED;
                last = CA_HTTPS_ERR_CONNECT;
                continue;
            }
        }
        /* Never cut short: a request that is too long for the HTTPS client is refused there. */
        std::string head = std::string("Authorization: Bearer ") + _access_token + "\r\n" + (headers ? headers : "");
        ca_https_req q = {};
        q.method = "POST";
        q.path = path;
        q.headers = head.c_str();
        q.body = body;
        q.length = length;
        q.fd = fd;
        q.offset = offset;
        q.progress = progress;
        q.progress_user = progress_user;
        response = ca_https_response {};
        response.body = _body;
        response.body_capacity = sizeof(_body);
        const int rc = ca_https_request(c, &q, &response, error, error_size);
        explicit_bzero(&head[0], head.size()); /* it holds the access token */
        if (rc == CA_HTTPS_ERR_STOPPED) return rc;
        if (rc == CA_HTTPS_ERR_FILE) return rc;
        if (rc != CA_HTTPS_OK) {
            ca_https_close(c);
            c = NULL;
            last = rc;
            continue;
        }
        /* A token made before a permission was ticked has no such scope, and renewing it will not
         * help: say which permission, and how to get a token that has it. */
        if ((response.status == 400 || response.status == 401 || response.status == 403) &&
            (strstr(_body, "required scope") || strstr(_body, "missing_scope"))) {
            char scope[64] = "";
            const char *quoted = strstr(_body, "required scope '");
            if (quoted) {
                quoted += strlen("required scope '");
                snprintf(scope, sizeof(scope), "%.*s", (int)strcspn(quoted, "'"), quoted);
            } else {
                json_string(_body, "required_scope", scope, sizeof(scope));
            }
            set_error(error, error_size, "The Dropbox app does not have the %s permission. Tick it on the app's Permissions tab, "
                      "click Submit, then authorise again to get a new token", scope[0] ? scope : "required");
            return CA_HTTPS_ERR_TLS;
        }
        if (response.status == 401) {
            _access_token[0] = '\0';
            set_error(error, error_size, "Dropbox rejected the access token");
            last = CA_HTTPS_ERR_TLS;
            continue;
        }
        if (response.status == 429 || response.status >= 500) {
            set_error(error, error_size, "Dropbox returned HTTP %d", response.status);
            last = CA_HTTPS_ERR_IO;
            const unsigned wait = (unsigned)strtoul(response.retry_after, NULL, 10);
            if (response.status == 429 && wait > 0 && wait <= 60 && attempt + 1 < ATTEMPTS && !pause_ms(wait * 1000U))
                return CA_HTTPS_ERR_STOPPED;
            continue;
        }
        return 0;
    }
    return last;
}

/* The file's metadata, or a 409 reply with path/not_found if it is not there. */
int APC_DropboxBackend::metadata(const std::string &path, ca_https_response &response, char *error, size_t error_size)
{
    std::string body = "{\"path\":\"";
    if (!json_ascii(body, path.c_str())) {
        set_error(error, error_size, "the file name is not valid");
        return CA_HTTPS_ERR_ARGS;
    }
    body += "\"}";
    return call(API, "/2/files/get_metadata", "Content-Type: application/json\r\n", body.data(), body.size(), -1, 0, NULL,
                NULL, response, error, error_size);
}

struct Progress { void (*fn)(void *, uint64_t); void *user; uint64_t base; };
static void report(void *context, uint64_t sent)
{
    Progress *p = (Progress *)context;
    if (p->fn) p->fn(p->user, p->base + sent);
}

/* A file that fits in one request. */
ca_put_result APC_DropboxBackend::upload(const std::string &path, int fd, uint64_t size, void (*progress)(void *, uint64_t),
                                         void *user, ca_https_response &final, char *error, size_t error_size)
{
    std::string arg = "Content-Type: application/octet-stream\r\nDropbox-API-Arg: {\"path\":\"";
    if (!json_ascii(arg, path.c_str())) {
        set_error(error, error_size, "the file name is not valid");
        return CA_PUT_REJECTED;
    }
    arg += "\",\"mode\":\"overwrite\",\"autorename\":false,\"mute\":true}\r\n";
    Progress p = {progress, user, 0};
    const int rc = call(CONTENT, "/2/files/upload", arg.c_str(), NULL, (size_t)size, fd, 0, progress ? report : NULL, &p, final,
                        error, error_size);
    if (rc == CA_HTTPS_ERR_STOPPED) return CA_PUT_STOPPED;
    if (rc == CA_HTTPS_ERR_ARGS) return CA_PUT_REJECTED; /* this file's request cannot be made: not worth retrying */
    if (rc != 0) return CA_PUT_FAILED;
    if (final.status != 200) {
        set_error(error, error_size, "Dropbox refused the file (HTTP %d)", final.status);
        return CA_PUT_FAILED;
    }
    return CA_PUT_UPLOADED;
}

/* A larger file: start a session, append 4 MiB pieces, then finish. After an error the
 * server says where it is (a 409 incorrect_offset), and the next piece starts there. */
ca_put_result APC_DropboxBackend::upload_session(const std::string &path, int fd, uint64_t size,
                                                 void (*progress)(void *, uint64_t), void *user, ca_https_response &final,
                                                 char *error, size_t error_size)
{
    std::string commit = "\"commit\":{\"path\":\"";
    if (!json_ascii(commit, path.c_str())) {
        set_error(error, error_size, "the file name is not valid");
        return CA_PUT_REJECTED;
    }
    commit += "\",\"mode\":\"overwrite\",\"autorename\":false,\"mute\":true}";

    for (int session_try = 0; session_try < 2; session_try++) {
        ca_https_response r;
        int rc = call(CONTENT, "/2/files/upload_session/start",
                      "Content-Type: application/octet-stream\r\nDropbox-API-Arg: {\"close\":false}\r\n", NULL, 0, -1, 0, NULL,
                      NULL, r, error, error_size);
        if (rc == CA_HTTPS_ERR_STOPPED) return CA_PUT_STOPPED;
        char session[160] = "";
        if (rc != 0 || r.status != 200 || !json_string(_body, "session_id", session, sizeof(session))) {
            if (rc == 0) set_error(error, error_size, "Dropbox would not start the upload (HTTP %d)", r.status);
            return CA_PUT_FAILED;
        }
        uint64_t offset = 0;
        int failures = 0;
        bool restart = false;
        while (!restart) {
            const uint64_t left = size - offset;
            const bool last = left <= CHUNK_BYTES;
            const uint64_t count = last ? left : CHUNK_BYTES;
            char cursor[320];
            snprintf(cursor, sizeof(cursor), "\"cursor\":{\"session_id\":\"%s\",\"offset\":%llu}", session, (unsigned long long)offset);
            std::string arg = "Content-Type: application/octet-stream\r\nDropbox-API-Arg: {";
            arg += cursor;
            if (last) { arg += ","; arg += commit; } else arg += ",\"close\":false";
            arg += "}\r\n";
            Progress p = {progress, user, offset};
            rc = call(CONTENT, last ? "/2/files/upload_session/finish" : "/2/files/upload_session/append_v2", arg.c_str(), NULL,
                      (size_t)count, fd, (off_t)offset, progress ? report : NULL, &p, r, error, error_size);
            if (rc == CA_HTTPS_ERR_STOPPED) return CA_PUT_STOPPED;
            if (rc == CA_HTTPS_ERR_FILE) return CA_PUT_FAILED;
            if (rc == CA_HTTPS_ERR_ARGS) return CA_PUT_REJECTED; /* this file's request cannot be made: not worth retrying */
            if (rc == 0 && r.status == 200) {
                if (last) {
                    final = r;
                    return CA_PUT_UPLOADED;
                }
                offset += count;
                failures = 0;
                continue;
            }
            if (rc == 0 && r.status == 409) {
                double correct = 0;
                if (strstr(_body, "incorrect_offset") && json_number(_body, "correct_offset", &correct) &&
                    correct >= 0 && (uint64_t)correct <= size && (uint64_t)correct != offset) {
                    /* Dropbox has more (or less) than we thought: carry on from there. If it already has
                     * everything, the finish call goes out empty. */
                    offset = (uint64_t)correct;
                    if (++failures >= ATTEMPTS) return CA_PUT_FAILED;
                    continue;
                }
                if (strstr(_body, "not_found") || strstr(_body, "closed")) {
                    restart = true; /* the session is gone: start over once */
                    set_error(error, error_size, "upload session expired");
                    break;
                }
                set_error(error, error_size, "Dropbox refused the data (%.80s)", _body);
                return CA_PUT_FAILED;
            }
            if (rc == 0 && r.status >= 400 && r.status < 500) {
                set_error(error, error_size, "Dropbox refused the data (HTTP %d)", r.status);
                return CA_PUT_FAILED;
            }
            if (++failures >= ATTEMPTS) return CA_PUT_FAILED;
        }
    }
    return CA_PUT_FAILED;
}

ca_put_result APC_DropboxBackend::put(const char *remote_dir, const char *name, const char *local_path,
                                      void (*progress)(void *, uint64_t), void *user, char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    if (stopped()) return CA_PUT_STOPPED;
    std::string path;
    const char *parts[3] = {_root, remote_dir ? remote_dir : "", name};
    for (const char *part : parts) {
        if (!*part) continue;
        path += '/';
        path += part;
    }
    if (path.size() > MAX_PATH_BYTES) {
        set_error(error, error_size, "path too long: %.40s", name);
        return CA_PUT_REJECTED;
    }

    /* The path goes into a header as \uXXXX for each non-ASCII character: it must still fit. Before any network work. */
    std::string escaped;
    if (!json_ascii(escaped, path.c_str()) || escaped.size() > MAX_ESCAPED_PATH_BYTES) {
        set_error(error, error_size, "path too long or not valid: %.40s", name);
        return CA_PUT_REJECTED;
    }

    const int fd = open(local_path, O_RDONLY | O_CLOEXEC);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        if (fd >= 0) close(fd);
        set_error(error, error_size, "cannot read %.60s", name);
        return CA_PUT_REJECTED;
    }
    /* Is it already there? A 409 with path/not_found means no. */
    ca_https_response r;
    int rc = metadata(path, r, error, error_size);
    if (rc != 0) {
        close(fd);
        return rc == CA_HTTPS_ERR_STOPPED ? CA_PUT_STOPPED : rc == CA_HTTPS_ERR_ARGS ? CA_PUT_REJECTED : CA_PUT_FAILED;
    }
    /* Only now is the file read through: with no connection a large recording is not read over and over. */
    char local_hash[65];
    uint64_t size = 0;
    bool hash_stopped = false;
    if (!content_hash(fd, local_hash, &size, _stop, &hash_stopped)) {
        close(fd);
        if (hash_stopped) return CA_PUT_STOPPED;
        set_error(error, error_size, "cannot read %.60s", name);
        return CA_PUT_REJECTED;
    }
    if (r.status == 200) {
        char remote_hash[80] = "";
        json_string(_body, "content_hash", remote_hash, sizeof(remote_hash));
        double remote_bytes = -1;
        json_number(_body, "size", &remote_bytes);
        if (!strcasecmp(remote_hash, local_hash) && remote_bytes == (double)size) {
            close(fd);
            return CA_PUT_SKIPPED;
        }
        if (size == 0 && remote_bytes > 0) {
            /* An empty local file (an interrupted capture, say) must not replace what Dropbox has, and it
             * is not worth failing for: leave both as they are. */
            close(fd);
            return CA_PUT_LEFT;
        }
    } else if (r.status == 409 && strstr(_body, "malformed_path")) {
        close(fd);
        set_error(error, error_size, "Dropbox cannot take the name %.60s", name);
        return CA_PUT_REJECTED; /* this file only: the others can still be sent */
    } else if (!(r.status == 409 && strstr(_body, "not_found"))) {
        close(fd);
        set_error(error, error_size, "Dropbox could not look up %.60s (HTTP %d)", name, r.status);
        return CA_PUT_FAILED;
    }

    ca_https_response final = {};
    const ca_put_result result = size <= CHUNK_BYTES
        ? upload(path, fd, size, progress, user, final, error, error_size)
        : upload_session(path, fd, size, progress, user, final, error, error_size);
    close(fd);
    if (result != CA_PUT_UPLOADED) {
        if (result == CA_PUT_FAILED && !stopped()) {
            /* A lost reply may hide a finished upload: look before reporting a failure. */
            ca_https_response check;
            if (metadata(path, check, error, 0) == 0 && check.status == 200) {
                char remote_hash[80] = "";
                double remote_bytes = -1;
                json_string(_body, "content_hash", remote_hash, sizeof(remote_hash));
                json_number(_body, "size", &remote_bytes);
                if (!strcasecmp(remote_hash, local_hash) && remote_bytes == (double)size) return CA_PUT_UPLOADED;
            }
        }
        return result;
    }

    /* The reply carries what Dropbox stored. Delete nothing locally until it matches. */
    char got_hash[80] = "";
    double got_size = -1;
    json_string(_body, "content_hash", got_hash, sizeof(got_hash));
    json_number(_body, "size", &got_size);
    if (strcasecmp(got_hash, local_hash) != 0 || got_size != (double)size) {
        set_error(error, error_size, "%.60s did not match after upload (the file may have changed)", name);
        return CA_PUT_FAILED;
    }
    return CA_PUT_UPLOADED;
}

} // namespace

APC_UploadBackend *ca_dropbox_backend_create(const ca_upload_target *target, char *error, size_t error_size)
{
    APC_DropboxBackend *backend = new (std::nothrow) APC_DropboxBackend();
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
