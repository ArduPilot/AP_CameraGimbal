/* Raspberry Pi libcamera -> V4L2 M2M H.264 pipeline (Pi 4 / CM4 encoder).
 *
 * libcamera fills YUV420 dmabufs which are queued, without copying, to the
 * bcm2835 encoder's OUTPUT queue. A frame's request is requeued to the camera
 * once the encoder releases its buffer; any pending image controls are
 * attached to that request. The encoder thread delivers Annex B access units
 * from the CAPTURE queue. Sequence headers repeat on every IDR frame so RTSP
 * clients can join at any key frame.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pipeline.h"
#include "camera_app/log.h"

#include <libcamera/libcamera.h>
#include <libcamera/control_ids.h>
#include <libcamera/version.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include "apcam/atomic.h"
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <memory>
#include <new>
#include <vector>

using namespace libcamera;

#define RPI_ENCODER_DEVICE "/dev/video11"
#define RPI_CAMERA_BUFFERS 6U
#define RPI_ENCODED_BUFFERS 12U
#define RPI_ENCODED_BUFFER_SIZE (512U * 1024U)

/* Requires libcamera 0.2 or later. 0.5 replaced the RPi convention of zero
 * meaning automatic with explicit exposure/gain modes. */
#define RPI_LIBCAMERA_AT_LEAST(major, minor) \
    (LIBCAMERA_VERSION_MAJOR > (major) || \
     (LIBCAMERA_VERSION_MAJOR == (major) && LIBCAMERA_VERSION_MINOR >= (minor)))

struct ca_rpi_pipeline {
    struct ca_rpi_pipeline_config config;
    std::unique_ptr<CameraManager> manager;
    std::shared_ptr<Camera> camera;
    std::unique_ptr<CameraConfiguration> camera_config;
    std::unique_ptr<FrameBufferAllocator> allocator;
    Stream *stream = nullptr;
    std::vector<std::unique_ptr<Request>> requests;
    bool acquired = false, camera_started = false;

    int encoder = -1;
    void *encoded[RPI_ENCODED_BUFFERS] = {};
    size_t encoded_length[RPI_ENCODED_BUFFERS] = {};
    bool encoder_started = false;

    pthread_t thread {};
    bool thread_started = false;
    atomic_bool stop;

    pthread_mutex_t lock;
    ControlList pending {controls::controls};
    bool have_pending = false;
    struct ca_rpi_image_controls image {};
};

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;
    do result = ioctl(fd, request, arg); while (result < 0 && errno == EINTR);
    return result;
}

static uint64_t monotonic_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000U + (uint64_t)t.tv_nsec / 1000U;
}

static void image_control_list(const struct ca_config *settings, ControlList &list,
                               struct ca_rpi_image_controls *mapped)
{
    struct ca_rpi_image_controls c;
    ca_rpi_image_controls(settings, &c);
    list.set(controls::Brightness, c.brightness);
    list.set(controls::Contrast, c.contrast);
    list.set(controls::Saturation, c.saturation);
    list.set(controls::ExposureValue, c.exposure_value);
#if RPI_LIBCAMERA_AT_LEAST(0, 5)
    list.set(controls::ExposureTimeMode, c.exposure_us > 0 ? controls::ExposureTimeModeManual
                                                           : controls::ExposureTimeModeAuto);
    list.set(controls::AnalogueGainMode, c.analogue_gain > 0.0f ? controls::AnalogueGainModeManual
                                                                : controls::AnalogueGainModeAuto);
    if (c.exposure_us > 0) list.set(controls::ExposureTime, c.exposure_us);
    if (c.analogue_gain > 0.0f) list.set(controls::AnalogueGain, c.analogue_gain);
#else
    /* The Raspberry Pi IPA treats zero as "return to automatic". */
    list.set(controls::ExposureTime, c.exposure_us);
    list.set(controls::AnalogueGain, c.analogue_gain);
#endif
    switch (settings->metering) {
    case CA_METERING_CENTER: list.set(controls::AeMeteringMode, controls::MeteringCentreWeighted); break;
    case CA_METERING_SPOT: list.set(controls::AeMeteringMode, controls::MeteringSpot); break;
    default: list.set(controls::AeMeteringMode, controls::MeteringMatrix); break;
    }
    switch (settings->white_balance) {
    case CA_WB_DAYLIGHT: list.set(controls::AwbMode, controls::AwbDaylight); break;
    case CA_WB_CLOUDY: list.set(controls::AwbMode, controls::AwbCloudy); break;
    case CA_WB_FLUORESCENT: list.set(controls::AwbMode, controls::AwbFluorescent); break;
    case CA_WB_INCANDESCENT: list.set(controls::AwbMode, controls::AwbIncandescent); break;
    default: list.set(controls::AwbMode, controls::AwbAuto); break;
    }
    if (mapped) *mapped = c;
}

