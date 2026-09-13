#define _GNU_SOURCE
#include "camera_app/raw_thermal_server.h"

#include "camera_app/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define SEND_CHUNK_BYTES 1024U
#define TARGET_SEND_TIME_US 500000U

struct ca_raw_thermal_server {
    int listen_fd;
    pthread_t thread;
    bool thread_started;
    pthread_mutex_t lock;
    bool lock_initialized;
    atomic_bool stop;
    uint8_t *latest;
    size_t frame_bytes;
    uint32_t width;
    uint32_t height;
    char capture_root[PATH_MAX];
    char filename[CA_RAW_THERMAL_FILENAME_BYTES];
    struct timespec captured_at;
    bool have_latest;
    unsigned port;
    unsigned served;
};

static bool send_all(struct ca_raw_thermal_server *server, int fd,
                     const uint8_t *data, size_t length)
{
    while (length != 0U && !atomic_load(&server->stop)) {
        ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);

        if (sent > 0) {
            data += (size_t)sent;
            length -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        return false;
    }
    return length == 0U;
}

static bool send_frame(struct ca_raw_thermal_server *server, int fd)
{
    uint8_t header[CA_RAW_THERMAL_HEADER_BYTES] = {0};
    uint8_t *snapshot;
    struct timespec captured_at;
    double timestamp;
    bool sent = false;

    snapshot = malloc(server->frame_bytes);
    if (snapshot == NULL) return false;
    pthread_mutex_lock(&server->lock);
    if (!server->have_latest) {
        pthread_mutex_unlock(&server->lock);
        free(snapshot);
        return false;
    }
    memcpy(snapshot, server->latest, server->frame_bytes);
    memcpy(header, server->filename, sizeof(server->filename));
    captured_at = server->captured_at;
    pthread_mutex_unlock(&server->lock);

    timestamp = (double)captured_at.tv_sec +
                (double)captured_at.tv_nsec * 1.0e-9;
    memcpy(header + CA_RAW_THERMAL_FILENAME_BYTES, &timestamp,
           sizeof(timestamp));
    if (!send_all(server, fd, header, sizeof(header))) goto done;
    for (size_t offset = 0; offset < server->frame_bytes;) {
        size_t bytes = server->frame_bytes - offset;
        useconds_t delay;

        if (bytes > SEND_CHUNK_BYTES) bytes = SEND_CHUNK_BYTES;
        if (!send_all(server, fd, snapshot + offset, bytes)) goto done;
        offset += bytes;
        delay = (useconds_t)((uint64_t)TARGET_SEND_TIME_US * bytes /
                             server->frame_bytes);
        if (delay != 0U) (void)usleep(delay);
    }
    sent = true;
done:
    free(snapshot);
    return sent;
}

static void *server_thread(void *opaque)
{
    struct ca_raw_thermal_server *server = opaque;

    while (!atomic_load(&server->stop)) {
        struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
        int fd = accept4(server->listen_fd, NULL, NULL, SOCK_CLOEXEC);

        if (fd < 0) {
            if (errno == EINTR) continue;
            if (atomic_load(&server->stop) || errno == EBADF ||
                errno == EINVAL) break;
            ca_log("raw thermal TCP accept failed: %s", strerror(errno));
            continue;
        }
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout));
        if (send_frame(server, fd)) {
            server->served++;
            if (server->served == 1U || server->served % 100U == 0U) {
                ca_log("raw thermal TCP frame %u served uncompressed",
                       server->served);
            }
        }
        close(fd);
    }
    return NULL;
}

