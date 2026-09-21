#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "apcam/network.h"
#include "apcam/target.h"
#include "apcam/APC_Config.h"

#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LINE_MAX 512U

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text)) text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

void ca_config_defaults(struct ca_config *config)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    memcpy(config->timezone, APCAM_DEFAULT_TIMEZONE, sizeof(APCAM_DEFAULT_TIMEZONE));
    config->photo_scope = (enum ca_photo_scope)APCAM_DEFAULT_PHOTO_SCOPE;
    config->orientation = (enum ca_mount_orientation)APCAM_DEFAULT_ORIENTATION;
    config->uart_protocol = CA_UART_NONE;
    config->thermal_palette = CA_PALETTE_WHITE_HOT;
    config->autorecord = CA_AUTORECORD_DISABLED;
    config->raw_stream_fps = 5;
    config->raw_record_fps = 5;
    config->main_resolution = (enum ca_video_resolution)APCAM_DEFAULT_MAIN_RESOLUTION;
    config->sub_resolution = (enum ca_video_resolution)APCAM_DEFAULT_SUB_RESOLUTION;
    config->recording_resolution = (enum ca_video_resolution)APCAM_DEFAULT_RECORDING_RESOLUTION;
    config->main_codec = CA_VIDEO_H264;
    config->sub_codec = CA_VIDEO_H264;
    config->brightness = 50;
    config->saturation = 50;
    config->contrast = 50;
    config->exposure_compensation = 0;
    config->iso = CA_ISO_AUTO;
    config->shutter = CA_SHUTTER_AUTO;
    config->metering = CA_METERING_AVERAGE;
    config->white_balance = CA_WB_AUTO;
    config->position_targeting = APCAM_DEFAULT_POSITION_TARGETING;
    config->osd_recording = !APCAM_HAVE_OVERLAY_RECORDING_SELECT;
    config->tracking_method = CA_TRACK_ANGLE;
    config->mavlink_system_id = APCAM_DEFAULT_SYSTEM_ID;
    config->mavlink_camera_component_id = 100U; /* MAV_COMP_ID_CAMERA */
    config->mavlink_tcp_port = 14550U;
    config->mavlink_udp_port = 14550U;
    config->support.mavlink_port = 10001U;
    config->support.signing_link_id = 1U;
    config->support.video1_port = 0U;
    config->support.video2_port = 0U;
    strcpy(config->main_alias, APCAM_DEFAULT_MAIN_ALIAS);
    strcpy(config->sub_alias, APCAM_DEFAULT_SUB_ALIAS);
    strcpy(config->support.video1_name, "video1");
    strcpy(config->support.video2_name, "video2");
    strcpy(config->support.network_interface, "eth0");
    strcpy(config->network.interface, "eth0");
}

const char *ca_mount_orientation_name(enum ca_mount_orientation orientation)
{
    if (orientation == CA_MOUNT_UPRIGHT) return "upright";
    if (orientation == CA_MOUNT_INVERTED) return "inverted";
    return "auto";
}

const char *ca_uart_protocol_name(enum ca_uart_protocol protocol)
{
    if (protocol == CA_UART_SIYI) return "siyi";
    if (protocol == CA_UART_MAVLINK) return "mavlink";
    return "none";
}

const char *ca_video_resolution_name(enum ca_video_resolution resolution)
{
    if (resolution == CA_VIDEO_720P) return "1280x720";
    if (resolution == CA_VIDEO_1440P) return "2560x1440";
    if (resolution == CA_VIDEO_2160P) return "3840x2160";
    return "1920x1080";
}

void ca_video_resolution_size(enum ca_video_resolution resolution,
                              unsigned *width, unsigned *height)
{
    unsigned w = 1920U, h = 1080U;
    if (resolution == CA_VIDEO_720P) { w = 1280U; h = 720U; }
    else if (resolution == CA_VIDEO_1440P) { w = 2560U; h = 1440U; }
    else if (resolution == CA_VIDEO_2160P) { w = 3840U; h = 2160U; }
    if (width != NULL) *width = w;
    if (height != NULL) *height = h;
}

const char *ca_video_codec_name(enum ca_video_codec codec)
{
    return codec == CA_VIDEO_H265 ? "h265" : "h264";
}

