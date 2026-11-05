#include "camera_app/APC_OpticalFlow.h"
#include "camera_app/APC_Media_Backend.h"
#include "camera_app/backend.h"
#include "camera_app/binlog.h"
#include "camera_app/log.h"
#include <cmath>
#include <climits>
#include <new>
#include <time.h>

static constexpr uint64_t ATTITUDE_TIMEOUT_MS = 1000;
static constexpr uint64_t NO_FRAMES_MS = 1000;
static constexpr unsigned FRAME_TIMEOUT_MS = 200;
static constexpr unsigned DISABLED_WAIT_MS = 1000, NOT_DOWN_WAIT_MS = 100;

static uint64_t monotonic_us()
{
    timespec t {}; clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}

// APC_Condition waits until a CLOCK_REALTIME deadline.
static timespec realtime_after_ms(unsigned ms)
{
    timespec until {}; clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += long(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    return until;
}

static int16_t truncate_px(float px)
{
    if (!(px > SHRT_MIN)) return SHRT_MIN;
    return px < SHRT_MAX ? int16_t(px) : SHRT_MAX;
}

bool APC_OpticalFlow::open(APC_Media_Backend *media, float hfov_deg)
{
    close();
    if (!media || !media->flow_available() || _lock.error() || _wake.error()) return false;
    if (!ca_flow_hfov_valid(hfov_deg)) {
        ca_log("optical flow unavailable: invalid horizontal FOV %.1f", double(hfov_deg));
        return false;
    }
    _engine = ca_flow_engine_create();
    _frame = new (std::nothrow) ca_flow_frame;
    if (!_engine || !_frame) { close(); return false; }
    _media = media; _hfov_deg = hfov_deg; _quit = false;
    const int error = pthread_create(&_thread, nullptr, [](void *p) -> void * {
        static_cast<APC_OpticalFlow *>(p)->run(); return nullptr;
    }, this);
    if (error) { close(); return false; }
    _running = true;
    return true;
}

void APC_OpticalFlow::close()
{
    if (_running) {
        { APC_LockGuard guard(_lock); _quit = true; _wake.signal(); }
        pthread_join(_thread, nullptr); _running = false;
    }
    ca_flow_engine_destroy(_engine); _engine = nullptr;
    delete _frame; _frame = nullptr;
    _media = nullptr;
    _state = CA_FLOW_STATE_OFF;
    APC_LockGuard guard(_lock);
    _queue_count = 0;
}

void APC_OpticalFlow::configure(const ca_optical_flow_config &config)
{
    APC_LockGuard guard(_lock);
    if (config.enabled == _config.enabled && config.max_tilt_deg == _config.max_tilt_deg &&
        config.gimbal_check == _config.gimbal_check) return;
    _config = config; ++_generation; _wake.signal();
}

void APC_OpticalFlow::update(ca_backend *gimbal)
{
    ca_gimbal_attitude attitude {};
    if (!gimbal || !ca_backend_gimbal_attitude(gimbal, &attitude) || !std::isfinite(attitude.pitch_rad)) return;
    APC_LockGuard guard(_lock);
    _have_attitude = true; _pitch_rad = attitude.pitch_rad; _attitude_ms = attitude.timestamp_ms;
}

bool APC_OpticalFlow::poll(ca_flow_sample &sample)
{
    APC_LockGuard guard(_lock);
    if (!_queue_count) return false;
    sample = _queue[_queue_head];
    _queue_head = (_queue_head + 1) % QUEUE; _queue_count--;
    return true;
}

void APC_OpticalFlow::publish(const ca_flow_sample &sample)
{
    APC_LockGuard guard(_lock);
    if (_queue_count == QUEUE) { _queue_head = (_queue_head + 1) % QUEUE; _queue_count--; }
    _queue[(_queue_head + _queue_count++) % QUEUE] = sample;
}

// Without a fresh gimbal attitude, flow is sent.
bool APC_OpticalFlow::facing_down(const ca_optical_flow_config &config, float &tilt_rad)
{
    tilt_rad = NAN;
    if (!config.gimbal_check) return true;
    APC_LockGuard guard(_lock);
    const uint64_t now_ms = monotonic_us() / 1000;
    if (!_have_attitude || now_ms > _attitude_ms + ATTITUDE_TIMEOUT_MS) return true;
    tilt_rad = ca_flow_tilt_rad(_pitch_rad);
    return tilt_rad <= config.max_tilt_deg * .01745329252f;
}

void APC_OpticalFlow::run()
{
    unsigned generation = ~0U;
    uint64_t last_frame_us = monotonic_us();
    for (;;) {
        _lock.lock();
        if (_quit) { _lock.unlock(); return; }
        const ca_optical_flow_config config = _config;
        const unsigned current = _generation;
        if (current != generation) {
            generation = current;
            ca_flow_engine_reset(_engine);
            last_frame_us = monotonic_us();
            _queue_count = 0;
        }
        if (!config.enabled) {
            _state = CA_FLOW_STATE_OFF;
            _wake.wait_until(_lock, realtime_after_ms(DISABLED_WAIT_MS));
            _lock.unlock();
            continue;
        }
        _lock.unlock();
        float tilt_rad;
        if (!facing_down(config, tilt_rad)) {
            ca_flow_engine_reset(_engine);
            _state = CA_FLOW_STATE_NOT_DOWN;
            APC_LockGuard guard(_lock);
            if (!_quit && current == _generation) _wake.wait_until(_lock, realtime_after_ms(NOT_DOWN_WAIT_MS));
            continue;
        }
        if (!_media->flow_frame(*_frame, FRAME_TIMEOUT_MS)) {
            if (monotonic_us() > last_frame_us + NO_FRAMES_MS * 1000) {
                ca_flow_engine_reset(_engine);
                _state = CA_FLOW_STATE_NO_FRAMES;
            }
            continue;
        }
        const uint64_t began = monotonic_us();
        last_frame_us = began;
        const ca_flow_result r = ca_flow_engine_update(_engine, *_frame);
        const uint64_t processed = monotonic_us();
        const float focal_px = ca_flow_focal_px(_frame->source_width, _hfov_deg);
        ca_flow_sample sample;
        const bool valid = r.status == CA_FLOW_OK && r.dt_s > 0 && r.dt_s < CA_FLOW_MAX_DT_S && focal_px > 0;
        if (valid) {
            sample.timestamp_us = _frame->timestamp_us;
            sample.flow_x = truncate_px(r.flow_x_px); sample.flow_y = truncate_px(r.flow_y_px);
            sample.rate_x = ca_flow_rate(r.flow_x_px, focal_px, r.dt_s);
            sample.rate_y = ca_flow_rate(r.flow_y_px, focal_px, r.dt_s);
            sample.quality = 255;
            publish(sample);
        }
        _state = CA_FLOW_STATE_RUNNING;
        constexpr float deg = 57.295779513f;
        CA_BINLOG(CA_LOG_FLOW, ca_log_flow, .frame_us = _frame->timestamp_us,
            .result = uint8_t(r.status), .sent = uint8_t(valid),
            .flow_x = r.flow_x_px, .flow_y = r.flow_y_px, .rate_x = sample.rate_x, .rate_y = sample.rate_y,
            .dt = r.dt_s, .tilt = tilt_rad * deg, .corners = r.corners, .tracked = r.tracked,
            .process_ms = (processed - began) * .001f);
    }
}
