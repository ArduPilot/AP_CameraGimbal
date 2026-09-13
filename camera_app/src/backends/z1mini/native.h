#ifndef CA_Z1_NATIVE_H
#define CA_Z1_NATIVE_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Experimental local ARM-native helper wire format, followed by size bytes of
 * one Annex-B access unit. PTS is microseconds. Data uses child fd 3; stdout and
 * stderr remain diagnostic logs. Stream 0 is live 1080p, stream 1 recording 4K.
 * The helper exclusively owns the AX pipeline. */
#define CA_Z1_NATIVE_MAGIC UINT32_C(0x34363248)
#define CA_Z1_NATIVE_MAX_FRAME (8U * 1024U * 1024U)
struct ca_z1_native_header {
    uint32_t magic, size;
    uint64_t pts;
    uint32_t key, stream;
};
_Static_assert(sizeof(struct ca_z1_native_header) == 24, "native frame ABI");
typedef void (*ca_z1_native_frame_fn)(void *, const uint8_t *, size_t, uint64_t, bool, unsigned);
typedef int (*ca_z1_native_run_fn)(const atomic_bool *, ca_z1_native_frame_fn, void *);
int ca_z1_native_receive(const char *helper, const atomic_bool *stop,
                         ca_z1_native_frame_fn publish, void *opaque);
struct ca_media;
bool ca_z1_media_ready(const struct ca_media *media);
#endif
