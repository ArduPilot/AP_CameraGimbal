/* A small blocking HTTPS/1.1 client over Mbed TLS: see camera_app/https.h.
 * The socket is non-blocking and every wait is a poll() in short slices, so a
 * stop flag is noticed quickly even in the middle of a transfer. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/https.h"

#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WAIT_SLICE_MS 200
#define DEFAULT_TIMEOUT_MS 30000U
#define HEAD_MAX 8192U
#define SEND_CHUNK 4096U
/* Not before this (1 January 2025): the camera's clock has been set. */
#define CLOCK_PLAUSIBLE_AFTER 1735689600

struct ca_https {
    char host[256];
    char server_name[256];
    char ca_file[256];
    unsigned port;
    unsigned timeout_ms;
    unsigned bandwidth_kib_s;
    const std::atomic<bool> *stop;

    int fd;
    bool ssl_ready;
    bool reusable;     /* the connection can carry another request */
    bool used;         /* it has carried a request, so the server may have closed it */
    int wait_error;    /* set by wait_fd(): a ca_https_error, or 0 */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;

    uint64_t bandwidth_start_us, bandwidth_sent;
    unsigned char rbuf[4096];
    size_t rpos, rlen;
};

static void set_error(char *error, size_t size, const char *format, ...)
{
    if (!error || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

static uint64_t monotonic_us()
{
    timespec t {};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000U + (uint64_t)t.tv_nsec / 1000U;
}

bool ca_https_clock_plausible(void)
{
    return time(NULL) >= CLOCK_PLAUSIBLE_AFTER;
}

static bool stopped(const ca_https *c)
{
    return c->stop && c->stop->load();
}

/* Waits for the socket; 0 when ready, else records and returns a ca_https_error. */
static int wait_fd(ca_https *c, short events)
{
    const uint64_t start = monotonic_us();
    for (;;) {
        if (stopped(c)) return c->wait_error = CA_HTTPS_ERR_STOPPED;
        const uint64_t waited_ms = (monotonic_us() - start) / 1000U;
        if (waited_ms >= c->timeout_ms) return c->wait_error = CA_HTTPS_ERR_IO;
        const uint64_t left = c->timeout_ms - waited_ms;
        pollfd p = {c->fd, events, 0};
        const int r = poll(&p, 1, left < WAIT_SLICE_MS ? (int)left : WAIT_SLICE_MS);
        if (r > 0) return 0; /* ready, or an error that the next read or write reports */
        if (r < 0 && errno != EINTR) return c->wait_error = CA_HTTPS_ERR_IO;
    }
}

static int bio_send(void *context, const unsigned char *buffer, size_t length)
{
    const int fd = ((ca_https *)context)->fd;
    const ssize_t n = send(fd, buffer, length, MSG_NOSIGNAL);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int bio_recv(void *context, unsigned char *buffer, size_t length)
{
    const int fd = ((ca_https *)context)->fd;
    const ssize_t n = recv(fd, buffer, length, 0);
    if (n > 0) return (int)n;
    if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

/* Calls an Mbed TLS I/O function until it is not waiting for the socket.
 * On return, c->wait_error is non-zero if a wait failed (stop or timeout). */
template <class F> static int tls_call(ca_https *c, F function)
{
    c->wait_error = 0;
    for (;;) {
        /* A socket that is always ready never reaches wait_fd(), so stop is also checked here. */
        if (stopped(c)) { c->wait_error = CA_HTTPS_ERR_STOPPED; return MBEDTLS_ERR_SSL_WANT_WRITE; }
        const int r = function();
        if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            if (wait_fd(c, POLLIN)) return r;
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (wait_fd(c, POLLOUT)) return r;
        } else {
            return r;
        }
    }
}

static void tls_message(ca_https *c, const char *what, int r, char *error, size_t size)
{
    char text[100];
    mbedtls_strerror(r, text, sizeof(text));
    const uint32_t flags = c->ssl_ready ? mbedtls_ssl_get_verify_result(&c->ssl) : 0;
    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags && flags != 0xFFFFFFFFU) {
        char info[160];
        mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
        for (char *end = info + strlen(info); end > info && (end[-1] == '\n' || end[-1] == ' '); *--end = '\0') {}
        set_error(error, size, "%s: certificate not trusted: %s", what, info);
    } else {
        set_error(error, size, "%s: %s", what, text);
    }
}

static void disconnect(ca_https *c)
{
    if (c->ssl_ready) {
        /* Best effort and never blocking: the socket is non-blocking. */
        (void)mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_init(&c->ssl);
        c->ssl_ready = false;
    }
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->rpos = c->rlen = 0;
    c->reusable = false;
}

static int open_socket(ca_https *c, char *error, size_t error_size)
{
    char service[8];
    snprintf(service, sizeof(service), "%u", c->port);
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *list = NULL;
    const int lookup = getaddrinfo(c->host, service, &hints, &list);
    if (lookup != 0) {
        set_error(error, error_size, "cannot resolve %s: %s", c->host, gai_strerror(lookup));
        return CA_HTTPS_ERR_RESOLVE;
    }
    int result = CA_HTTPS_ERR_CONNECT;
    set_error(error, error_size, "cannot connect to %s", c->host);
    for (const addrinfo *a = list; a; a = a->ai_next) {
        const int fd = socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) continue;
        c->fd = fd;
        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            const int waited = wait_fd(c, POLLOUT);
            if (waited) {
                close(fd);
                c->fd = -1;
                result = waited;
                set_error(error, error_size, waited == CA_HTTPS_ERR_STOPPED ? "stopped" : "connecting to %s timed out", c->host);
                if (waited == CA_HTTPS_ERR_IO) result = CA_HTTPS_ERR_CONNECT;
                if (waited == CA_HTTPS_ERR_STOPPED) break;
                continue;
            }
            int so_error = 0;
            socklen_t length = sizeof(so_error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length);
            r = so_error ? -1 : 0;
            errno = so_error;
        }
        if (r == 0) {
            const int on = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
            freeaddrinfo(list);
            if (error && error_size) error[0] = '\0'; /* connected: no leftover "cannot connect" */
            return CA_HTTPS_OK;
        }
        set_error(error, error_size, "cannot connect to %s: %s", c->host, strerror(errno));
        close(fd);
        c->fd = -1;
    }
    freeaddrinfo(list);
    return result;
}

