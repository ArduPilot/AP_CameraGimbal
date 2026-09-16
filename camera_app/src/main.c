#define _GNU_SOURCE
#include "apcam/target.h"
#if APCAM_HAVE_XFROBOT
#include "camera_app/xfrobot_server.h"
#endif
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI && !defined(CAMERA_APP_SITL)
#include "backends/z1mini/native.h"
#endif
#include "camera_app/backend.h"
#include "camera_app/manual_control.h"
#include "camera_app/config.h"
#include "camera_app/log.h"
#include "camera_app/binlog.h"
#include "camera_app/media.h"
#include "camera_app/metadata.h"
#include "camera_app/mavlink_server.h"
#include "camera_app/support_network.h"

#include "camera_app/siyi.h"
#include "camera_app/siyi_server.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT APCAM_VENDOR_PORT
#define DEFAULT_READY_PATH "/run/camera-app.ready"
static volatile sig_atomic_t stop_requested;

static const char *environment_string(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static unsigned environment_port(const char *name, unsigned fallback)
{
    const char *text = getenv(name);
    char *end;
    unsigned long value;

    if (text == NULL || text[0] == '\0') return fallback;
    errno = 0;
    value = strtoul(text, &end, 10);
    return errno == 0 && *end == '\0' && value > 0U && value <= 65535U
               ? (unsigned)value
               : fallback;
}

static unsigned environment_optional_port(const char *name, unsigned fallback)
{
    const char *text = getenv(name);
    char *end;
    unsigned long value;

    if (text == NULL || text[0] == '\0') return fallback;
    errno = 0;
    value = strtoul(text, &end, 10);
    return errno == 0 && *end == '\0' && value <= 65535U
               ? (unsigned)value
               : fallback;
}

static int set_recording(void *opaque, bool active)
{
    return ca_media_set_recording(opaque, active);
}

static bool get_recording(void *opaque)
{
    return ca_media_recording(opaque);
}

static int set_zoom(void *opaque, float zoom)
{
    return ca_media_set_zoom(opaque, zoom);
}

static float get_zoom(void *opaque)
{
    return ca_media_zoom(opaque);
}

static int set_lens(void *opaque, bool wide)
{
    return ca_media_set_lens(opaque, wide ? CA_MEDIA_LENS_WIDE
                                          : CA_MEDIA_LENS_ZOOM);
}

static bool lens_is_wide(void *opaque)
{
    return ca_media_lens(opaque) == CA_MEDIA_LENS_WIDE;
}

static int set_thermal_main(void *opaque, bool thermal_main)
{
    return ca_media_set_thermal_main(opaque, thermal_main);
}

static bool thermal_is_main(void *opaque)
{
    return ca_media_thermal_main(opaque);
}

static int autofocus(void *opaque, uint16_t x, uint16_t y)
{
    return ca_media_autofocus(opaque, x, y);
}

static int manual_focus(void *opaque, int direction)
{
    return ca_media_manual_focus(opaque, direction);
}

static bool get_thermal_range(void *opaque, struct ca_thermal_range *range)
{
    return ca_media_thermal_range(opaque, range);
}

struct photo_context {
    struct ca_media *media;
};

static int capture_photo(void *opaque)
{
    struct photo_context *context = opaque;
    return ca_media_capture_photo(context->media, ca_media_settings(context->media)->photo_scope);
}

static int get_thermal_gain(void *opaque, uint8_t *gain)
{
    return ca_media_get_thermal_gain(opaque, gain);
}

static int set_thermal_gain(void *opaque, uint8_t gain)
{
    return ca_media_set_thermal_gain(opaque, gain);
}

static int get_thermal_palette(void *opaque, uint8_t *palette)
{
    return ca_media_get_thermal_palette(opaque, palette);
}

static int set_thermal_palette(void *opaque, uint8_t palette)
{
    return ca_media_set_thermal_palette(opaque, palette);
}

static int set_inverted(void *opaque, bool inverted)
{
    return ca_media_set_inverted(opaque, inverted);
}

static const char *ready_path(void)
{
    const char *path = getenv("CAMERA_APP_READY_PATH");
    return path != NULL ? path : DEFAULT_READY_PATH;
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
}

/* vendor media libraries keep replacing the SIGTERM disposition from their
 * own threads (SigmaStar MI sets it to SIG_IGN once the ISP is running) */
static void restore_signal_handlers(void)
{
    struct sigaction current;

    if (sigaction(SIGTERM, NULL, &current) == 0 &&
        current.sa_handler != handle_signal) {
        install_signal_handlers();
    }
}

#if APCAM_HAVE_SIYI
static void log_unknown_siyi(const struct ca_siyi_packet *packet)
{
    const size_t payload_limit = 32U;
    size_t shown = packet->payload_length < payload_limit
                           ? packet->payload_length
                           : payload_limit;

    char payload_hex[payload_limit * 2U + 4U];
    size_t used = 0;
    if (shown == 0U) {
        memcpy(payload_hex, "<empty>", 8U);
        used = 7U;
    } else {
        static const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < shown; i++) {
            payload_hex[used++] = digits[packet->payload[i] >> 4];
            payload_hex[used++] = digits[packet->payload[i] & 0x0fU];
        }
        if (shown < packet->payload_length) {
            memcpy(payload_hex + used, "...", 3U);
            used += 3U;
        }
    }
    payload_hex[used] = '\0';
    ca_log("unknown SIYI command opcode=0x%02x control=0x%02x sequence=%u "
           "payload_length=%u payload=%s",
           packet->opcode, packet->control, packet->sequence,
           packet->payload_length, payload_hex);
}

