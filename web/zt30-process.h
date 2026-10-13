#pragma once
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

static inline bool zt30_staged_app_pid(pid_t pid, const char *executable,
                                      const char *launch_path, const char *prefix)
{
    const size_t n = strlen(prefix);
    if (strncmp(executable, prefix, n) != 0) return false;
    const char *tail = executable + n;
    if (strlen(tail) != 6U + strlen("/camera-app")) return false;
    for (size_t i = 0; i < 6U; ++i)
        if (!isalnum((unsigned char)tail[i])) return false;
    if (strcmp(tail + 6U, "/camera-app") != 0) return false;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char argument[4096];
    const size_t length = fread(argument, 1, sizeof(argument), file);
    fclose(file);
    const size_t expected = strlen(launch_path);
    return length > expected && argument[expected] == '\0' &&
           memcmp(argument, launch_path, expected) == 0;
}