static int establish(ca_https *c, char *error, size_t error_size)
{
    int r = open_socket(c, error, error_size);
    if (r != CA_HTTPS_OK) return r;
    mbedtls_ssl_init(&c->ssl);
    if ((r = mbedtls_ssl_setup(&c->ssl, &c->conf)) != 0 ||
        (r = mbedtls_ssl_set_hostname(&c->ssl, c->server_name)) != 0) {
        tls_message(c, "TLS setup", r, error, error_size);
        disconnect(c);
        return CA_HTTPS_ERR_TLS;
    }
    c->ssl_ready = true;
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    r = tls_call(c, [&] { return mbedtls_ssl_handshake(&c->ssl); });
    if (r != 0) {
        const int wait_error = c->wait_error;
        if (wait_error) set_error(error, error_size, wait_error == CA_HTTPS_ERR_STOPPED ? "stopped" : "TLS handshake timed out");
        else tls_message(c, "TLS handshake", r, error, error_size);
        disconnect(c);
        return wait_error ? wait_error : CA_HTTPS_ERR_TLS;
    }
    c->reusable = true;
    c->used = false;
    c->bandwidth_start_us = 0;
    return CA_HTTPS_OK;
}

struct ca_https *ca_https_connect(const char *host, unsigned port, const struct ca_https_options *options,
                                  char *error, size_t error_size)
{
    if (!host || !host[0] || !port || port > 65535 || !options || !options->ca_file || !options->ca_file[0] ||
        strlen(host) >= sizeof(((ca_https *)0)->host) || strlen(options->ca_file) >= sizeof(((ca_https *)0)->ca_file) ||
        (options->server_name && strlen(options->server_name) >= sizeof(((ca_https *)0)->server_name))) {
        set_error(error, error_size, "bad connection settings");
        return NULL;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        set_error(error, error_size, "cannot start the cryptography library");
        return NULL;
    }
    ca_https *c = (ca_https *)calloc(1, sizeof(*c));
    if (!c) {
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    snprintf(c->host, sizeof(c->host), "%s", host);
    snprintf(c->server_name, sizeof(c->server_name), "%s", options->server_name && options->server_name[0] ? options->server_name : host);
    snprintf(c->ca_file, sizeof(c->ca_file), "%s", options->ca_file);
    c->port = port;
    c->timeout_ms = options->timeout_ms ? options->timeout_ms : DEFAULT_TIMEOUT_MS;
    c->bandwidth_kib_s = options->bandwidth_kib_s;
    c->stop = options->stop;
    c->fd = -1;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);

    int r = mbedtls_x509_crt_parse_file(&c->ca, c->ca_file);
    if (r < 0 || c->ca.raw.p == NULL) {
        set_error(error, error_size, "cannot read the CA file %s", c->ca_file);
        ca_https_close(c);
        return NULL;
    }
    if ((r = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        tls_message(c, "TLS setup", r, error, error_size);
        ca_https_close(c);
        return NULL;
    }
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    if (establish(c, error, error_size) != CA_HTTPS_OK) {
        ca_https_close(c);
        return NULL;
    }
    return c;
}

void ca_https_close(struct ca_https *c)
{
    if (!c) return;
    disconnect(c);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->ca);
    free(c);
}

