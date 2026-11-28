#ifndef CAMERA_APP_UPLOAD_UTIL_H
#define CAMERA_APP_UPLOAD_UTIL_H
/* Small helpers shared by the cloud upload backends: JSON values, URL encoding
 * and the credentials file. Not a general JSON parser; it reads what the
 * services' replies need. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Overwrites memory that held a secret. explicit_bzero is not in every C library the cameras use (uClibc
 * lacks it), and a plain memset can be optimised away; stores through a volatile pointer are not. */
static inline void secure_zero(void *memory, size_t length)
{
    volatile unsigned char *p = (volatile unsigned char *)memory;
    while (length--) *p++ = 0;
}

static inline void set_error(char *error, size_t size, const char *format, ...)
{
    if (!error || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

static inline uint64_t monotonic_ms()
{
    timespec t {};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}

/* ---- JSON: only what Google's replies need ---- */

/* The string value of the first "key" in the text. Escapes are decoded; \u
 * sequences become '?'. */
static inline bool json_string(const char *text, const char *key, char *out, size_t size)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = text;
    while ((p = strstr(p, pattern)) != NULL) {
        p += strlen(pattern);
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p != ':') continue; /* the text was a value, not a key */
        p++;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p != '"') return false;
        size_t used = 0;
        for (p++; *p && *p != '"'; p++) {
            char c = *p;
            if (c == '\\' && p[1]) {
                p++;
                if (*p == 'u') {
                    for (int i = 0; i < 4 && p[1]; i++) p++;
                    c = '?';
                } else if (*p == 'n' || *p == 't' || *p == 'r') {
                    c = ' ';
                } else {
                    c = *p;
                }
            }
            if (used + 1 >= size) return false; /* it does not fit: never hand back half of it */
            out[used++] = c;
        }
        if (*p != '"') return false; /* no closing quote: the text was cut short */
        out[used] = '\0';
        return true;
    }
    return false;
}

static inline bool json_number(const char *text, const char *key, double *out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(text, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    char *end;
    const double value = strtod(p, &end);
    if (end == p) return false;
    *out = value;
    return true;
}

/* Appends `text` as the inside of a JSON string. */
static inline bool json_escape(char *out, size_t size, size_t *used, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        char piece[8];
        size_t n;
        if (*p == '"' || *p == '\\') n = (size_t)snprintf(piece, sizeof(piece), "\\%c", *p);
        else if (*p < 0x20) n = (size_t)snprintf(piece, sizeof(piece), "\\u%04x", *p);
        else { piece[0] = (char)*p; n = 1; }
        if (*used + n + 1 > size) return false;
        memcpy(out + *used, piece, n);
        *used += n;
    }
    out[*used] = '\0';
    return true;
}

/* Appends `text` percent-encoded (everything but unreserved characters). */
static inline bool url_encode(char *out, size_t size, size_t *used, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        const bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                           *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (*used + (plain ? 1U : 3U) + 1 > size) return false;
        if (plain) out[(*used)++] = (char)*p;
        else *used += (size_t)snprintf(out + *used, 4, "%%%02X", *p);
    }
    out[*used] = '\0';
    return true;
}

/* ---- the credentials file ---- */

/* Copies `key = value` from the named [section] of an rclone-format file. */
static inline bool config_value(const char *path, const char *section, const char *key, char *out, size_t size)
{
    FILE *file = fopen(path, "re");
    if (!file) return false;
    char header[64];
    snprintf(header, sizeof(header), "[%s]", section);
    char *line = (char *)malloc(8192);
    bool in_section = false, found = false;
    while (line && fgets(line, 8192, file)) {
        if (line[0] == '[') {
            in_section = strncmp(line, header, strlen(header)) == 0;
        } else if (in_section && !strncmp(line, key, strlen(key))) {
            const char *p = line + strlen(key);
            while (*p == ' ') p++;
            if (*p != '=') continue;
            p++;
            while (*p == ' ') p++;
            size_t length = strcspn(p, "\r\n");
            if (length >= size) break; /* too long for the caller: not found, rather than cut short */
            memcpy(out, p, length);
            out[length] = '\0';
            found = true;
            break;
        }
    }
    if (line) secure_zero(line, 8192); /* it held credentials */
    free(line);
    fclose(file);
    return found;
}

#endif