static void report_exposure(struct ca_rpi_pipeline *p, const ControlList &metadata)
{
    if (!p->config.exposure) return;
    struct ca_exposure s = ca_exposure_empty(0, monotonic_us());
    const auto shutter = metadata.get(controls::ExposureTime);
    const auto again = metadata.get(controls::AnalogueGain);
    const auto dgain = metadata.get(controls::DigitalGain);
    if (shutter) { s.shutter_us = (float)*shutter; s.valid |= CA_AE_SHUTTER; }
    if (again) { s.analog_gain = *again; s.valid |= CA_AE_AGAIN; }
    if (dgain) { s.digital_gain = *dgain; s.valid |= CA_AE_DGAIN; }
    pthread_mutex_lock(&p->lock);
    s.mode = (uint8_t)p->image.mode;
    pthread_mutex_unlock(&p->lock);
    s.valid |= CA_AE_MODE;
    p->config.exposure(p->config.opaque, &s);
}

static void requeue(struct ca_rpi_pipeline *p, unsigned index);

/* libcamera's thread: hand the completed frame to the encoder. */
static void request_complete(struct ca_rpi_pipeline *p, Request *request)
{
    if (request->status() == Request::RequestCancelled || atomic_load(&p->stop)) return;
    report_exposure(p, request->metadata());
    FrameBuffer *buffer = request->findBuffer(p->stream);
    if (!buffer) return;
    const auto sensor_ns = request->metadata().get(controls::SensorTimestamp);
    const uint64_t timestamp_us = sensor_ns ? (uint64_t)*sensor_ns / 1000U : monotonic_us();

    struct v4l2_plane plane = {};
    struct v4l2_buffer v = {};
    v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    v.memory = V4L2_MEMORY_DMABUF;
    v.index = (unsigned)request->cookie();
    v.field = V4L2_FIELD_NONE;
    v.length = 1;
    v.m.planes = &plane;
    v.timestamp.tv_sec = (time_t)(timestamp_us / 1000000U);
    v.timestamp.tv_usec = (suseconds_t)(timestamp_us % 1000000U);
    /* All YUV420 planes share one dmabuf at the plane offsets. Pass its full
     * extent: the encoder may expect height padding beyond frameSize. */
    const auto planes = buffer->planes();
    plane.m.fd = planes[0].fd.get();
    plane.bytesused = planes.back().offset + planes.back().length;
    plane.length = plane.bytesused;
    if (xioctl(p->encoder, VIDIOC_QBUF, &v) < 0) {
        ca_log("RPi encoder input queue failed: %s", strerror(errno));
        /* Drop the frame but return its buffer, or capture stalls. */
        requeue(p, v.index);
    }
}

static void requeue(struct ca_rpi_pipeline *p, unsigned index)
{
    if (index >= p->requests.size() || atomic_load(&p->stop)) return;
    Request *request = p->requests[index].get();
    request->reuse(Request::ReuseBuffers);
    pthread_mutex_lock(&p->lock);
    if (p->have_pending) {
        /* Controls persist in the IPA, so one request carries each change. */
        request->controls().merge(p->pending);
        p->pending.clear();
        p->have_pending = false;
    }
    pthread_mutex_unlock(&p->lock);
    if (p->camera->queueRequest(request) < 0 && !atomic_load(&p->stop))
        ca_log("RPi camera request queue failed");
}