const char *ca_thermal_palette_name(enum ca_thermal_palette palette)
{
    const auto *field = APC_Config::find("thermal", "palette");
    if (!field) return "white_hot";
    for (size_t i = 0; i < field->option_count; i++) {
        if (field->options[i].value == (int)palette) return field->options[i].name;
    }
    return "white_hot";
}

const char *ca_photo_scope_name(enum ca_photo_scope scope)
{
    return scope == CA_PHOTO_SCOPE_ALL ? "all" : "thermal";
}

int ca_config_load(struct ca_config *config, const char *path,
                   char *error, size_t error_size)
{
    FILE *file;
    char line[CONFIG_LINE_MAX];
    char section[64] = "";
    unsigned line_number = 0U;
    bool seen[APC_Config::count()] = {false};
    struct ca_config parsed;

    if (config == NULL || path == NULL || *path == '\0') {
        errno = EINVAL;
        return -1;
    }
    parsed = *config;
    file = fopen(path, "r");
    if (file == NULL) return -1;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *text;
        char *equals;
        size_t length;

        line_number++;
        length = strlen(line);
        if (length != 0U && line[length - 1U] != '\n' && !feof(file)) {
            snprintf(error, error_size, "line %u is too long", line_number);
            errno = EINVAL;
            goto fail;
        }
        text = trim(line);
        if (*text == '\0' || *text == '#' || *text == ';') continue;
        if (*text == '[') {
            char *close = strchr(text + 1, ']');
            char *name;
            if (close == NULL || *trim(close + 1) != '\0') {
                snprintf(error, error_size, "line %u has an invalid section", line_number);
                errno = EINVAL;
                goto fail;
            }
            *close = '\0';
            name = trim(text + 1);
            if (*name == '\0' || strlen(name) >= sizeof(section)) {
                snprintf(error, error_size, "line %u has an invalid section name", line_number);
                errno = EINVAL;
                goto fail;
            }
            memcpy(section, name, strlen(name) + 1U);
            continue;
        }
        equals = strchr(text, '=');
        if (equals == NULL) {
            snprintf(error, error_size, "line %u is not key=value", line_number);
            errno = EINVAL;
            goto fail;
        }
        *equals = '\0';
        char *key = trim(text);
        char *value = trim(equals + 1);
        size_t value_length = strlen(value);
        if (value_length >= 2U && value[0] == '"' &&
            value[value_length - 1U] == '"') {
            value[value_length - 1U] = '\0';
            value = strcmp(section, "support_proxy") == 0 ? value + 1 : trim(value + 1);
        }

        for (size_t i = 0; i < APC_Config::count();
             i++) {
            const APC_Config::Field *field = &APC_Config::fields()[i];
            if (strcmp(section, field->section) != 0 ||
                strcmp(key, field->key) != 0) continue;
            if (seen[i] || !APC_Config::assign(parsed, *field, value)) {
                snprintf(error, error_size,
                         "line %u has an invalid or duplicate %s",
                         line_number, field->key);
                errno = EINVAL;
                goto fail;
            }
            seen[i] = true;
            break;
        }
        /* Unknown keys are retained by the web editor and ignored here. This
         * permits newer configurations to be used with an older binary. */
    }
    if (ferror(file)) {
        snprintf(error, error_size, "read failed: %s", strerror(errno));
        goto fail;
    }
    if (!APC_Config::support_valid(&parsed.support)) {
        snprintf(error, error_size, "SupportProxy needs a host, distinct enabled video ports, stream names and a passphrase when signing is enabled");
        errno = EINVAL;
        goto fail;
    }
    /* New Network keys take precedence even when explicitly empty. Legacy
     * proxy network settings retain their old enabled-only behavior until saved. */
    if (parsed.support.enabled) {
        for (size_t i = 0; i < APC_Config::count(); i++) {
            const APC_Config::Field *f = &APC_Config::fields()[i];
            if (seen[i] || strcmp(f->section, "network")) continue;
            const char *legacy = !strcmp(f->key, "interface") ? parsed.support.network_interface :
                !strcmp(f->key, "secondary_address") ? parsed.support.network_address :
                !strcmp(f->key, "gateway") ? parsed.support.network_gateway : NULL;
            if (legacy) snprintf((char *)&parsed + f->offset, f->size, "%s", legacy);
        }
    }
    if (!apcam_network_valid(parsed.network.primary_address, parsed.network.secondary_address, parsed.network.gateway)) {
        snprintf(error, error_size, "Network needs distinct host addresses and a gateway reachable through either configured subnet");
        errno = EINVAL;
        goto fail;
    }
    fclose(file);
    *config = parsed;
    return 0;

