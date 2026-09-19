#pragma once
#include "camera_app/APC_Media_Backend.h"
#include "apcam/APC_Camera.h"
#include "apcam/APC_Resource.h"

// Stable owner used by protocol callbacks while the underlying image pipeline
// changes. Diagnostic workers are joined before replacement and destruction.
class APC_Media {
public:
    explicit APC_Media(const ca_media_config &config) : _config(config),
        _inverted(config.settings.orientation == CA_MOUNT_INVERTED) {}
    ~APC_Media();
    APC_Media(const APC_Media &) = delete;
    APC_Media &operator=(const APC_Media &) = delete;
    int initialize();
    int configure(const struct ca_config *settings);
    const struct ca_config * settings() const;
    bool ready() const;
    int set_recording(bool active);
    bool recording() const;
    const char * recording_path() const;
    int set_zoom(float zoom);
    float zoom() const;
    int set_lens_zoom(enum ca_media_lens lens, float zoom);
    float lens_zoom(enum ca_media_lens lens) const;
    float hfov(bool thermal) const;
    unsigned frame_rate(bool thermal) const;
    int set_lens(enum ca_media_lens lens);
    enum ca_media_lens lens() const;
    int set_thermal_main(bool thermal_main);
    bool thermal_main() const;
    int autofocus(uint16_t x, uint16_t y);
    int manual_focus(int direction);
    int set_focus_percent(float percent);
    bool thermal_range(struct ca_thermal_range *range);
    int capture_photo(enum ca_photo_scope scope);
    bool cached_thermal_controls(uint8_t *gain, uint8_t *palette);
    int get_thermal_gain(uint8_t *gain);
    int set_thermal_gain(uint8_t gain);
    int get_thermal_palette(uint8_t *palette);
    int set_thermal_palette(uint8_t palette);
    int set_inverted(bool inverted);
private:
    struct LiveControls;
    int _open_backend(const ca_media_config *config);
    void _apply_overlay_after_control(const char *control);
    int _restore_controls(const LiveControls *state);
    void _start_controls_monitor();
    void _stop_controls_monitor();
    void _monitor_controls();
    void _monitor_exposure();
    const APC_Camera &_camera = APC_Camera::get_singleton();
    std::unique_ptr<APC_Media_Backend> _backend;
    ca_media_config _config {};
    bool _inverted = false;
    uint8_t _cached_gain = 0, _cached_palette = 0;
    bool _cached_thermal = false;
    APC_Mutex _controls_lock;
    APC_Condition _controls_wake, _exposure_wake;
    pthread_t _controls_thread {}, _exposure_thread {};
    bool _controls_running = false, _controls_stop = false, _exposure_running = false;
    unsigned _controls_generation = 0;
};
