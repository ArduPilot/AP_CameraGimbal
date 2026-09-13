#define _GNU_SOURCE
#include "../src/backends/z1mini/native.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_bool stopped;
static unsigned count;
static void frame(void *unused, const uint8_t *data, size_t length, uint64_t pts, bool key, unsigned stream)
{
    (void)unused;
    assert(length == 5 && !memcmp(data, "\0\0\0\1\x65", 5));
    assert(pts == 1234567 && key);
    assert(stream == count);
    count++;
    if (count == 2) atomic_store(&stopped, true);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "fd:3")) {
        const char *mode = getenv("Z1_NATIVE_TEST_MODE");
        struct ca_z1_native_header h = {CA_Z1_NATIVE_MAGIC, 5, 1234567, 1, 0};
        if (!strcmp(mode, "stream")) h.stream = 2;
        if (!strcmp(mode, "oversized")) h.size = CA_Z1_NATIVE_MAX_FRAME + 1U;
        size_t size = !strcmp(mode, "truncated") ? 7 : sizeof(h);
        /* Exercise socket fragmentation rather than assuming full reads. */
        for (size_t i = 0; i < size; i++) {
            if (write(3, (char *)&h + i, 1) != 1) return 0;
        }
        if (!strcmp(mode, "valid")) {
            if (write(3, "\0\0\0\1\x65", 5) != 5) return 1;
            h.stream = 1;
            if (write(3, &h, sizeof(h)) != sizeof(h) ||
                write(3, "\0\0\0\1\x65", 5) != 5) return 1;
            pause(); /* parent must terminate and reap us after callback stop */
        }
        return 0;
    }
    const char *modes[] = {"valid", "oversized", "truncated", "stream"};
    for (unsigned i = 0; i < 4; i++) {
        setenv("Z1_NATIVE_TEST_MODE", modes[i], 1);
        atomic_store(&stopped, false);
        count = 0;
        int result = ca_z1_native_receive(argv[0], &stopped, frame, NULL);
        assert(result == (i == 0 ? 0 : -1));
        assert(count == (i == 0 ? 2U : 0U));
        assert(waitpid(-1, NULL, WNOHANG) == -1); /* no leaked helper children */
    }
    puts("Z1 native IPC: two fragmented streams, malformed size/stream, truncated header and child cleanup passed");
    return 0;
}