void ca_https_set_bandwidth(struct ca_https *c, unsigned kib_s)
{
    if (c) c->bandwidth_kib_s = kib_s;
}

/* ---- sending ---- */

static int write_all(ca_https *c, const unsigned char *data, size_t length, char *error, size_t error_size)
{
    size_t done = 0;
    while (done < length) {
        const int r = tls_call(c, [&] { return mbedtls_ssl_write(&c->ssl, data + done, length - done); });
        if (r > 0) {
            done += (size_t)r;
            continue;
        }
        if (c->wait_error) {
            set_error(error, error_size, c->wait_error == CA_HTTPS_ERR_STOPPED ? "stopped" : "sending timed out");
            return c->wait_error;
        }
        tls_message(c, "send failed", r, error, error_size);
        return CA_HTTPS_ERR_IO;
    }
    return CA_HTTPS_OK;
}

/* Sleeps so the average rate of sent body data stays within the limit. */
static int throttle(ca_https *c, size_t sent_now)
{
    if (!c->bandwidth_kib_s) return CA_HTTPS_OK;
    if (!c->bandwidth_start_us) c->bandwidth_start_us = monotonic_us();
    c->bandwidth_sent += sent_now;
    const uint64_t due_us = c->bandwidth_sent * 1000000U / ((uint64_t)c->bandwidth_kib_s * 1024U);
    for (;;) {
        if (stopped(c)) return CA_HTTPS_ERR_STOPPED;
        const uint64_t elapsed_us = monotonic_us() - c->bandwidth_start_us;
        if (elapsed_us >= due_us) return CA_HTTPS_OK;
        const uint64_t wait_us = due_us - elapsed_us;
        usleep(wait_us < 50000U ? (useconds_t)wait_us : 50000U);
    }
}

