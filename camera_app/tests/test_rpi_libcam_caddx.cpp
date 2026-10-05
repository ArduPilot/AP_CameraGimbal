/* rpi_libcam_caddx image control mapping and CADDX gimbal; needs no libcamera. */
#include "../src/backends/rpi_libcam/pipeline.h"
#include "../src/backends/caddx/caddx.h"
#include "camera_app/backend.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void near(float actual, float expected) { assert(fabsf(actual - expected) < 0.0001f); }

static bool recording_get(void *opaque) { return *static_cast<bool *>(opaque); }

static const float DEG = 3.14159265358979323846f / 180.0f;

/* Reference packets from ArduPilot's AP_Mount_CADDX encoding (roll/pitch locked). */
static void caddx_packets(void)
{
    static const uint8_t neutral[10] = {0xa5, 0x5a, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x6e};
    static const uint8_t down45_right90[10] = {0xa5, 0x5a, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x40, 0x72, 0xa5};
    static const uint8_t down90_left45[10] = {0xa5, 0x5a, 0x03, 0x00, 0x00, 0x00, 0x0c, 0xe0, 0xa1, 0x2d};
    uint8_t packet[10];
    ca_caddx_pack(packet, 0, 0, 0);
    assert(memcmp(packet, neutral, sizeof(packet)) == 0);
    ca_caddx_pack(packet, 0, -45 * DEG, 90 * DEG);
    assert(memcmp(packet, down45_right90, sizeof(packet)) == 0);
    ca_caddx_pack(packet, 0, -90 * DEG, -45 * DEG);
    assert(memcmp(packet, down90_left45, sizeof(packet)) == 0);
    /* A full turn wraps to zero rather than overflowing 12 bits. */
    ca_caddx_pack(packet, 0, 0, 359.99f * DEG);
    assert(memcmp(packet, neutral, sizeof(packet)) == 0);
}

/* A closed passthrough port, as while the flight controller reboots, raises
 * ICMP errors; sending resumes once the port is listening again. */