static int handle_siyi_request(void *opaque, const uint8_t *raw, size_t length)
{
    struct ca_backend *backend = opaque;
    struct ca_siyi_packet packet;
    size_t consumed = 0U;
    if (ca_siyi_parse_one(raw, length, &packet, &consumed) != 1 ||
        consumed != length) {
        errno = EPROTO;
        return -1;
    }
    if (!ca_siyi_opcode_known(packet.opcode)) log_unknown_siyi(&packet);
    if (ca_backend_handle_siyi(backend, raw, length) < 0) {
        ca_log("opcode 0x%02x failed: %s", packet.opcode, strerror(errno));
        return -1;
    }
    return 0;
}

#endif

static int write_ready(const struct ca_backend *backend, unsigned port,
                       unsigned mavlink_tcp_port, unsigned mavlink_udp_port,
                       enum ca_uart_protocol uart_protocol, unsigned manual_port)
{
    int fd = open(ready_path(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    char text[256];
    int length;

    if (fd < 0) return -1;
    length = snprintf(text, sizeof(text),
                      "backend=%s\npid=%ld\nudp_port=%u\ntcp_port=%u\n"
                      "mavlink_tcp_port=%u\nmavlink_udp_port=%u\n"
                      "uart_protocol=%s\nrecording=%u\nmanual_port=%u\n",
                      ca_backend_name(backend), (long)getpid(), port,
#if APCAM_HAVE_XFROBOT
                      port == APCAM_VENDOR_PORT ? APCAM_VENDOR_TCP_PORT : port,
#else
                      port,
#endif
                      mavlink_tcp_port, mavlink_udp_port,
                      ca_uart_protocol_name(uart_protocol),
                      ca_backend_recording(backend) ? 1U : 0U, manual_port);
    if (length < 0 || (size_t)length >= sizeof(text) ||
        write(fd, text, (size_t)length) != length) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno != 0 ? saved_errno : EIO;
        return -1;
    }
    close(fd);
    return 0;
}

static void usage(const char *program)
{
    ca_log("Usage: %s [--backend mt11|a8|zr10|z1mini] [--uart DEVICE|udp://IPv4:PORT] [--port PORT] "
           "[--config PATH]", program);
}

int main(int argc, char **argv)
{
    const char *backend_name = environment_string("CAMERA_APP_BACKEND", APCAM_NAME);
    /* NULL lets each backend pick its own gimbal UART */
    const char *uart_device = environment_string("CAMERA_APP_UART", NULL);
    const char *external_uart_device =
        environment_string("CAMERA_APP_EXTERNAL_UART", "/dev/ttyAMA4");
    const char *config_path =
        environment_string("CAMERA_APP_CONFIG", CA_CONFIG_DEFAULT_PATH);
    unsigned port = environment_port("CAMERA_APP_PORT", DEFAULT_PORT);
    struct ca_siyi_server *server = NULL;
#if APCAM_HAVE_XFROBOT
    struct ca_xfrobot_server *xfrobot = NULL;
#endif
    struct ca_mavlink_server *mavlink_server = NULL;
    struct ca_manual_control manual = {.fd=-1};
    struct ca_backend *backend = NULL;
    struct ca_media *media = NULL;
    struct ca_backend_config config;
    struct ca_config app_config;
    struct photo_context photo_context;
    char config_error[192] = "";
    int i;
    int result = 1;
    unsigned mavlink_tcp_port;
    unsigned mavlink_udp_port;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend_name = argv[++i];
        } else if (strcmp(argv[i], "--uart") == 0 && i + 1 < argc) {
            uart_device = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            char *end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || value == 0 || value > 65535) {
                usage(argv[0]);
                return 2;
            }
            port = (unsigned)value;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    ca_config_defaults(&app_config);
    if (ca_config_load(&app_config, config_path, config_error,
                       sizeof(config_error)) < 0 && errno != ENOENT) {
        ca_log("cannot load config %s: %s", config_path,
               config_error[0] != '\0' ? config_error : strerror(errno));
        return 2;
    }
    if (setenv("TZ", app_config.timezone, 1) < 0) {
        ca_log("cannot apply timezone %s: %s", app_config.timezone,
               strerror(errno));
        return 2;
    }
    tzset();
    mavlink_tcp_port = environment_optional_port(
        "CAMERA_APP_MAVLINK_TCP_PORT", app_config.mavlink_tcp_port);
    mavlink_udp_port = environment_optional_port(
        "CAMERA_APP_MAVLINK_UDP_PORT", app_config.mavlink_udp_port);
    ca_log("configuration loaded path=%s timezone=%s photo_scope=%s "
           "orientation=%s uart=%s palette=%s autorecord=%u main=%s/%s sub=%s/%s "
           "recording=%s position_targeting=%u mavlink_tcp=%u mavlink_udp=%u",
           config_path, app_config.timezone,
           ca_photo_scope_name(app_config.photo_scope),
           ca_mount_orientation_name(app_config.orientation),
           ca_uart_protocol_name(app_config.uart_protocol),
           ca_thermal_palette_name(app_config.thermal_palette),
           (unsigned)app_config.autorecord,
           ca_video_resolution_name(app_config.main_resolution),
           ca_video_codec_name(app_config.main_codec),
           ca_video_resolution_name(app_config.sub_resolution),
           ca_video_codec_name(app_config.sub_codec),
           ca_video_resolution_name(app_config.recording_resolution),
           app_config.position_targeting ? 1U : 0U,
           mavlink_tcp_port, mavlink_udp_port);