int ca_raw_thermal_server_open(struct ca_raw_thermal_server **result,
                               const char *capture_root, unsigned port,
                               uint32_t width, uint32_t height)
{
    struct ca_raw_thermal_server *server;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    socklen_t address_length = sizeof(address);
    int one = 1;
    int thread_result;

    if (result == NULL || capture_root == NULL || *capture_root == '\0' ||
        strlen(capture_root) >= PATH_MAX || port > 65535U || width == 0U ||
        height == 0U || (size_t)width > SIZE_MAX / height ||
        (size_t)width * height > SIZE_MAX / sizeof(uint16_t) ||
        sizeof(double) != 8U) {
        errno = EINVAL;
        return -1;
    }
    server = calloc(1, sizeof(*server));
    if (server == NULL) return -1;
    server->listen_fd = -1;
    server->width = width;
    server->height = height;
    server->frame_bytes = (size_t)width * height * sizeof(uint16_t);
    memcpy(server->capture_root, capture_root, strlen(capture_root) + 1U);
    server->latest = malloc(server->frame_bytes);
    if (server->latest == NULL) goto fail;
    thread_result = pthread_mutex_init(&server->lock, NULL);
    if (thread_result != 0) {
        errno = thread_result;
        goto fail;
    }
    server->lock_initialized = true;
    server->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server->listen_fd < 0 ||
        setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one,
                   sizeof(one)) < 0 ||
        bind(server->listen_fd, (const struct sockaddr *)&address,
             sizeof(address)) < 0 ||
        listen(server->listen_fd, 20) < 0 ||
        getsockname(server->listen_fd, (struct sockaddr *)&address,
                    &address_length) < 0) {
        goto fail;
    }
    server->port = ntohs(address.sin_port);
    thread_result = pthread_create(&server->thread, NULL, server_thread, server);
    if (thread_result != 0) {
        errno = thread_result;
        goto fail;
    }
    server->thread_started = true;
    ca_log("raw thermal TCP ready: port=%u format=filename128+timestamp64+%zu raw bytes",
           server->port, server->frame_bytes);
    *result = server;
    return 0;
fail:
    {
        int saved_errno = errno;
        ca_raw_thermal_server_close(server);
        errno = saved_errno;
    }
    return -1;
}

int ca_raw_thermal_server_publish(struct ca_raw_thermal_server *server,
                                  const uint16_t *pixels,
                                  const struct timespec *captured_at)
{
    struct tm local;
    char day[16];
    char filename[CA_RAW_THERMAL_FILENAME_BYTES];
    unsigned milliseconds;
    int length;

    if (server == NULL || pixels == NULL || captured_at == NULL ||
        captured_at->tv_sec < 0 || captured_at->tv_nsec < 0 ||
        captured_at->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    if (localtime_r(&captured_at->tv_sec, &local) == NULL ||
        strftime(day, sizeof(day), "%Y-%m-%d", &local) == 0U) {
        errno = EINVAL;
        return -1;
    }
    milliseconds = (unsigned)captured_at->tv_nsec / 1000000U;
    length = snprintf(filename, sizeof(filename),
                      "%s/%s/%s_%02d-%02d-%02d_%u_I.bin",
                      server->capture_root, day, day, local.tm_hour,
                      local.tm_min, local.tm_sec, milliseconds);
    if (length < 0 || (size_t)length >= sizeof(filename)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    pthread_mutex_lock(&server->lock);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    memcpy(server->latest, pixels, server->frame_bytes);
#else
    for (size_t i = 0; i < server->frame_bytes / 2U; i++) {
        server->latest[i * 2U] = (uint8_t)pixels[i];
        server->latest[i * 2U + 1U] = (uint8_t)(pixels[i] >> 8U);
    }
#endif
    memset(server->filename, 0, sizeof(server->filename));
    memcpy(server->filename, filename, (size_t)length);
    server->captured_at = *captured_at;
    server->have_latest = true;
    pthread_mutex_unlock(&server->lock);
    return 0;
}

unsigned ca_raw_thermal_server_port(const struct ca_raw_thermal_server *server)
{
    return server != NULL ? server->port : 0U;
}

void ca_raw_thermal_server_close(struct ca_raw_thermal_server *server)
{
    if (server == NULL) return;
    atomic_store(&server->stop, true);
    if (server->listen_fd >= 0) {
        (void)shutdown(server->listen_fd, SHUT_RDWR);
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    if (server->thread_started) pthread_join(server->thread, NULL);
    if (server->lock_initialized) (void)pthread_mutex_destroy(&server->lock);
    free(server->latest);
    free(server);
}
