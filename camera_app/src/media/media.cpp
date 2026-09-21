#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/APC_Media.h"
#include <new>
#include "camera_app/log.h"
#include "camera_app/binlog.h"
#include "camera_app/metadata.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include "apcam/lens.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>

// Compatibility handle retained by existing MAVLink/vendor callbacks.
struct ca_media final : public APC_Media {
    using APC_Media::APC_Media;
};


void APC_Media::_apply_overlay_after_control(const char *control)
{
    if (_backend->apply_overlay(&_config.settings) < 0) {
        ca_log("video overlay update failed after %s; %s",
               control, strerror(errno));
    }
}

/* Thermal USB transactions can take hundreds of milliseconds. Refresh their
 * diagnostic cache off the control loop, never holding the cache lock over I/O. */
void APC_Media::_monitor_controls()
{
    _controls_lock.lock();
    while (!_controls_stop) {
        unsigned generation=_controls_generation;
        _controls_lock.unlock();
        uint8_t gain,palette;
        bool valid=_backend->get_thermal_gain(&gain)==0 &&
            _backend->get_thermal_palette(&palette)==0;
        _controls_lock.lock();
        if (valid && generation==_controls_generation) {
            _cached_gain=gain; _cached_palette=palette;
            _cached_thermal=true;
        }
        if (!_controls_stop) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME,&until);
            until.tv_sec++;
            _controls_wake.wait_until(_controls_lock,until);
        }
    }
    _controls_lock.unlock();
    return;
}
/* Independent of slow thermal USB reads. The backend is joined before any
 * pipeline teardown, including reconfiguration and rollback. */
void APC_Media::_monitor_exposure()
{
    _controls_lock.lock();
    while (!_controls_stop) {
        _controls_lock.unlock();
        if (ca_binlog_active()) {
            for (unsigned lens=0; lens<_camera.num_lenses(); lens++) {
                struct ca_exposure sample=ca_exposure_empty(lens,ca_binlog_time_us());
                sample.result=_backend->exposure(lens,&sample);
                ca_binlog_emit(CA_LOG_AE,&sample,sizeof(sample));
            }
        }
        _controls_lock.lock();
        if (!_controls_stop) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME,&until);
            until.tv_nsec+=200000000;
            if (until.tv_nsec>=1000000000) { until.tv_sec++; until.tv_nsec-=1000000000; }
            _exposure_wake.wait_until(_controls_lock,until);
        }
    }
    _controls_lock.unlock();
    return;
}
void APC_Media::_stop_controls_monitor()
{
    if (!_controls_running && !_exposure_running) return;
    {
        APC_LockGuard guard(_controls_lock);
        _controls_stop=true;
        _controls_wake.signal();
        _exposure_wake.signal();
    }
    if (_controls_running) pthread_join(_controls_thread,NULL);
    if (_exposure_running) pthread_join(_exposure_thread,NULL);
    _exposure_running=false;
    _controls_running=false;
}
void APC_Media::_start_controls_monitor()
{
    if (!_backend || _controls_running || _exposure_running) return;
    _controls_stop=false;
    _cached_thermal=false;
    int error = pthread_create(&_exposure_thread, nullptr, [](void *opaque) -> void * {
        static_cast<APC_Media *>(opaque)->_monitor_exposure();
        return nullptr;
    }, this);
    if (error) ca_log("cannot start exposure monitor: %s",strerror(error));
    else _exposure_running=true;
    if (!_camera.has_thermal()) return;
    error = pthread_create(&_controls_thread, nullptr, [](void *opaque) -> void * {
        static_cast<APC_Media *>(opaque)->_monitor_controls();
        return nullptr;
    }, this);
    if (error) ca_log("cannot start thermal controls monitor: %s",strerror(error));
    else _controls_running=true;
}

