// Lucas-Kanade optical flow. Only this translation unit enables exceptions and
// includes OpenCV.
#include "camera_app/optical_flow.h"
#include <new>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

struct ca_flow_engine {
    cv::Mat previous, current;
    uint64_t previous_us = 0;
    bool have_previous = false;
    std::vector<cv::Point2f> corners, tracked;
    std::vector<uint8_t> status;
    std::vector<float> error;
};

static constexpr int MAX_CORNERS = 50;
static constexpr double CORNER_QUALITY = .2, CORNER_DISTANCE = 10;
static constexpr int CORNER_BLOCK = 5;
// Fewer tracked than this retries with a smaller window and pyramid.
static constexpr int MIN_TRACKED = 5;
static constexpr double SCALE = .5;

// One thread: video and MAVLink keep the other cores.
ca_flow_engine *ca_flow_engine_create()
{
    try {
        cv::setNumThreads(1);
    } catch (...) {
        return nullptr;
    }
    return new (std::nothrow) ca_flow_engine;
}

void ca_flow_engine_destroy(ca_flow_engine *engine)
{
    delete engine;
}

void ca_flow_engine_reset(ca_flow_engine *engine)
{
    if (engine) engine->have_previous = false;
}

static ca_flow_result update(ca_flow_engine &e, const ca_flow_frame &frame)
{
    ca_flow_result r;
    const cv::Mat crop(int(frame.height), int(frame.width), CV_8UC1, const_cast<uint8_t *>(frame.pixels));
    cv::resize(crop, e.current, cv::Size(), SCALE, SCALE, cv::INTER_LINEAR);
    const uint64_t previous_us = e.previous_us;
    const bool have_previous = e.have_previous && e.previous.size() == e.current.size();
    // Every outcome keeps this frame for the next update.
    auto keep = [&]() { cv::swap(e.previous, e.current); e.previous_us = frame.timestamp_us; e.have_previous = true; };
    if (!have_previous) {
        keep();
        r.status = CA_FLOW_NO_PREVIOUS;
        return r;
    }
    r.dt_s = float(int64_t(frame.timestamp_us - previous_us) * 1e-6);
    cv::goodFeaturesToTrack(e.previous, e.corners, MAX_CORNERS, CORNER_QUALITY, CORNER_DISTANCE,
                            cv::noArray(), CORNER_BLOCK);
    r.corners = uint16_t(e.corners.size());
    if (e.corners.empty()) {
        keep();
        r.status = CA_FLOW_NO_CORNERS;
        return r;
    }
    const cv::TermCriteria criteria(cv::TermCriteria::EPS | cv::TermCriteria::COUNT, 20, .03);
    cv::calcOpticalFlowPyrLK(e.previous, e.current, e.corners, e.tracked, e.status, e.error,
                             cv::Size(15, 15), 2, criteria);
    if (cv::countNonZero(e.status) < MIN_TRACKED)
        cv::calcOpticalFlowPyrLK(e.previous, e.current, e.corners, e.tracked, e.status, e.error,
                                 cv::Size(12, 12), 1);
    keep();
    // Mean motion weighted by inverse tracking error.
    double sum_x = 0, sum_y = 0, sum_weight = 0;
    for (size_t i = 0; i < e.corners.size(); i++) {
        if (!e.status[i]) continue;
        const double weight = 1. / (double(e.error[i]) + 1e-6);
        sum_x += weight * (e.tracked[i].x - e.corners[i].x);
        sum_y += weight * (e.tracked[i].y - e.corners[i].y);
        sum_weight += weight;
        r.tracked++;
    }
    if (!r.tracked || !(sum_weight > 0)) {
        r.status = CA_FLOW_NOT_TRACKED;
        return r;
    }
    r.flow_x_px = float(sum_x / sum_weight / SCALE);
    r.flow_y_px = float(sum_y / sum_weight / SCALE);
    r.status = CA_FLOW_OK;
    return r;
}

ca_flow_result ca_flow_engine_update(ca_flow_engine *engine, const ca_flow_frame &frame)
{
    if (!engine || frame.width < 2 || frame.height < 2 ||
        frame.width > CA_FLOW_MAX_WIDTH || frame.height > CA_FLOW_MAX_HEIGHT) {
        ca_flow_engine_reset(engine);
        return {};
    }
    try {
        return update(*engine, frame);
    } catch (...) {
        ca_flow_engine_reset(engine);
        return {};
    }
}