static void *encoder_thread(void *opaque)
{
    auto *p = static_cast<struct ca_rpi_pipeline *>(opaque);
    while (!atomic_load(&p->stop)) {
        struct pollfd item = {.fd = p->encoder, .events = POLLIN | POLLOUT};
        int ready = poll(&item, 1, 100);
        if (ready < 0 && errno != EINTR) { ca_log("RPi encoder poll failed: %s", strerror(errno)); break; }
        if (ready <= 0) continue;
        for (;;) {
            struct v4l2_plane plane = {};
            struct v4l2_buffer v = {};
            v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            v.memory = V4L2_MEMORY_DMABUF;
            v.length = 1;
            v.m.planes = &plane;
            if (xioctl(p->encoder, VIDIOC_DQBUF, &v) < 0) break;
            requeue(p, v.index);
        }
        for (;;) {
            struct v4l2_plane plane = {};
            struct v4l2_buffer v = {};
            v.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            v.memory = V4L2_MEMORY_MMAP;
            v.length = 1;
            v.m.planes = &plane;
            if (xioctl(p->encoder, VIDIOC_DQBUF, &v) < 0) break;
            if (v.index < RPI_ENCODED_BUFFERS && plane.bytesused && p->config.frame) {
                const uint64_t pts = (uint64_t)v.timestamp.tv_sec * 1000000U + (uint64_t)v.timestamp.tv_usec;
                p->config.frame(p->config.opaque, static_cast<const uint8_t *>(p->encoded[v.index]),
                                plane.bytesused, pts, (v.flags & V4L2_BUF_FLAG_KEYFRAME) != 0);
            }
            plane.bytesused = 0;
            if (xioctl(p->encoder, VIDIOC_QBUF, &v) < 0) {
                ca_log("RPi encoder output requeue failed: %s", strerror(errno));
                break;
            }
        }
    }
    return NULL;
}

static int set_encoder_control(int fd, uint32_t id, int32_t value, const char *name)
{
    struct v4l2_control control = {.id = id, .value = value};
    if (xioctl(fd, VIDIOC_S_CTRL, &control) < 0) {
        ca_log("RPi encoder %s=%d failed: %s", name, value, strerror(errno));
        return -1;
    }
    return 0;
}