static int send_request(ca_https *c, const ca_https_req *q, bool *head_sent, char *error, size_t error_size)
{
    char head[4096];
    const bool has_body = q->length > 0;
    const bool wants_length = has_body || !strcmp(q->method, "POST") || !strcmp(q->method, "PUT") ||
                              !strcmp(q->method, "PATCH");
    char length_line[48] = "";
    if (wants_length) snprintf(length_line, sizeof(length_line), "Content-Length: %llu\r\n", (unsigned long long)q->length);
    const int n = snprintf(head, sizeof(head),
                           "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ap-cameragimbal\r\nAccept: */*\r\n%s%s\r\n",
                           q->method, q->path, c->server_name, length_line, q->headers ? q->headers : "");
    if (n < 0 || (size_t)n >= sizeof(head)) {
        set_error(error, error_size, "request header too long");
        return CA_HTTPS_ERR_ARGS;
    }
    int r = write_all(c, (const unsigned char *)head, (size_t)n, error, error_size);
    if (r != CA_HTTPS_OK) return r;
    *head_sent = true;

    c->bandwidth_start_us = 0;
    c->bandwidth_sent = 0;
    uint64_t sent = 0;
    unsigned char chunk[SEND_CHUNK];
    while (sent < q->length) {
        size_t count = q->length - sent < sizeof(chunk) ? (size_t)(q->length - sent) : sizeof(chunk);
        const unsigned char *data;
        if (q->body) {
            data = (const unsigned char *)q->body + sent;
        } else {
            const ssize_t got = q->fd >= 0 ? pread(q->fd, chunk, count, q->offset + (off_t)sent) : -1;
            if (got <= 0) {
                set_error(error, error_size, "cannot read the file to upload");
                return CA_HTTPS_ERR_FILE;
            }
            count = (size_t)got;
            data = chunk;
        }
        r = write_all(c, data, count, error, error_size);
        if (r != CA_HTTPS_OK) return r;
        sent += count;
        if (q->progress) q->progress(q->progress_user, sent);
        if (throttle(c, count) == CA_HTTPS_ERR_STOPPED) {
            set_error(error, error_size, "stopped");
            return CA_HTTPS_ERR_STOPPED;
        }
    }
    return CA_HTTPS_OK;
}

/* ---- receiving ---- */

/* Makes more bytes available in rbuf. Returns 1, 0 at end of stream, or a negative error. */
static int fill(ca_https *c, char *error, size_t error_size)
{
    for (;;) {
        const int r = tls_call(c, [&] { return mbedtls_ssl_read(&c->ssl, c->rbuf, sizeof(c->rbuf)); });
        if (r > 0) {
            c->rpos = 0;
            c->rlen = (size_t)r;
            return 1;
        }
        if (c->wait_error) {
            set_error(error, error_size, c->wait_error == CA_HTTPS_ERR_STOPPED ? "stopped" : "no response from the server");
            return c->wait_error;
        }
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
        tls_message(c, "receive failed", r, error, error_size);
        return CA_HTTPS_ERR_IO;
    }
}

static int next_byte(ca_https *c, char *error, size_t error_size)
{
    if (c->rpos >= c->rlen) {
        const int r = fill(c, error, error_size);
        if (r <= 0) return r == 0 ? -1000 : r; /* -1000: end of stream */
    }
    return c->rbuf[c->rpos++];
}

#define EOS (-1000)

/* Reads up to the blank line that ends a header block. */
static int read_head(ca_https *c, char *head, size_t size, size_t *got_bytes, char *error, size_t error_size)
{
    size_t length = 0;
    while (length + 1 < size) {
        const int b = next_byte(c, error, error_size);
        if (b < 0) return b == EOS ? CA_HTTPS_ERR_IO : b;
        head[length++] = (char)b;
        *got_bytes += 1;
        if (length >= 4 && !memcmp(head + length - 4, "\r\n\r\n", 4)) {
            head[length] = '\0';
            return CA_HTTPS_OK;
        }
    }
    set_error(error, error_size, "response header too long");
    return CA_HTTPS_ERR_PROTOCOL;
}

static void copy_value(char *to, size_t size, const char *from, size_t length)
{
    if (length >= size) length = size - 1;
    memcpy(to, from, length);
    to[length] = '\0';
}

