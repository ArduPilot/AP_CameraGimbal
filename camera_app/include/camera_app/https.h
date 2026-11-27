#ifndef CAMERA_APP_HTTPS_H
#define CAMERA_APP_HTTPS_H
#include <atomic>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* A small blocking HTTPS/1.1 client over Mbed TLS, for the cloud uploader's
 * worker thread. The server certificate is always verified against a CA file
 * and the host name. Every wait polls in short slices, so setting the stop
 * flag ends a call within about 200 ms (DNS lookups excepted). One connection
 * is kept alive and reopened if the server closes it. */

struct ca_https;

struct ca_https_options {
    const char *ca_file;                 /* PEM bundle; required */
    const char *server_name;             /* name checked against the certificate; default: the host */
    unsigned timeout_ms;                 /* longest wait with no progress; default 30000 */
    unsigned bandwidth_kib_s;            /* limit on sent data, KiB/s; 0 = unlimited */
    const std::atomic<bool> *stop;       /* optional: set to abandon the call */
};

enum ca_https_error {
    CA_HTTPS_OK = 0,
    CA_HTTPS_ERR_ARGS = -1,
    CA_HTTPS_ERR_RESOLVE = -2,    /* DNS lookup failed */
    CA_HTTPS_ERR_CONNECT = -3,    /* TCP connection failed */
    CA_HTTPS_ERR_TLS = -4,        /* handshake or certificate failure */
    CA_HTTPS_ERR_IO = -5,         /* connection lost or timed out */
    CA_HTTPS_ERR_PROTOCOL = -6,   /* malformed HTTP response */
    CA_HTTPS_ERR_STOPPED = -7,    /* the stop flag was set */
    CA_HTTPS_ERR_FILE = -8        /* reading the request body failed */
};

struct ca_https_req {
    const char *method;          /* "GET", "POST", "PUT", ... */
    const char *path;            /* "/drive/v3/files?...", already percent-encoded */
    const char *headers;         /* extra header lines, each ending in "\r\n"; not Host, Content-Length or Connection */
    /* Body: from memory, or `length` bytes of file `fd` from `offset`. */
    const void *body;
    int fd;                      /* -1 if none */
    off_t offset;
    uint64_t length;             /* body size; 0 = no body */
    /* Called as body bytes are sent (optional), to report progress. */
    void (*progress)(void *user, uint64_t sent);
    void *progress_user;
};

struct ca_https_response {
    int status;                  /* HTTP status code */
    char location[512];          /* Location header, or "" */
    char range[64];              /* Range header, or "" (a 308 resume reply) */
    char retry_after[16];        /* Retry-After header, or "" */
    char *body;                  /* caller's buffer, always NUL-terminated if body_capacity > 0 */
    size_t body_capacity;
    size_t body_length;
    bool truncated;              /* the body was longer than the buffer; the rest was read and dropped */
};

/* Connects (TCP and TLS). Returns NULL on failure with a message in `error`. */
struct ca_https *ca_https_connect(const char *host, unsigned port, const struct ca_https_options *options,
                                  char *error, size_t error_size);
void ca_https_close(struct ca_https *connection);
/* Sends one request and reads the whole response. Returns CA_HTTPS_OK or a
 * negative ca_https_error with a message in `error`. A non-2xx status is not
 * an error here. If the server had closed the idle connection, it reconnects
 * and sends the request once more, but only a request without a body, or one
 * that failed while its headers were written: after that the server may have
 * acted on the body, so the caller decides (a resumable upload asks the server
 * how much it has). */
int ca_https_request(struct ca_https *connection, const struct ca_https_req *request,
                     struct ca_https_response *response, char *error, size_t error_size);
/* Certificates have validity dates, so TLS cannot work before the clock is set. */
bool ca_https_clock_plausible(void);
/* Changes the limit on sent data for later requests. */
void ca_https_set_bandwidth(struct ca_https *connection, unsigned kib_s);

#endif