static int open_encoder(struct ca_rpi_pipeline *p, const StreamConfiguration &stream)
{
    const char *device = getenv("CAMERA_APP_RPI_ENCODER");
    p->encoder = open(device && *device ? device : RPI_ENCODER_DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (p->encoder < 0) {
        ca_log("RPi cannot open H.264 encoder: %s", strerror(errno));
        return -1;
    }
    const int fd = p->encoder;
    if (set_encoder_control(fd, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)p->config.bit_rate_kbps * 1000, "bitrate") < 0 ||
        set_encoder_control(fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, "profile") < 0 ||
        set_encoder_control(fd, V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_1, "level") < 0 ||
        set_encoder_control(fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, (int32_t)p->config.frame_rate, "I period") < 0 ||
        set_encoder_control(fd, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1, "repeat headers") < 0)
        return -1;

    struct v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    format.fmt.pix_mp.width = stream.size.width;
    format.fmt.pix_mp.height = stream.size.height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    format.fmt.pix_mp.field = V4L2_FIELD_ANY;
    format.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].bytesperline = stream.stride;
    if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) {
        ca_log("RPi encoder input format failed: %s", strerror(errno));
        return -1;
    }
    format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = stream.size.width;
    format.fmt.pix_mp.height = stream.size.height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    format.fmt.pix_mp.field = V4L2_FIELD_ANY;
    format.fmt.pix_mp.colorspace = V4L2_COLORSPACE_DEFAULT;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].sizeimage = RPI_ENCODED_BUFFER_SIZE;
    if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) {
        ca_log("RPi encoder output format failed: %s", strerror(errno));
        return -1;
    }
    struct v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = p->config.frame_rate;
    (void)xioctl(fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers buffers = {};
    buffers.count = (unsigned)p->requests.size();
    buffers.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buffers.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(fd, VIDIOC_REQBUFS, &buffers) < 0 || buffers.count < p->requests.size()) {
        ca_log("RPi encoder input buffers unavailable: %s", strerror(errno));
        return -1;
    }
    buffers = {};
    buffers.count = RPI_ENCODED_BUFFERS;
    buffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffers.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &buffers) < 0 || buffers.count < RPI_ENCODED_BUFFERS) {
        ca_log("RPi encoder output buffers unavailable: %s", strerror(errno));
        return -1;
    }
    for (unsigned i = 0; i < RPI_ENCODED_BUFFERS; i++) {
        struct v4l2_plane plane = {};
        struct v4l2_buffer v = {};
        v.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        v.memory = V4L2_MEMORY_MMAP;
        v.index = i;
        v.length = 1;
        v.m.planes = &plane;
        if (xioctl(fd, VIDIOC_QUERYBUF, &v) < 0) return -1;
        void *data = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
        if (data == MAP_FAILED) return -1;
        p->encoded[i] = data;
        p->encoded_length[i] = plane.length;
        if (xioctl(fd, VIDIOC_QBUF, &v) < 0) return -1;
    }
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) return -1;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) return -1;
    p->encoder_started = true;
    return 0;
}

static int open_camera(struct ca_rpi_pipeline *p)
{
    p->manager = std::make_unique<CameraManager>();
    int result = p->manager->start();
    if (result < 0) { ca_log("RPi libcamera start failed: %s", strerror(-result)); errno = -result; return -1; }
    const char *id = getenv("CAMERA_APP_RPI_CAMERA");
    if (id && *id) p->camera = p->manager->get(id);
    else if (!p->manager->cameras().empty()) p->camera = p->manager->cameras()[0];
    if (!p->camera) { ca_log("RPi no libcamera camera found"); errno = ENODEV; return -1; }
    if (p->camera->acquire() < 0) { ca_log("RPi camera %s is busy", p->camera->id().c_str()); errno = EBUSY; return -1; }
    p->acquired = true;

    p->camera_config = p->camera->generateConfiguration({StreamRole::VideoRecording});
    if (!p->camera_config || p->camera_config->empty()) { errno = ENOTSUP; return -1; }
    StreamConfiguration &stream = p->camera_config->at(0);
    stream.pixelFormat = formats::YUV420;
    stream.size = Size(p->config.width, p->config.height);
    stream.bufferCount = RPI_CAMERA_BUFFERS;
    stream.colorSpace = ColorSpace::Rec709;
    p->camera_config->orientation = p->config.inverted ? Orientation::Rotate180 : Orientation::Rotate0;
    if (p->camera_config->validate() == CameraConfiguration::Invalid) {
        ca_log("RPi camera configuration invalid"); errno = EINVAL; return -1;
    }
    if (stream.size != Size(p->config.width, p->config.height) || stream.pixelFormat != formats::YUV420) {
        ca_log("RPi camera cannot provide %ux%u YUV420 (got %s)", p->config.width, p->config.height,
               stream.toString().c_str());
        errno = ENOTSUP; return -1;
    }
    if (p->camera->configure(p->camera_config.get()) < 0) { ca_log("RPi camera configure failed"); errno = EIO; return -1; }
    p->stream = stream.stream();
    ca_log("RPi camera %s configured %s", p->camera->id().c_str(), stream.toString().c_str());

    p->allocator = std::make_unique<FrameBufferAllocator>(p->camera);
    if (p->allocator->allocate(p->stream) < 0) { errno = ENOMEM; return -1; }
    const auto &buffers = p->allocator->buffers(p->stream);
    for (unsigned i = 0; i < buffers.size(); i++) {
        std::unique_ptr<Request> request = p->camera->createRequest(i);
        if (!request || request->addBuffer(p->stream, buffers[i].get()) < 0) { errno = ENOMEM; return -1; }
        p->requests.push_back(std::move(request));
    }
    return 0;
}

