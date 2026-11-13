#pragma once
#include "camera_app/optical_flow.h"
#include "apcam/config.h"
#include "apcam/APC_Resource.h"
#include <atomic>
#include <pthread.h>
class APC_Media_Backend;
struct ca_backend;

// Stable owner, joined before the media backend is replaced. Image work runs
// on the worker; the main thread never waits for it.
class APC_OpticalFlow {
public:
    APC_OpticalFlow() = default;
    ~APC_OpticalFlow() { close(); }
    APC_OpticalFlow(const APC_OpticalFlow &) = delete;
    APC_OpticalFlow &operator=(const APC_OpticalFlow &) = delete;
    bool open(APC_Media_Backend *media, float hfov_deg);
    void close();
    bool available() const { return _running; }
    void configure(const ca_optical_flow_config &config);
    // Main thread.
    void update(ca_backend *gimbal);
    bool poll(ca_flow_sample &sample);
    ca_flow_state state() const { return _state; }
private:
    void run();
    bool facing_down(const ca_optical_flow_config &config, float &tilt_rad);
    void publish(const ca_flow_sample &sample);
    APC_Media_Backend *_media = nullptr;
    ca_flow_engine *_engine = nullptr;
    ca_flow_frame *_frame = nullptr;
    float _hfov_deg = 0;
    APC_Mutex _lock;
    APC_Condition _wake;
    pthread_t _thread {};
    bool _running = false, _quit = false;
    unsigned _generation = 0;
    ca_optical_flow_config _config {};
    std::atomic<ca_flow_state> _state {CA_FLOW_STATE_OFF};
    bool _have_attitude = false;
    float _pitch_rad = 0;
    uint64_t _attitude_ms = 0;
    static constexpr unsigned QUEUE = 8;
    ca_flow_sample _queue[QUEUE] {};
    unsigned _queue_head = 0, _queue_count = 0;
};