fail:
    fclose(file);
    return -1;
}

static const APC_Config::Field *param_field(size_t index)
{
    for (size_t i = 0; i < APC_Config::count(); i++) {
        if (APC_Config::fields()[i].param_name == NULL) continue;
        if (index-- == 0U) return &APC_Config::fields()[i];
    }
    return NULL;
}

size_t ca_config_param_count(void)
{
    size_t count = 0U;
    for (size_t i = 0; i < APC_Config::count(); i++) {
        if (APC_Config::fields()[i].param_name != NULL) count++;
    }
    return count;
}

const char *ca_config_param_name(size_t index)
{
    const APC_Config::Field *field = param_field(index);
    return field != NULL ? field->param_name : NULL;
}

int ca_config_param_find(const char *name)
{
    for (size_t i = 0; i < ca_config_param_count(); i++) {
        if (strcmp(name, ca_config_param_name(i)) == 0) return (int)i;
    }
    return -1;
}

bool ca_config_param_is_bool(size_t index)
{
    const APC_Config::Field *field = param_field(index);
    return field && field->kind == APC_Config::Kind::Bool;
}

size_t ca_config_param_options(size_t index, const struct ca_config_option **options,
                                int *minimum, int *maximum)
{
    const APC_Config::Field *field = param_field(index);
    *options = field ? field->options : NULL;
    *minimum = field ? field->minimum : 0;
    *maximum = field ? (field->kind == APC_Config::Kind::Bool ? 1 : field->maximum) : 0;
    return field ? field->option_count : 0;
}

int ca_config_param_get(const struct ca_config *config, size_t index)
{
    const APC_Config::Field *field = param_field(index);
    if (field == NULL) return 0;
    const uint8_t *source = (const uint8_t *)config + field->offset;
    if (field->kind == APC_Config::Kind::Bool) {
        bool value;
        memcpy(&value, source, sizeof(value));
        return value ? 1 : 0;
    }
    if (field->kind == APC_Config::Kind::Uint) {
        unsigned value;
        memcpy(&value, source, sizeof(value));
        return (int)value;
    }
    int value;
    memcpy(&value, source, sizeof(value));
    return value;
}

/* Replace only the requested key, retaining comments, unknown settings and
 * edits made by the web UI since startup. Rename prevents a partial INI from
 * being observed by readers if power is lost during the write. */
static int save_field(const char *path, const APC_Config::Field *field,
                       const char *value)
{
    FILE *input = fopen(path, "r");
    FILE *output = NULL;
    char *temporary = NULL;
    char *line = NULL;
    size_t capacity = 0U;
    bool in_section = false, written = false, key_present = false;
    struct stat st;
    int result = -1;
    if (input == NULL && errno != ENOENT) return -1;
    /* INI sections may repeat. Locate the key before inserting a missing
     * one, or an earlier partial section could create a duplicate key. */
    while (input != NULL && getline(&line, &capacity, input) >= 0) {
        char *text = trim(line);
        if (*text == '[') {
            char *close = strchr(text + 1, ']');
            if (close != NULL) *close = '\0';
            in_section = close != NULL &&
                         strcmp(trim(text + 1), field->section) == 0;
        } else if (in_section && *text != '#' && *text != ';') {
            char *equals = strchr(text, '=');
            if (equals != NULL) {
                *equals = '\0';
                if (strcmp(trim(text), field->key) == 0) key_present = true;
            }
        }
    }
    int fd;
    if (input != NULL && (ferror(input) || fseek(input, 0, SEEK_SET) != 0)) goto done;
    in_section = false;
    if (asprintf(&temporary, "%s.XXXXXX", path) < 0) goto done;
    fd = mkstemp(temporary);
    if (fd < 0) goto done;
    output = fdopen(fd, "w");
    if (output == NULL) { close(fd); goto done; }
    if (input != NULL && fstat(fileno(input), &st) == 0 &&
        fchmod(fd, st.st_mode & 0777) < 0) goto done;
    while (input != NULL && getline(&line, &capacity, input) >= 0) {
        char *copy = strdup(line);
        if (copy == NULL) goto done;
        char *text = trim(copy);
        if (*text == '[') {
            if (in_section && !written && !key_present) {
                fprintf(output, "%s = %s\n", field->key, value);
                written = true;
            }
            char *close = strchr(text + 1, ']');
            if (close != NULL) *close = '\0';
            in_section = close != NULL &&
                         strcmp(trim(text + 1), field->section) == 0;
        } else if (in_section && *text != '#' && *text != ';') {
            char *equals = strchr(text, '=');
            if (equals != NULL) {
                *equals = '\0';
                if (strcmp(trim(text), field->key) == 0) {
                    fprintf(output, "%s = %s\n", field->key, value);
                    written = true;
                    free(copy);
                    continue;
                }
            }
        }
        free(copy);
        fputs(line, output);
        /* Ensure a missing final newline cannot join an appended setting. */
        if (*line != '\0' && line[strlen(line) - 1U] != '\n') fputc('\n', output);
    }
    if (input != NULL && ferror(input)) goto done;
    if (!written) {
        if (!in_section) fprintf(output, "\n[%s]\n", field->section);
        fprintf(output, "%s = %s\n", field->key, value);
    }
    if (ferror(output) || fflush(output) != 0 || fsync(fileno(output)) < 0) goto done;
    if (fclose(output) != 0) { output = NULL; goto done; }
    output = NULL;
    if (rename(temporary, path) < 0) goto done;
    result = 0;
done: {
        int saved_errno = errno;
        if (input != NULL) fclose(input);
        if (output != NULL) fclose(output);
        if (temporary != NULL) unlink(temporary);
        free(temporary);
        free(line);
        errno = saved_errno;
        return result;
    }
}

