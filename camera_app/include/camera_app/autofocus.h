#ifndef CAMERA_APP_AUTOFOCUS_H
#define CAMERA_APP_AUTOFOCUS_H

#include <stdint.h>

typedef int (*ca_autofocus_move_fn)(void *opaque, int position);
typedef int (*ca_autofocus_measure_fn)(void *opaque, uint64_t *score);

struct ca_autofocus_ops {
    ca_autofocus_move_fn move;
    ca_autofocus_measure_fn measure;
    void *opaque;
};

struct ca_autofocus_result {
    int position;
    uint64_t score;
    unsigned samples;
};

int ca_autofocus_search(int minimum, int maximum, int initial,
                        unsigned coarse_step,
                        const struct ca_autofocus_ops *ops,
                        struct ca_autofocus_result *result);

#endif