static void store_body(ca_https_response *resp, const unsigned char *data, size_t length)
{
    if (!resp->body || !resp->body_capacity) return;
    const size_t room = resp->body_capacity - 1 - resp->body_length;
    const size_t take = length < room ? length : room;
    memcpy(resp->body + resp->body_length, data, take);
    resp->body_length += take;
    resp->body[resp->body_length] = '\0';
    if (take < length) resp->truncated = true;
}

static int read_body_bytes(ca_https *c, ca_https_response *resp, uint64_t count, char *error, size_t error_size)
{
    while (count > 0) {
        if (c->rpos >= c->rlen) {
            const int r = fill(c, error, error_size);
            if (r == 0) {
                set_error(error, error_size, "connection closed during the response");
                return CA_HTTPS_ERR_IO;
            }
            if (r < 0) return r;
        }
        const size_t take = c->rlen - c->rpos < count ? c->rlen - c->rpos : (size_t)count;
        store_body(resp, c->rbuf + c->rpos, take);
        c->rpos += take;
        count -= take;
    }
    return CA_HTTPS_OK;
}

static int read_line(ca_https *c, char *line, size_t size, char *error, size_t error_size)
{
    size_t length = 0;
    for (;;) {
        const int b = next_byte(c, error, error_size);
        if (b < 0) {
            if (b == EOS) set_error(error, error_size, "connection closed during the response");
            return b == EOS ? CA_HTTPS_ERR_IO : b;
        }
        if (b == '\n') break;
        if (length + 1 < size) line[length++] = (char)b;
    }
    while (length && line[length - 1] == '\r') length--;
    line[length] = '\0';
    return CA_HTTPS_OK;
}

static int read_chunked(ca_https *c, ca_https_response *resp, char *error, size_t error_size)
{
    for (;;) {
        char line[64];
        int r = read_line(c, line, sizeof(line), error, error_size);
        if (r != CA_HTTPS_OK) return r;
        char *end;
        const unsigned long long size = strtoull(line, &end, 16);
        if (end == line) {
            set_error(error, error_size, "bad chunk size in the response");
            return CA_HTTPS_ERR_PROTOCOL;
        }
        if (size == 0) {
            do { /* trailers, up to the blank line */
                r = read_line(c, line, sizeof(line), error, error_size);
                if (r != CA_HTTPS_OK) return r;
            } while (line[0]);
            return CA_HTTPS_OK;
        }
        r = read_body_bytes(c, resp, size, error, error_size);
        if (r != CA_HTTPS_OK) return r;
        r = read_line(c, line, sizeof(line), error, error_size); /* the CRLF after the data */
        if (r != CA_HTTPS_OK) return r;
    }
}

/* Reads one response. `got_bytes` counts what arrived, so the caller can tell a
 * stale kept-alive connection (nothing arrived) from a failure part way. */