static void caddx_udp_unreachable(void)
{
    int server = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in address = {};
    socklen_t length = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(server >= 0 && bind(server, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(server, (struct sockaddr *)&address, &length) == 0);
    close(server);
    char device[64];
    snprintf(device, sizeof(device), "udp://127.0.0.1:%u", ntohs(address.sin_port));

    struct ca_backend_config config = {};
    config.uart_device = device;
    struct ca_backend *backend = NULL;
    assert(ca_backend_open(&backend, &config) == 0);
    assert(ca_backend_set_gimbal_angles(backend, 0, 0) == 0);
    for (unsigned i = 0; i < 10; i++) {
        ca_backend_periodic(backend);
        usleep(25000);
    }
    assert(ca_backend_fd(backend) < 0);

    server = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(server >= 0 && bind(server, (struct sockaddr *)&address, sizeof(address)) == 0);
    struct pollfd item = {.fd = server, .events = POLLIN};
    for (unsigned i = 0; i < 20 && poll(&item, 1, 0) == 0; i++) {
        ca_backend_periodic(backend);
        usleep(25000);
    }
    uint8_t packet[16];
    assert(poll(&item, 1, 0) == 1 && recv(server, packet, sizeof(packet), 0) == 10);
    assert(packet[0] == 0xa5 && packet[1] == 0x5a);
    ca_backend_close(backend);
    close(server);
}

/* The UDP transport used through a flight controller's serial passthrough. */
static void caddx_udp(void)
{
    int server = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in address = {};
    socklen_t length = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(server >= 0 && bind(server, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(server, (struct sockaddr *)&address, &length) == 0);
    char device[64];
    snprintf(device, sizeof(device), "udp://127.0.0.1:%u", ntohs(address.sin_port));

    struct ca_backend_config config = {};
    config.uart_device = device;
    struct ca_backend *backend = NULL;
    assert(ca_backend_open(&backend, &config) == 0);
    /* The main loop must not poll a UDP link (see caddx_udp_unreachable). */
    assert(ca_backend_fd(backend) < 0);
    assert(ca_backend_has_gimbal(backend));

    /* Nothing is sent, and no attitude reported, before the first command. */
    struct ca_gimbal_attitude attitude;
    ca_backend_periodic(backend);
    struct pollfd item = {.fd = server, .events = POLLIN};
    assert(poll(&item, 1, 50) == 0);
    assert(!ca_backend_gimbal_attitude(backend, &attitude));

    assert(ca_backend_set_gimbal_angles(backend, -45 * DEG, 90 * DEG) == 0);
    ca_backend_periodic(backend);
    uint8_t packet[16];
    assert(poll(&item, 1, 1000) == 1);
    assert(recv(server, packet, sizeof(packet), 0) == 10);
    static const uint8_t expected[10] = {0xa5, 0x5a, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x40, 0x72, 0xa5};
    assert(memcmp(packet, expected, sizeof(expected)) == 0);
    assert(ca_backend_gimbal_attitude(backend, &attitude));
    assert(fabsf(attitude.pitch_rad + 45 * DEG) < 1e-5f && fabsf(attitude.yaw_rad - 90 * DEG) < 1e-5f);

    /* Targets are limited to the gimbal's range. */
    assert(ca_backend_set_gimbal_angles(backend, -120 * DEG, 0) == 0);
    assert(ca_backend_gimbal_attitude(backend, &attitude));
    assert(fabsf(attitude.pitch_rad + 90 * DEG) < 1e-5f);
    errno = 0;
    assert(ca_backend_set_gimbal_angles(backend, NAN, 0) < 0 && errno == EINVAL);
    ca_backend_close(backend);
    close(server);
}

int main(void)
{
    struct ca_config settings;
    ca_config_defaults(&settings);
    struct ca_rpi_image_controls c;
    ca_rpi_image_controls(&settings, &c);
    near(c.brightness, 0); near(c.contrast, 1); near(c.saturation, 1);
    near(c.exposure_value, 0);
    assert(c.analogue_gain == 0 && c.exposure_us == 0 && c.mode == CA_AE_AUTO);

    settings.brightness = 100; settings.contrast = 0; settings.saturation = 75;
    settings.exposure_compensation = -10;
    settings.iso = CA_ISO_400;
    ca_rpi_image_controls(&settings, &c);
    near(c.brightness, 1); near(c.contrast, 0); near(c.saturation, 1.5f);
    near(c.exposure_value, -1);
    near(c.analogue_gain, 4);
    assert(c.exposure_us == 0 && c.mode == CA_AE_GAIN_PRIORITY);

    settings.brightness = 0; settings.iso = CA_ISO_100; settings.shutter = CA_SHUTTER_1_1000;
    ca_rpi_image_controls(&settings, &c);
    near(c.brightness, -1); near(c.analogue_gain, 1);
    assert(c.exposure_us == 1000 && c.mode == CA_AE_MANUAL);

    settings.iso = CA_ISO_AUTO; settings.shutter = CA_SHUTTER_1_30;
    ca_rpi_image_controls(&settings, &c);
    assert(c.analogue_gain == 0 && c.exposure_us == 33333 && c.mode == CA_AE_SHUTTER_PRIORITY);
    settings.iso = CA_ISO_3200; settings.shutter = CA_SHUTTER_1_2000;
    ca_rpi_image_controls(&settings, &c);
    near(c.analogue_gain, 32); assert(c.exposure_us == 500);

    bool recording = true;
    struct ca_backend_config config = {};
    config.recording_get = recording_get;
    config.recording_opaque = &recording;
    struct ca_backend *backend = NULL;
    assert(ca_backend_open(&backend, &config) == 0);
    assert(ca_backend_fd(backend) < 0);
    assert(!ca_backend_has_gimbal(backend));
    assert(ca_backend_handle_fd(backend) == 0);
    struct ca_gimbal_attitude attitude;
    assert(!ca_backend_gimbal_attitude(backend, &attitude));
    errno = 0;
    assert(ca_backend_set_gimbal_angles(backend, 0, 0) < 0 && errno == ENOTSUP);
    assert(ca_backend_set_gimbal_rates(backend, 0, 0) < 0 && errno == ENOTSUP);
    assert(ca_backend_recording(backend));
    recording = false;
    assert(!ca_backend_recording(backend));
    ca_backend_periodic(backend);
    ca_backend_close(backend);
    caddx_packets();
    caddx_udp();
    caddx_udp_unreachable();
    puts("rpi_libcam_caddx image controls and CADDX gimbal passed");
    return 0;
}