int ca_media_open(struct ca_media **result, const struct ca_media_config *config)
{
    if (!result || !config) { errno = EINVAL; return -1; }
    auto *media = new (std::nothrow) ca_media(*config);
    if (!media) { errno = ENOMEM; return -1; }
    if (media->initialize() < 0) {
        const int saved = errno;
        delete media;
        errno = saved;
        return -1;
    }
    *result = media;
    return 0;
}

int APC_Media::initialize()
{
    if (_backend) { errno = EALREADY; return -1; }
    const int error = _controls_lock.error() ? _controls_lock.error() :
        _controls_wake.error() ? _controls_wake.error() : _exposure_wake.error();
    if (error) { errno = error; return -1; }
    if (_open_backend(&_config) < 0) return -1;
    if (_backend->apply_overlay(&_config.settings) < 0) {
        ca_log("initial video overlay could not be applied; camera remains available: %s",
               strerror(errno));
    }
    _start_controls_monitor();
    return 0;
}

int APC_Media::_open_backend(const ca_media_config *config)
{
    _backend = APC_Media_Backend::create(*config);
    return _backend ? 0 : -1;
}

const struct ca_config * APC_Media::settings() const
{
    return &_config.settings;
}

struct APC_Media::LiveControls {
    float zoom;
    float lens_zoom[2];
    enum ca_media_lens lens;
    bool thermal_main;
    int gain, palette;
};

int APC_Media::_restore_controls(const LiveControls *state)
{
    int result = 0;
    if (_backend->set_inverted(_inverted) < 0) result = -1;
    if (_camera.num_lenses() > 1 && _backend->set_lens(state->lens) < 0) result = -1;
    if (_camera.has_independent_lens_zoom()) {
        for (unsigned lens = 0; lens < 2; lens++) {
            if (isfinite(state->lens_zoom[lens]) &&
                _backend->set_lens_zoom((enum ca_media_lens)lens,
                                        state->lens_zoom[lens]) < 0) result = -1;
        }
    } else if (_camera.has_zoom() && isfinite(state->zoom) &&
               _backend->set_zoom(state->zoom) < 0) {
        result = -1;
    }
    if (_camera.has_thermal()) {
        if (_backend->set_thermal_main(state->thermal_main) < 0) result = -1;
        if (state->gain >= 0 && _backend->set_thermal_gain((uint8_t)state->gain) < 0) result = -1;
        if (state->palette >= 0 && _backend->set_thermal_palette((uint8_t)state->palette) < 0) result = -1;
    }
    return result;
}

int APC_Media::configure(const struct ca_config *settings)
{
    if (!settings) { errno = EINVAL; return -1; }
    const struct ca_config *old = &_config.settings;
    bool pipeline = !_backend || old->main_resolution != settings->main_resolution ||
        old->sub_resolution != settings->sub_resolution ||
        old->recording_resolution != settings->recording_resolution ||
        old->main_codec != settings->main_codec || old->sub_codec != settings->sub_codec;
    if (pipeline) {
        if (_backend && _backend->recording()) { errno = EBUSY; return -1; }
        LiveControls state = {};
        state.zoom = 1;
        state.lens_zoom[0] = state.lens_zoom[1] = 1;
        state.gain = -1;
        state.palette = -1;
        if (_backend) {
            state.zoom = _backend->zoom();
            if (_camera.has_independent_lens_zoom()) {
                for (unsigned lens = 0; lens < 2; lens++)
                    state.lens_zoom[lens] = _backend->lens_zoom((enum ca_media_lens)lens);
            }
            state.lens = _backend->lens();
            state.thermal_main = _backend->thermal_main();
            uint8_t value;
            if (_camera.has_thermal() && _backend->get_thermal_gain(&value) == 0) state.gain = value;
            if (_camera.has_thermal() && _backend->get_thermal_palette(&value) == 0) state.palette = value;
        }
        struct ca_media_config next = _config;
        next.settings = *settings;
        _stop_controls_monitor();
        _backend.reset();
        ca_log("reconfiguring media pipeline without restarting camera app");
        if (_open_backend(&next) < 0 || _restore_controls(&state) < 0 ||
            _backend->apply_overlay(settings) < 0) {
            int saved_errno = errno;
            _backend.reset();
            if (_open_backend(&_config) < 0 || _restore_controls(&state) < 0 ||
                _backend->apply_overlay(old) < 0)
                ca_log("media configuration rollback failed; retry configuration");
            _start_controls_monitor();
            errno = saved_errno ? saved_errno : EIO;
            return -1;
        }
        _start_controls_monitor();
        _config = next;
        return 0;
    }
    if (!ca_config_image_equal(old, settings)) {
        if (_backend->apply_image(settings) < 0) {
            int saved_errno = errno;
            if (_backend->apply_image(old) < 0)
                ca_log("image configuration rollback failed");
            errno = saved_errno;
            return -1;
        }
    }
    if (old->osd_cross != settings->osd_cross || old->osd_thermal_fov != settings->osd_thermal_fov ||
        old->osd_recording != settings->osd_recording) {
        if (_backend->apply_overlay(settings) < 0) {
            int saved = errno;
            (void)_backend->apply_overlay(old);
            if (!ca_config_image_equal(old, settings))
                (void)_backend->apply_image(old);
            errno = saved;
            return -1;
        }
    }
    if (_backend->configure_raw_thermal(settings) < 0) {
        int saved = errno;
        (void)_backend->apply_overlay(old);
        if (!ca_config_image_equal(old, settings)) (void)_backend->apply_image(old);
        errno = saved;
        return -1;
    }
    _config.settings = *settings;
    return 0;
}