static int read_response(ca_https *c, const ca_https_req *q, ca_https_response *resp, size_t *got_bytes,
                         char *error, size_t error_size)
{
    char head[HEAD_MAX];
    for (;;) {
        int r = read_head(c, head, sizeof(head), got_bytes, error, error_size);
        if (r != CA_HTTPS_OK) {
            if (r == CA_HTTPS_ERR_IO && error && !error[0])
                set_error(error, error_size, *got_bytes == 0 ? "connection closed by the server" : "connection closed during the response");
            return r;
        }
        int minor = 1, status = 0;
        if (sscanf(head, "HTTP/1.%d %d", &minor, &status) != 2 || status < 100 || status > 599) {
            set_error(error, error_size, "malformed HTTP response");
            return CA_HTTPS_ERR_PROTOCOL;
        }
        if (status >= 100 && status < 200 && status != 101) continue; /* 100 Continue */
        resp->status = status;
        resp->location[0] = resp->range[0] = resp->retry_after[0] = '\0';
        resp->body_length = 0;
        resp->truncated = false;
        if (resp->body && resp->body_capacity) resp->body[0] = '\0';

        bool chunked = false, close_after = (minor == 0), have_length = false;
        unsigned long long length = 0;
        for (const char *line = strstr(head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
            line += 2;
            const char *colon = strchr(line, ':');
            const char *eol = strstr(line, "\r\n");
            if (!colon || !eol || colon > eol) continue;
            const char *value = colon + 1;
            while (value < eol && (*value == ' ' || *value == '\t')) value++;
            const size_t name_length = (size_t)(colon - line), value_length = (size_t)(eol - value);
            if (name_length == 14 && !strncasecmp(line, "Content-Length", 14)) {
                length = strtoull(value, NULL, 10);
                have_length = true;
            } else if (name_length == 17 && !strncasecmp(line, "Transfer-Encoding", 17)) {
                chunked = value_length >= 7 && !strncasecmp(value, "chunked", 7);
            } else if (name_length == 10 && !strncasecmp(line, "Connection", 10)) {
                if (value_length >= 5 && !strncasecmp(value, "close", 5)) close_after = true;
            } else if (name_length == 8 && !strncasecmp(line, "Location", 8)) {
                copy_value(resp->location, sizeof(resp->location), value, value_length);
            } else if (name_length == 5 && !strncasecmp(line, "Range", 5)) {
                copy_value(resp->range, sizeof(resp->range), value, value_length);
            } else if (name_length == 11 && !strncasecmp(line, "Retry-After", 11)) {
                copy_value(resp->retry_after, sizeof(resp->retry_after), value, value_length);
            }
        }
        const bool no_body = !strcmp(q->method, "HEAD") || status == 204 || status == 304;
        if (no_body) {
            r = CA_HTTPS_OK;
        } else if (chunked) {
            r = read_chunked(c, resp, error, error_size);
        } else if (have_length) {
            r = read_body_bytes(c, resp, length, error, error_size);
        } else {
            close_after = true; /* body runs to the end of the connection */
            /* What arrived together with the headers is already in the buffer: keep it first. */
            if (c->rpos < c->rlen) {
                store_body(resp, c->rbuf + c->rpos, c->rlen - c->rpos);
                c->rpos = c->rlen;
            }
            for (;;) {
                r = fill(c, error, error_size);
                if (r == 0) { r = CA_HTTPS_OK; break; }
                if (r < 0) break;
                store_body(resp, c->rbuf, c->rlen);
                c->rpos = c->rlen;
            }
        }
        if (r != CA_HTTPS_OK) return r;
        if (close_after) c->reusable = false;
        return CA_HTTPS_OK;
    }
}

int ca_https_request(struct ca_https *c, const struct ca_https_req *q, struct ca_https_response *resp,
                     char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    if (!c || !q || !resp || !q->method || !q->path || (q->length && !q->body && q->fd < 0)) {
        set_error(error, error_size, "bad request");
        return CA_HTTPS_ERR_ARGS;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        const bool reused = c->fd >= 0 && c->used;
        if (c->fd < 0 || !c->reusable) {
            disconnect(c);
            const int r = establish(c, error, error_size);
            if (r != CA_HTTPS_OK) return r;
        }
        size_t got_bytes = 0;
        bool head_sent = false;
        int r = send_request(c, q, &head_sent, error, error_size);
        if (r == CA_HTTPS_OK) {
            c->used = true;
            r = read_response(c, q, resp, &got_bytes, error, error_size);
        }
        if (r == CA_HTTPS_OK) {
            if (!c->reusable) disconnect(c);
            return CA_HTTPS_OK;
        }
        disconnect(c);
        /* The server may have closed an idle connection: try once on a new one. A
         * request with a body is only repeated if the failure came while its
         * headers were being written, since the server may have acted on the body. */
        if (r == CA_HTTPS_ERR_IO && reused && got_bytes == 0 && (q->length == 0 || !head_sent) && attempt == 0 && !stopped(c) && !c->wait_error) {
            if (error && error_size) error[0] = '\0';
            continue;
        }
        return r;
    }
    return CA_HTTPS_ERR_IO;
}