static int start(struct ca_rpi_pipeline *p)
{
    if (open_camera(p) < 0 || open_encoder(p, p->camera_config->at(0)) < 0) return -1;
    p->camera->requestCompleted.connect(p, [p](Request *request) { request_complete(p, request); });
    ControlList initial(controls::controls);
    const int64_t frame_us = 1000000 / (int64_t)p->config.frame_rate;
    initial.set(controls::FrameDurationLimits, Span<const int64_t, 2>({frame_us, frame_us}));
    image_control_list(&p->config.image, initial, &p->image);
    if (p->camera->start(&initial) < 0) { ca_log("RPi camera start failed"); errno = EIO; return -1; }
    p->camera_started = true;
    int error = pthread_create(&p->thread, NULL, encoder_thread, p);
    if (error) { errno = error; return -1; }
    p->thread_started = true;
    for (auto &request : p->requests) {
        if (p->camera->queueRequest(request.get()) < 0) { errno = EIO; return -1; }
    }
    return 0;
}

int ca_rpi_pipeline_open(struct ca_rpi_pipeline **out, const struct ca_rpi_pipeline_config *config)
{
    if (!out || !config || !config->width || !config->height || !config->frame_rate) { errno = EINVAL; return -1; }
    auto *p = new (std::nothrow) ca_rpi_pipeline();
    if (!p) { errno = ENOMEM; return -1; }
    p->config = *config;
    atomic_init(&p->stop, false);
    pthread_mutex_init(&p->lock, NULL);
    if (start(p) < 0) {
        int saved = errno ? errno : EIO;
        ca_rpi_pipeline_close(p);
        errno = saved;
        return -1;
    }
    *out = p;
    return 0;
}

int ca_rpi_pipeline_set_image(struct ca_rpi_pipeline *p, const struct ca_config *settings)
{
    if (!p || !settings) { errno = EINVAL; return -1; }
    ControlList list(controls::controls);
    struct ca_rpi_image_controls mapped;
    image_control_list(settings, list, &mapped);
    pthread_mutex_lock(&p->lock);
    /* Each list holds every image control, so the newest replaces any change
     * not yet queued; merge() would keep the older values. */
    p->pending = std::move(list);
    p->have_pending = true;
    p->image = mapped;
    pthread_mutex_unlock(&p->lock);
    return 0;
}

void ca_rpi_pipeline_close(struct ca_rpi_pipeline *p)
{
    if (!p) return;
    atomic_store(&p->stop, true);
    if (p->thread_started) pthread_join(p->thread, NULL);
    if (p->camera_started) (void)p->camera->stop();
    if (p->camera) p->camera->requestCompleted.disconnect(p);
    if (p->encoder >= 0) {
        if (p->encoder_started) {
            int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            (void)xioctl(p->encoder, VIDIOC_STREAMOFF, &type);
            type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            (void)xioctl(p->encoder, VIDIOC_STREAMOFF, &type);
        }
        for (unsigned i = 0; i < RPI_ENCODED_BUFFERS; i++)
            if (p->encoded[i]) munmap(p->encoded[i], p->encoded_length[i]);
        close(p->encoder);
    }
    p->requests.clear();
    if (p->allocator && p->stream) (void)p->allocator->free(p->stream);
    p->allocator.reset();
    p->camera_config.reset();
    if (p->acquired) (void)p->camera->release();
    p->camera.reset();
    if (p->manager) p->manager->stop();
    p->manager.reset();
    pthread_mutex_destroy(&p->lock);
    delete p;
}