    install_signal_handlers();
    (void)unlink(ready_path());

#if APCAM_HAVE_SIYI
    const char *siyi_uart = app_config.uart_protocol == CA_UART_SIYI
                                ? external_uart_device
                                : NULL;
    if (ca_siyi_server_open(&server, port, siyi_uart) < 0) {
        ca_log("cannot bind SIYI UDP/TCP %u: %s", port, strerror(errno));
        goto done;
    }
#else
    if (ca_xfrobot_server_open(&xfrobot, port, app_config.orientation == CA_MOUNT_INVERTED) < 0) goto done;
#endif
    const char *record_root = environment_string("CAMERA_APP_RECORD_ROOT",
                                                 "/mnt/DCIM/record");
    const char *capture_root = environment_string("CAMERA_APP_CAPTURE_ROOT",
                                                  "/mnt/DCIM/capture");
    const char *log_root = environment_string("CAMERA_APP_LOG_ROOT", APCAM_LOG_ROOT);
#ifdef CAMERA_APP_SITL
    /* Keep isolated tests/portable installs off the host /mnt filesystem. */
    char sitl_log_root[4096];
    if (!getenv("CAMERA_APP_LOG_ROOT")) {
        snprintf(sitl_log_root, sizeof(sitl_log_root), "%s/../logs", record_root);
        log_root = sitl_log_root;
    }
#endif
    if (ca_binlog_init(log_root) < 0) ca_log("cannot start BIN log writer: %s", strerror(errno));
    unsigned rtsp_port = environment_port("CAMERA_APP_RTSP_PORT", 8554U);
    struct ca_media_config media_config = {
        .backend = backend_name,
        .record_root = record_root,
        .capture_root = capture_root,
        .rtsp_port = rtsp_port,
        .width = 1920,
        .height = 1080,
        .frame_rate = APCAM_FRAME_RATE,
        .bit_rate_kbps = 4096,
        .settings = app_config,
    };
#ifdef CAMERA_APP_SITL
    if (app_config.support.enabled &&
        (app_config.support.network_address[0] || app_config.support.network_gateway[0]))
        ca_log("SupportProxy network configuration skipped in SITL");
#else
    if (ca_support_network_configure(&app_config.support) < 0)
        ca_log("SupportProxy network configuration failed: %s", strerror(errno));
#endif
    if (ca_media_open(&media, &media_config) < 0) {
        ca_log("cannot open media backend %s: %s", backend_name, strerror(errno));
        goto done;
    }
    config.manual_control=&manual.active;
    config.manual_command=&manual.executing;
    config.name = backend_name;
    config.uart_device = uart_device;
#if APCAM_HAVE_XFROBOT
    config.emit = NULL;
#else
    config.emit = ca_siyi_server_emit;
#endif
    config.emit_opaque = server;
    config.recording_set = set_recording;
    config.recording_get = get_recording;
    config.recording_opaque = media;
    config.zoom_set = set_zoom;
    config.zoom_get = get_zoom;
    config.zoom_opaque = media;
    config.lens_set = set_lens;
    config.lens_wide = lens_is_wide;
    config.lens_opaque = media;
    config.thermal_main_set = set_thermal_main;
    config.thermal_main_get = thermal_is_main;
    config.thermal_main_opaque = media;
    config.autofocus = autofocus;
    config.autofocus_opaque = media;
    config.manual_focus = manual_focus;
    config.manual_focus_opaque = media;
    config.thermal_range = get_thermal_range;
    config.thermal_opaque = media;
    photo_context.media = media;
    config.photo_capture = capture_photo;
    config.photo_capture_opaque = &photo_context;
    config.thermal_gain_get = get_thermal_gain;
    config.thermal_gain_set = set_thermal_gain;
    config.thermal_gain_opaque = media;
    config.thermal_palette_get = get_thermal_palette;
    config.thermal_palette_set = set_thermal_palette;
    config.thermal_palette_opaque = media;
    config.orientation = app_config.orientation;
    config.inverted_set = set_inverted;
    config.inverted_opaque = media;
    if (ca_backend_open(&backend, &config) < 0) {
        ca_log("cannot open backend %s: %s", backend_name, strerror(errno));
        goto done;
    }
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
    /* Do not report ready before gimbal feedback is available. */
    bool gimbal_ready = false;
    for (unsigned attempt = 0; attempt < 150 && !stop_requested; attempt++) {
        struct pollfd item = {.fd = ca_backend_fd(backend), .events = POLLIN};
        if (poll(&item, 1, 20) > 0 && ca_backend_handle_fd(backend) < 0) break;
        ca_backend_periodic(backend);
        struct ca_gimbal_attitude attitude;
        if (ca_backend_gimbal_attitude(backend, &attitude)) { gimbal_ready = true; break; }
    }
    if (!gimbal_ready) { ca_log("Z1 gimbal feedback unavailable"); goto done; }
#endif
    ca_metadata_set_model(APCAM_MODEL_NAME);
    struct ca_mavlink_server_config mavlink_config = {
        .config_path = config_path,
        .tcp_port = mavlink_tcp_port,
        .udp_port = mavlink_udp_port,
        .uart_device = app_config.uart_protocol == CA_UART_MAVLINK
                           ? external_uart_device
                           : NULL,
        .rtsp_port = rtsp_port,
        .capture_root = capture_root,
        .photo_scope = app_config.photo_scope,
        .settings = app_config,
        .backend = backend,
        .manual_control = &manual.active,
        .media = media,
    };
    if (ca_mavlink_server_open(&mavlink_server, &mavlink_config) < 0) {
        ca_log("cannot initialize MAVLink: %s", strerror(errno));
        goto done;
    }
    if (ca_manual_control_open(&manual,backend,mavlink_server)<0)
        ca_log("web manual control unavailable: %s",strerror(errno));
    if (app_config.autorecord == CA_AUTORECORD_ENABLED &&
        ca_media_set_recording(media, true) < 0) {
        ca_log("automatic recording could not start: %s", strerror(errno));
    }
    /* The vendor media libraries install signal handlers while starting ISP.
     * Restore application-owned shutdown handling once all of them are loaded. */
    install_signal_handlers();
    if (write_ready(backend, port, mavlink_tcp_port, mavlink_udp_port,
                    app_config.uart_protocol, manual.port) < 0) {
        ca_log("cannot publish readiness: %s", strerror(errno));
        goto done;
    }
    ca_log("ready backend=%s vendor_port=%u configured MAVLink/TCP=%u MAVLink/UDP=%u "
           "UART4=%s/230400 PID=%ld",
           backend_name, port, mavlink_tcp_port, mavlink_udp_port,
           ca_uart_protocol_name(app_config.uart_protocol),
           (long)getpid());

