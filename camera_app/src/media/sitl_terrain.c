#define _GNU_SOURCE
#include "camera_app/sitl_terrain.h"
#include "camera_app/video_metadata.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

extern char **environ;
struct ca_sitl_terrain { int fd; pid_t pid; };

static int transfer(int fd, void *buffer, size_t length, bool writing)
{
    uint8_t *p = buffer;
    while (length) {
        struct pollfd f = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
        int ready = poll(&f, 1, 10000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { if (!ready) errno = ETIMEDOUT; return -1; }
        ssize_t n = writing ? send(fd, p, length, MSG_NOSIGNAL) : recv(fd, p, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
        p += n; length -= (size_t)n;
    }
    return 0;
}

int ca_sitl_terrain_open(struct ca_sitl_terrain **out, const char *script,
                         const unsigned width[4], const unsigned height[4], unsigned fps)
{
    int pair[2] = {-1, -1};
    const char *link_option = "--fd", *link_value = "3";
    char token[33] = "";
#ifdef __CYGWIN__
    /* Native Windows Python cannot inherit a Cygwin socket descriptor. */
    char port[16];
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t address_size = sizeof(address);
    pair[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (pair[0] < 0) return -1;
    if (bind(pair[0], (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(pair[0], 1) < 0 ||
        getsockname(pair[0], (struct sockaddr *)&address, &address_size) < 0) {
        close(pair[0]); return -1;
    }
    unsigned char random[16];
    arc4random_buf(random, sizeof(random));
    for (unsigned i = 0; i < sizeof(random); i++) snprintf(token + 2*i, 3, "%02x", random[i]);
    snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));
    link_option = "--connect"; link_value = port;
#else
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) return -1;
#endif
    struct ca_sitl_terrain *t = calloc(1, sizeof(*t));
    if (!t) { close(pair[0]); close(pair[1]); return -1; }
    t->fd = pair[0];
    char w1[16], h1[16], w2[16], h2[16], w3[16], h3[16], w4[16], h4[16], rate[16];
    snprintf(w1, sizeof(w1), "%u", width[0]); snprintf(h1, sizeof(h1), "%u", height[0]);
    snprintf(w2, sizeof(w2), "%u", width[1]); snprintf(h2, sizeof(h2), "%u", height[1]);
    snprintf(w3, sizeof(w3), "%u", width[2]); snprintf(h3, sizeof(h3), "%u", height[2]);
    snprintf(w4, sizeof(w4), "%u", width[3]); snprintf(h4, sizeof(h4), "%u", height[3]);
    snprintf(rate, sizeof(rate), "%u", fps);
    const char *python = getenv("CAMERA_GIMBAL_SITL_PYTHON");
    if (!python || !*python) python = "python3";
    char *argv[] = {(char *)python, (char *)script, (char *)link_option, (char *)link_value, "--width1", w1,
                    "--height1", h1, "--width2", w2, "--height2", h2, "--width3", w3, "--height3", h3, "--width4", w4, "--height4", h4, "--fps", rate, "--token", token, NULL};
    posix_spawn_file_actions_t actions;
    int code = posix_spawn_file_actions_init(&actions);
    if (!code) {
        code = posix_spawn_file_actions_addclose(&actions, pair[0]);
#ifndef __CYGWIN__
        if (!code) code = posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
        if (!code && pair[1] != 3) code = posix_spawn_file_actions_addclose(&actions, pair[1]);
#endif
        if (!code) code = posix_spawnp(&t->pid, python, &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(pair[1]);
    if (code) { close(t->fd); free(t); errno = code; return -1; }
#ifdef __CYGWIN__
    struct pollfd listener = {.fd = t->fd, .events = POLLIN};
    int connected = poll(&listener, 1, 30000) > 0 ? accept4(t->fd, NULL, NULL, SOCK_CLOEXEC) : -1;
    close(t->fd);
    t->fd = connected;
    char received[32];
    if (connected < 0 || transfer(connected, received, sizeof(received), false) < 0 ||
        memcmp(received, token, sizeof(received)) != 0) {
        ca_sitl_terrain_close(t); errno = EPROTO; return -1;
    }
#endif
    *out = t;
    return 0;
}

int ca_sitl_terrain_frame(struct ca_sitl_terrain *t, uint64_t pts, uint64_t presentation_ms,
                          const float hfov[2], bool thermal_main, bool has_thermal, bool separate_recording,
                          uint8_t *data[4], size_t length[4], bool key[4])
{
    struct ca_metadata metadata;
    struct timespec utc, now;
    ca_metadata_snapshot(&metadata);
    clock_gettime(CLOCK_REALTIME, &utc);
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ms = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    unsigned lead_ms = presentation_ms > now_ms ? (unsigned)(presentation_ms - now_ms) : 0U;
    if (lead_ms > 250U) lead_ms = 250U;
    char request[CA_VIDEO_METADATA_JSON_MAX + 200];
    size_t n = ca_video_metadata_json(&metadata, &utc, pts, request, sizeof(request));
    if (!n) { errno = EINVAL; return -1; }
    int extra = snprintf(request + n - 1, sizeof(request) - n + 1,
                          ",\"fov\":[%.4f,%.4f],\"thermal\":[false,%s],\"swap\":%s,\"recording\":%s,\"prediction_ms\":%u}\n",
                          (double)hfov[0], (double)hfov[1],
                          has_thermal ? "true" : "false", thermal_main ? "true" : "false",
                          separate_recording ? "true" : "false", lead_ms);
    if (extra < 0 || (size_t)extra >= sizeof(request) - n + 1) { errno = EINVAL; return -1; }
    if (transfer(t->fd, request, n - 1 + (size_t)extra, true) < 0) return -1;
    for (unsigned i = 0; i < 4; i++) {
        uint32_t header[2];
        if (transfer(t->fd, header, sizeof(header), false) < 0) return -1;
        length[i] = ntohl(header[0]); key[i] = ntohl(header[1]) != 0;
        if (i >= 2 && length[i] == 0) continue;
        if (!length[i] || length[i] > 8U * 1024U * 1024U) { errno = EPROTO; return -1; }
        data[i] = malloc(length[i]);
        if (!data[i] || transfer(t->fd, data[i], length[i], false) < 0) return -1;
    }
    return 0;
}

void ca_sitl_terrain_interrupt(struct ca_sitl_terrain *t)
{
    if (t) shutdown(t->fd, SHUT_RDWR);
}

void ca_sitl_terrain_close(struct ca_sitl_terrain *t)
{
    if (!t) return;
    close(t->fd);
    /* Renderer owns only this camera's resources; never leave it behind on restart. */
    kill(t->pid, SIGTERM);
    while (waitpid(t->pid, NULL, 0) < 0 && errno == EINTR) {}
    free(t);
}
