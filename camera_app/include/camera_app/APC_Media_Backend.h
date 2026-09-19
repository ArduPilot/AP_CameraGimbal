#pragma once
#include "camera_app/media.h"
#include "camera_app/exposure.h"
#include <memory>
#include <cerrno>
#include <cmath>

// Image pipeline only. Gimbal transport/control remains a separate backend.
// A backend owns its SDK, capture workers and stream/recording resources. The
// frontend joins diagnostic readers before destroying or replacing it.
class APC_Media_Backend {
public:
    virtual ~APC_Media_Backend() = default;
    APC_Media_Backend(const APC_Media_Backend &) = delete;
    APC_Media_Backend &operator=(const APC_Media_Backend &) = delete;
    static std::unique_ptr<APC_Media_Backend> create(const ca_media_config &config);
    virtual bool ready() const = 0;
    virtual int set_recording(bool active) = 0;
    virtual bool recording() const = 0;
    virtual const char * recording_path() const = 0;
    virtual int set_zoom(float zoom) = 0;
    virtual float zoom() const = 0;
    virtual int set_lens_zoom(enum ca_media_lens lens, float zoom) { if (lens != CA_MEDIA_LENS_WIDE) { errno = EINVAL; return -1; } return set_zoom(zoom); }
    virtual float lens_zoom(enum ca_media_lens lens) const { return lens == CA_MEDIA_LENS_WIDE ? zoom() : NAN; }
    virtual float hfov(bool thermal) const = 0;
    virtual unsigned frame_rate(bool thermal) const = 0;
    virtual int set_lens(enum ca_media_lens lens) = 0;
    virtual enum ca_media_lens lens() const = 0;
    virtual int set_thermal_main(bool thermal_main) = 0;
    virtual bool thermal_main() const = 0;
    virtual int autofocus(uint16_t x, uint16_t y) = 0;
    virtual int manual_focus(int direction) = 0;
    virtual int set_focus_percent(float percent) = 0;
    virtual bool thermal_range(struct ca_thermal_range *range) = 0;
    virtual int capture_photo(enum ca_photo_scope scope) = 0;
    virtual int get_thermal_gain(uint8_t *gain) = 0;
    virtual int set_thermal_gain(uint8_t gain) = 0;
    virtual int get_thermal_palette(uint8_t *palette) = 0;
    virtual int set_thermal_palette(uint8_t palette) = 0;
    virtual int set_inverted(bool inverted) = 0;
    virtual int exposure(unsigned lens, struct ca_exposure *sample) = 0;
    virtual int apply_overlay(const struct ca_config *settings) = 0;
    virtual int apply_image(const struct ca_config *settings) = 0;
protected:
    APC_Media_Backend() = default;
};