APC_Media::~APC_Media()
{
    _stop_controls_monitor();
    if (recording()) (void)set_recording(false);
    _backend.reset();
}

/* The stable handle is retained by gimbal callbacks and the MAVLink server;
 * only the private implementation changes during pipeline reconfiguration. */
#define REQUIRE_IMPL do { if (!_backend) { errno = ENODEV; return -1; } } while (0)
bool APC_Media::ready() const
{ return _backend && _backend->ready(); }
int APC_Media::set_recording(bool active)
{
    REQUIRE_IMPL;
    bool previous=_backend->recording();
    struct ca_log_vid r={.time_us=ca_binlog_time_us(),.active=active};
    const char *path=_backend->recording_path();
    if (path) snprintf(r.path,sizeof(r.path),"%s",path);
    r.result=_backend->set_recording(active);
    int saved_errno=errno;
    if (active) {
        path=_backend->recording_path();
        if (path) snprintf(r.path,sizeof(r.path),"%s",path);
    }
    if (previous!=active || r.result<0) ca_binlog_emit(CA_LOG_VID,&r,sizeof(r));
    errno=saved_errno;
    return r.result;
}
bool APC_Media::recording() const
{ return _backend && _backend->recording(); }
const char * APC_Media::recording_path() const
{ return _backend ? _backend->recording_path() : NULL; }
int APC_Media::set_zoom(float zoom)
{
    REQUIRE_IMPL;
    int result=_backend->set_zoom(zoom);
    int saved=errno;
    _apply_overlay_after_control("zoom");
    if (result<0) { errno=saved; return result; }
    return 0;
}
float APC_Media::zoom() const
{ return _backend ? _backend->zoom() : 1; }
int APC_Media::set_lens_zoom(enum ca_media_lens lens, float zoom)
{
    REQUIRE_IMPL;
    if (_camera.has_independent_lens_zoom()) {
        int result = _backend->set_lens_zoom(lens, zoom);
        int saved = errno;
        _apply_overlay_after_control("lens zoom");
        errno = saved;
        return result;
    } else {
        if (lens != CA_MEDIA_LENS_WIDE) { errno = EINVAL; return -1; }
        return set_zoom(zoom);
    }
}
float APC_Media::lens_zoom(enum ca_media_lens lens) const
{
    if (_camera.has_independent_lens_zoom()) {
        return _backend ? _backend->lens_zoom(lens) : NAN;
    } else {
        return lens == CA_MEDIA_LENS_WIDE ? zoom() : NAN;
    }
}
float APC_Media::hfov(bool thermal) const
{ return _backend ? _backend->hfov(thermal) : NAN; }
unsigned APC_Media::frame_rate(bool thermal) const
{ return _backend ? _backend->frame_rate(thermal) : 0; }
int APC_Media::set_lens(enum ca_media_lens lens)
{
    REQUIRE_IMPL;
    int result=_backend->set_lens(lens);
    int saved=errno;
    _apply_overlay_after_control("lens change");
    if (result<0) { errno=saved; return result; }
    return 0;
}
enum ca_media_lens APC_Media::lens() const
{ return _backend ? _backend->lens() : CA_MEDIA_LENS_WIDE; }
int APC_Media::set_thermal_main(bool thermal_main)
{
    REQUIRE_IMPL;
    int result=_backend->set_thermal_main(thermal_main);
    int saved=errno;
    _apply_overlay_after_control("video source change");
    if (result<0) { errno=saved; return result; }
    return 0;
}
bool APC_Media::thermal_main() const
{ return _backend && _backend->thermal_main(); }
int APC_Media::autofocus(uint16_t x, uint16_t y)
{ REQUIRE_IMPL; return _backend->autofocus(x, y); }
int APC_Media::manual_focus(int direction)
{ REQUIRE_IMPL; return _backend->manual_focus(direction); }
int APC_Media::set_focus_percent(float percent)
{ REQUIRE_IMPL; return _backend->set_focus_percent(percent); }
bool APC_Media::thermal_range(struct ca_thermal_range *range)
{ return _backend && _backend->thermal_range(range) &&
         ca_thermal_range_fresh(range, ca_binlog_time_us()); }
