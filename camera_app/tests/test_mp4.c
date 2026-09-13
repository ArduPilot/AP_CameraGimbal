#include "camera_app/mp4.h"
#include "camera_app/video_metadata.h"
#include "camera_app/video_fov.h"
#include <assert.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

struct stream_file {
    int fd;
};

static int stream_write(void *opaque, const void *buffer, size_t length)
{
    struct stream_file *output = opaque;
    const uint8_t *data = buffer;

    while (length != 0U) {
        ssize_t written = write(output->fd, data, length);
        if (written <= 0) return -1;
        data += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const uint8_t standalone_sei[] = {
        0x00, 0x00, 0x00, 0x01, 0x06, 0xe5, 0x01, 0xfc, 0x80
    };
    struct stat st;
    struct ca_mp4 *writer = NULL;
    struct ca_fmp4 *fragmented = NULL;
    struct stream_file output = {.fd = -1};
    char fragmented_path[4096];
    if (argc != 3 || stat(argv[1], &st) < 0 || st.st_size <= 0) return 2;
    uint8_t *data = malloc((size_t)st.st_size);
    int fd = open(argv[1], O_RDONLY);
    if (data == NULL || fd < 0 || read(fd, data, (size_t)st.st_size) != st.st_size) return 3;
    close(fd);
    unlink(argv[2]);
    if (ca_mp4_open(&writer, argv[2], 320, 240, 10) < 0) return 4;
    if (snprintf(fragmented_path, sizeof(fragmented_path), "%s.fragmented",
                 argv[2]) >= (int)sizeof(fragmented_path)) return 5;
    unlink(fragmented_path);
    output.fd = open(fragmented_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output.fd < 0 || ca_fmp4_open(&fragmented, 320, 240, 10,
                                     stream_write, &output) < 0) return 6;
    size_t start = 0U, offset = 0U, nal, length;
    unsigned frames = 0U;
    const float hfov_deg = ca_video_hfov(CA_VISIBLE_HFOV_DEG, 2.0f);
    while (ca_annexb_next(data, (size_t)st.st_size, &offset, &nal, &length)) {
        if (length == 0U || (data[nal] & 31U) != 9U || nal <= 4U) continue;
        size_t end = nal - (data[nal - 4U] == 0U ? 4U : 3U);
        ca_metadata_set_position(-353632610 + (int32_t)frames, 1491652300,
                                  620.25f, 36.5f, 1.25f);
        ca_metadata_set_vehicle_attitude(0.1f, -0.2f, 0.3f);
        ca_metadata_set_gimbal_attitude(0.0f, -0.5f, 0.25f);
        ca_metadata_set_zoom(2.0f);
        if (ca_mp4_write_h264(writer, data + start, end - start, frames * 9000U, frames == 0U, hfov_deg) < 0 ||
            ca_fmp4_write_h264(fragmented, data + start, end - start, frames * 9000U, frames == 0U, hfov_deg) < 0) return 7;
        start = end;
        frames++;
        if (frames == 10U && getenv("CA_TEST_MP4_PAUSE") != NULL) {
            puts("READY active recording");
            fflush(stdout);
            if (getchar() == EOF) _exit(0);
        }
    }
    ca_metadata_set_position(-353632610 + (int32_t)frames, 1491652300,
                              620.25f, 36.5f, 1.25f);
    if (ca_mp4_write_h264(writer, data + start, (size_t)st.st_size - start, frames * 9000U, false, hfov_deg) < 0 ||
        ca_fmp4_write_h264(fragmented, data + start, (size_t)st.st_size - start, frames * 9000U, false, hfov_deg) < 0 ||
        ca_mp4_write_h264(writer, standalone_sei, sizeof(standalone_sei), 0, false, hfov_deg) < 0) return 8;
    if (getenv("CA_TEST_MP4_PAUSE") != NULL) {
        puts("READY unclosed recording");
        fflush(stdout);
        if (getchar() == EOF) _exit(0);
    }
    if (ca_mp4_close(writer) < 0 || ca_fmp4_close(fragmented) < 0 ||
        fsync(output.fd) < 0 || close(output.fd) < 0) return 8;
    assert(frames > 0U); /* fixture must contain per-frame AUDs */
    free(data);
    puts("PASS H.264 MP4 and fragmented-MP4 muxing");
    return 0;
}