    unsigned loops = 0;
    while (!stop_requested) {
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI && !defined(CAMERA_APP_SITL)
        const char *native_helper = getenv("CAMERA_APP_Z1_NATIVE_HELPER");
        if (native_helper && *native_helper && !ca_media_ready(media)) {
            ca_log("Z1 native capture stopped; exiting for supervisor recovery");
            goto done;
        }
#endif
#if APCAM_HAVE_XFROBOT
        ca_xfrobot_server_update(xfrobot, backend, media, manual.active);
#endif
        if (++loops % 50U == 0U) restore_signal_handlers();
        struct pollfd items[4] = {
            {.fd = ca_siyi_server_fd(server), .events = POLLIN},
            {.fd = ca_backend_fd(backend), .events = POLLIN},
            {.fd = ca_mavlink_server_fd(mavlink_server), .events = POLLIN},
            {.fd = manual.fd, .events = POLLIN},
        };
        int ready = poll(items, 4, 20);
        if (ready < 0) {
            if (errno == EINTR) continue;
            ca_log("poll failed: %s", strerror(errno));
            goto done;
        }
        ca_manual_control_update(&manual);
        if ((items[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ca_log("backend descriptor failed");
            goto done;
        }
        if ((items[1].revents & POLLIN) != 0 && ca_backend_handle_fd(backend) < 0) {
            ca_log("backend read failed: %s", strerror(errno));
            goto done;
        }
#if APCAM_HAVE_SIYI
        if ((items[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ca_log("SIYI server descriptor failed");
            goto done;
        }
        if (((items[0].revents & POLLIN) != 0 || items[0].fd < 0) &&
            ca_siyi_server_handle(server, handle_siyi_request, backend) < 0) {
            ca_log("SIYI server failed: %s", strerror(errno));
            goto done;
        }
#endif
        if ((items[2].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ca_log("MAVLink server descriptor failed");
            goto done;
        }
        if (((items[2].revents & POLLIN) != 0 || items[2].fd < 0) &&
            ca_mavlink_server_handle(mavlink_server) < 0) {
            ca_log("MAVLink server failed: %s", strerror(errno));
            goto done;
        }
        ca_backend_periodic(backend);
        ca_mavlink_server_periodic(mavlink_server);
    }
    result = 0;

done:
    (void)unlink(ready_path());
    ca_manual_control_close(&manual);
    ca_mavlink_server_close(mavlink_server);
    ca_backend_close(backend);
    ca_media_close(media);
    ca_binlog_close();
#if APCAM_HAVE_XFROBOT
    ca_xfrobot_server_close(xfrobot);
#endif
    ca_siyi_server_close(server);
    ca_log("stopped");
    return result;
}