int APC_Media::capture_photo(enum ca_photo_scope scope)
{
    REQUIRE_IMPL;
    struct ca_metadata snapshot;
    ca_metadata_snapshot(&snapshot);
    struct ca_log_cam r={};
    r.time_us = ca_binlog_time_us();
    r.scope = scope;
    r.lat = snapshot.lat_e7;
    r.lon = snapshot.lon_e7;
    r.alt = snapshot.alt_amsl_m;
    r.roll = snapshot.gimbal_roll_rad*57.295779513f;
    r.pitch = snapshot.gimbal_pitch_rad*57.295779513f;
    r.yaw = snapshot.gimbal_yaw_rad*57.295779513f;
    r.result=_backend->capture_photo(scope);
    int saved_errno=errno;
    ca_binlog_emit(CA_LOG_CAM,&r,sizeof(r));
    errno=saved_errno;
    return r.result;
}
bool APC_Media::cached_thermal_controls(uint8_t *gain, uint8_t *palette)
{
    APC_LockGuard guard(_controls_lock);
    bool valid=_cached_thermal;
    *gain=_cached_gain; *palette=_cached_palette;
    return valid;
}
int APC_Media::get_thermal_gain(uint8_t *gain)
{ REQUIRE_IMPL; int result=_backend->get_thermal_gain(gain);
  if (!result) {
      APC_LockGuard guard(_controls_lock);
      _cached_gain=*gain;
      _controls_generation++;
  }
  return result; }
int APC_Media::set_thermal_gain(uint8_t gain)
{ REQUIRE_IMPL; int result=_backend->set_thermal_gain(gain);
  if (!result) {
      APC_LockGuard guard(_controls_lock);
      _cached_gain=gain;
      _controls_generation++;
  }
  return result; }
int APC_Media::get_thermal_palette(uint8_t *palette)
{ REQUIRE_IMPL; int result=_backend->get_thermal_palette(palette);
  if (!result) {
      APC_LockGuard guard(_controls_lock);
      _cached_palette=*palette;
      _controls_generation++;
  }
  return result; }
int APC_Media::set_thermal_palette(uint8_t palette)
{ REQUIRE_IMPL; int result=_backend->set_thermal_palette(palette);
  if (!result) {
      APC_LockGuard guard(_controls_lock);
      _cached_palette=palette;
      _controls_generation++;
  }
  return result; }