int ca_config_param_save(struct ca_config *config, const char *path,
                          size_t index, float value)
{
    const APC_Config::Field *field = param_field(index);
    char number[32];
    const char *text = number;
    char error[160];
    struct ca_config updated;
    if (field == NULL || !isfinite(value) || value < -65535.0f ||
        value > 65535.0f || value != (float)(int)value) {
        errno = EINVAL;
        return -1;
    }
    int selected = (int)value;
    snprintf(number, sizeof(number), "%d", selected);
    if (field->kind == APC_Config::Kind::Enum) {
        text = NULL;
        for (size_t i = 0; i < field->option_count; i++) {
            if (field->options[i].value == selected) text = field->options[i].name;
        }
    } else if (field->kind == APC_Config::Kind::Bool) {
        text = selected == 0 ? "false" : selected == 1 ? "true" : NULL;
    }
    ca_config_defaults(&updated);
    if (ca_config_load(&updated, path, error, sizeof(error)) < 0 && errno != ENOENT) {
        return -1;
    }
    if (text == NULL || !APC_Config::assign(updated, *field, text) || !APC_Config::support_valid(&updated.support)) {
        errno = EINVAL;
        return -1;
    }
    if (save_field(path, field, text) < 0) return -1;
    *config = updated;
    return 0;
}

#define IMAGE_FIELDS(X) X(brightness) X(saturation) X(contrast) X(exposure_compensation) \
    X(iso) X(shutter) X(metering) X(white_balance)

bool ca_config_image_equal(const struct ca_config *a, const struct ca_config *b)
{
#define SAME(field) if (a->field != b->field) return false;
    IMAGE_FIELDS(SAME)
#undef SAME
    return true;
}

void ca_config_copy_image(struct ca_config *destination, const struct ca_config *source)
{
#define COPY(field) destination->field = source->field;
    IMAGE_FIELDS(COPY)
#undef COPY
}

int ca_config_param_assign(struct ca_config *config, size_t index, float value)
{
    const APC_Config::Field *field = param_field(index);
    if (!field || !isfinite(value) || value < -65535 || value > 65535 || value != truncf(value)) {
        errno = EINVAL;
        return -1;
    }
    int selected = (int)value;
    char number[32];
    snprintf(number, sizeof(number), "%d", selected);
    const char *text = number;
    if (field->kind == APC_Config::Kind::Enum) {
        text = NULL;
        for (size_t i = 0; i < field->option_count; i++) {
            if (field->options[i].value == selected) text = field->options[i].name;
        }
    } else if (field->kind == APC_Config::Kind::Bool) {
        text = selected == 0 ? "false" : selected == 1 ? "true" : NULL;
    }
    struct ca_config updated = *config;
    if (!text || !APC_Config::assign(updated, *field, text) || !APC_Config::support_valid(&updated.support)) {
        errno = EINVAL;
        return -1;
    }
    *config = updated;
    return 0;
}
