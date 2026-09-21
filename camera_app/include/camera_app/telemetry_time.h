/*
 * Minimum-delay clock mapping adapted from ArduPilot AP_RTC/JitterCorrection.
 * Copyright ArduPilot contributors. SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <math.h>
#include <stdint.h>

/* Zero-initialise each instance. Use a separate clock per telemetry stream:
 * their transport delays differ, and some senders use different boot epochs.
 * This estimates variable delay, not the unknown minimum one-way latency. */
struct ca_telemetry_clock {
    int64_t offset_us, minimum_us;
    uint64_t last_raw, remote_ticks, received_us;
    unsigned count;
    bool initialised;

    bool sample(uint64_t raw, uint32_t tick_us, uint64_t now_us,
                uint64_t &sample_ms, bool &reset)
    {
        reset = false;
        // Zero is also used by legacy senders with no usable timestamp.
        if (raw == 0 && !initialised) {
            sample_ms = now_us / 1000;
            return true;
        }
        if (raw > uint64_t(INT64_MAX) / tick_us) return false;
        int64_t delta = initialised ? int64_t(raw) - int64_t(last_raw) : 0;
        // Handles 32-bit milliseconds and ArduPilot's uint32 micros() carried
        // in the nominally 64-bit AUTOPILOT_STATE time_boot_us field.
        if (initialised && raw <= UINT32_MAX && last_raw <= UINT32_MAX) {
            if (delta < -INT64_C(0x80000000)) delta += INT64_C(0x100000000);
            else if (delta > INT64_C(0x80000000)) delta -= INT64_C(0x100000000);
        }
        if (initialised && delta <= 0) {
            // Ignore duplicates/reordering without refreshing sample age.
            // A backwards clock after a stream outage indicates a restart.
            if (delta >= -int64_t(1000000 / tick_us) ||
                now_us - received_us <= 1000000) return false;
            *this = {};
            reset = true;
        }
        remote_ticks = initialised ? remote_ticks + delta : raw;
        last_raw = raw;
        received_us = now_us;
        const int64_t remote_us = int64_t(remote_ticks * tick_us);
        const int64_t diff = int64_t(now_us) - remote_us;
        if (!initialised || diff < offset_us) offset_us = diff;
        initialised = true;
        int64_t estimate = remote_us + offset_us;
        // The same 500 ms maximum lag and 100-sample drift window as AP_RTC.
        if (estimate + 500000 < int64_t(now_us)) {
            estimate = int64_t(now_us) - 500000;
            offset_us = estimate - remote_us;
        }
        if (count == 0 || diff < minimum_us) minimum_us = diff;
        if (++count == 100) {
            offset_us = minimum_us;
            count = 0;
        }
        sample_ms = uint64_t(estimate > 0 ? estimate : 0) / 1000;
        return true;
    }
};

/* Vehicle yaw in the camera's monotonic clock. Rates are Euler yaw rates.
 * Keep enough samples to bracket cached gimbal feedback even at 100 Hz. */
struct ca_yaw_history {
    static constexpr unsigned prediction_ms = 250;
    struct sample { uint64_t ms; float yaw, rate; } samples[128];
    unsigned count, next;

    void add(uint64_t ms, float yaw, float rate)
    {
        if (count && ms <= samples[(next + 127) % 128].ms) count = next = 0;
        samples[next] = {ms, yaw, rate};
        next = (next + 1) % 128;
        if (count < 128) count++;
    }

    bool at(uint64_t ms, float &yaw, float &rate) const
    {
        if (!count) return false;
        const sample &last = samples[(next + 127) % 128];
        if (ms >= last.ms) {
            if (ms - last.ms > prediction_ms) return false;
            rate = last.rate;
            yaw = remainderf(last.yaw + rate * ((ms - last.ms) * .001f), 2 * float(M_PI));
            return true;
        }
        const sample *previous = &samples[(next + 128 - count) % 128];
        if (ms < previous->ms) return false;
        for (unsigned i = 1; i < count; i++) {
            const sample &s = samples[(next + 128 - count + i) % 128];
            if (ms <= s.ms) {
                if (s.ms - previous->ms > prediction_ms) return false;
                float fraction = float(ms - previous->ms) / (s.ms - previous->ms);
                yaw = remainderf(previous->yaw + fraction * remainderf(s.yaw - previous->yaw,
                                  2 * float(M_PI)), 2 * float(M_PI));
                rate = previous->rate + fraction * (s.rate - previous->rate);
                return true;
            }
            previous = &s;
        }
        return false;
    }
};