int APC_Media::set_inverted(bool inverted)
{
    REQUIRE_IMPL;
    if (_backend->set_inverted(inverted) < 0) return -1;
    _inverted = inverted;
    return 0;
}

// Legacy protocol entry points; new services can use APC_Media directly.
int ca_media_configure(struct ca_media *media, const struct ca_config *settings)
{
    if (!media) { errno = EINVAL; return -1; }
    return media->configure(settings);
}
const struct ca_config * ca_media_settings(const struct ca_media *media)
{
    return media ? media->settings() : (nullptr);
}
bool ca_media_ready(const struct ca_media *media)
{
    return media ? media->ready() : (false);
}
int ca_media_set_recording(struct ca_media *media, bool active)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->set_recording(active);
}
bool ca_media_recording(const struct ca_media *media)
{
    return media ? media->recording() : (false);
}
const char * ca_media_recording_path(const struct ca_media *media)
{
    return media ? media->recording_path() : (nullptr);
}
int ca_media_set_zoom(struct ca_media *media, float zoom)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->set_zoom(zoom);
}
float ca_media_zoom(const struct ca_media *media)
{
    return media ? media->zoom() : (1);
}
int ca_media_set_lens_zoom(struct ca_media *media, enum ca_media_lens lens, float zoom)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->set_lens_zoom(lens, zoom);
}
float ca_media_lens_zoom(const struct ca_media *media, enum ca_media_lens lens)
{
    return media ? media->lens_zoom(lens) : (APC_Camera::get_singleton().has_independent_lens_zoom() ? NAN : (lens == CA_MEDIA_LENS_WIDE ? 1 : NAN));
}
float ca_media_hfov(const struct ca_media *media, bool thermal)
{
    return media ? media->hfov(thermal) : (NAN);
}
unsigned ca_media_frame_rate(const struct ca_media *media, bool thermal)
{
    return media ? media->frame_rate(thermal) : (0);
}
int ca_media_set_lens(struct ca_media *media, enum ca_media_lens lens)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->set_lens(lens);
}
enum ca_media_lens ca_media_lens(const struct ca_media *media)
{
    return media ? media->lens() : (CA_MEDIA_LENS_WIDE);
}
int ca_media_set_thermal_main(struct ca_media *media, bool thermal_main)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->set_thermal_main(thermal_main);
}
bool ca_media_thermal_main(const struct ca_media *media)
{
    return media ? media->thermal_main() : (false);
}
int ca_media_autofocus(struct ca_media *media, uint16_t x, uint16_t y)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->autofocus(x, y);
}
int ca_media_manual_focus(struct ca_media *media, int direction)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->manual_focus(direction);
}
int ca_media_set_focus_percent(struct ca_media *media, float percent)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->set_focus_percent(percent);
}
bool ca_media_thermal_range(struct ca_media *media, struct ca_thermal_range *range)
{
    return media ? media->thermal_range(range) : (false);
}
int ca_media_capture_photo(struct ca_media *media, enum ca_photo_scope scope)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->capture_photo(scope);
}
bool ca_media_cached_thermal_controls(struct ca_media *media, uint8_t *gain, uint8_t *palette)
{
    return media ? media->cached_thermal_controls(gain, palette) : (false);
}
int ca_media_get_thermal_gain(struct ca_media *media, uint8_t *gain)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->get_thermal_gain(gain);
}
int ca_media_set_thermal_gain(struct ca_media *media, uint8_t gain)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->set_thermal_gain(gain);
}
int ca_media_get_thermal_palette(struct ca_media *media, uint8_t *palette)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->get_thermal_palette(palette);
}
int ca_media_set_thermal_palette(struct ca_media *media, uint8_t palette)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->set_thermal_palette(palette);
}
int ca_media_set_inverted(struct ca_media *media, bool inverted)
{
    if (!media) { errno = ENODEV; return -1; }
    return media->set_inverted(inverted);
}
void ca_media_close(struct ca_media *media) { delete media; }
