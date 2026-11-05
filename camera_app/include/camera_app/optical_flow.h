#pragma once
#include <cmath>
#include <cstdint>

// Optical flow on the centre of the luma. Image x is right and y is down.

constexpr unsigned CA_FLOW_MAX_SOURCE_WIDTH = 1920, CA_FLOW_MAX_SOURCE_HEIGHT = 1080;
constexpr double CA_FLOW_CROP = .6;

// Centre crop, sizes rounded down.
struct ca_flow_crop { unsigned x, y, width, height; };
constexpr ca_flow_crop ca_flow_crop_of(unsigned width, unsigned height)
{
    const unsigned w = unsigned(width * CA_FLOW_CROP), h = unsigned(height * CA_FLOW_CROP);
    return {(width - w) / 2, (height - h) / 2, w, h};
}
constexpr unsigned CA_FLOW_MAX_WIDTH = ca_flow_crop_of(CA_FLOW_MAX_SOURCE_WIDTH, CA_FLOW_MAX_SOURCE_HEIGHT).width;
constexpr unsigned CA_FLOW_MAX_HEIGHT = ca_flow_crop_of(CA_FLOW_MAX_SOURCE_WIDTH, CA_FLOW_MAX_SOURCE_HEIGHT).height;
// Flow over longer frame gaps is not sent.
constexpr float CA_FLOW_MAX_DT_S = .2f;

struct ca_flow_frame {
    unsigned width = 0, height = 0;
    unsigned source_width = 0, source_height = 0;
    uint64_t timestamp_us = 0;
    uint8_t pixels[CA_FLOW_MAX_WIDTH * CA_FLOW_MAX_HEIGHT] {};
};

enum ca_flow_result_status : uint8_t {
    CA_FLOW_OK = 0,
    CA_FLOW_NO_PREVIOUS,
    CA_FLOW_NO_CORNERS,
    CA_FLOW_NOT_TRACKED,
    CA_FLOW_ENGINE_ERROR,
};

struct ca_flow_result {
    ca_flow_result_status status = CA_FLOW_ENGINE_ERROR;
    float flow_x_px = 0, flow_y_px = 0;
    float dt_s = 0;
    uint16_t corners = 0, tracked = 0;
};

// Calls for one engine come from one thread; a failure resets it.
struct ca_flow_engine;
ca_flow_engine *ca_flow_engine_create();
void ca_flow_engine_destroy(ca_flow_engine *engine);
void ca_flow_engine_reset(ca_flow_engine *engine);
ca_flow_result ca_flow_engine_update(ca_flow_engine *engine, const ca_flow_frame &frame);

enum ca_flow_state : uint8_t {
    CA_FLOW_STATE_OFF = 0,
    CA_FLOW_STATE_RUNNING,
    CA_FLOW_STATE_NOT_DOWN,
    CA_FLOW_STATE_NO_FRAMES,
};

struct ca_flow_sample {
    uint64_t timestamp_us = 0;
    int16_t flow_x = 0, flow_y = 0;  // source pixels
    float rate_x = 0, rate_y = 0;  // rad/s
    uint8_t quality = 0;
};

inline bool ca_flow_hfov_valid(float hfov_deg)
{
    return std::isfinite(hfov_deg) && hfov_deg > 0 && hfov_deg < 180;
}
// Focal length in source pixels, or 0 if the inputs are invalid.
inline float ca_flow_focal_px(unsigned source_width, float hfov_deg)
{
    if (!source_width || !ca_flow_hfov_valid(hfov_deg)) return 0;
    return .5f * source_width / std::tan(hfov_deg * .00872664626f);
}
inline float ca_flow_rate(float flow_px, float focal_px, float dt_s)
{
    return std::atan2(flow_px, focal_px) / dt_s;
}

// Angle from straight down; gimbal pitch is up positive.
inline float ca_flow_tilt_rad(float pitch_rad)
{
    const float down = -std::sin(pitch_rad);
    return std::acos(down > 1 ? 1 : down < -1 ? -1 : down);
}
