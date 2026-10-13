#pragma once
#include "APC_Config.h"

// The native pipeline currently owns fixed video channels. Preserve saved
// legacy values, but do not accept changes to controls it cannot apply.
class APC_ZT30_Config {
public:
    static bool editable(const APC_Config::Field &field)
    {
        const char *s = field.section, *k = field.key;
        if (!strcmp(s, "general") || !strcmp(s, "logging") || !strcmp(s, "capture")) return true;
        if (!strcmp(s, "stream.main") || !strcmp(s, "stream.sub")) return !strcmp(k, "alias");
        if (!strcmp(s, "mavlink")) return strcmp(k, "position_targeting") && strcmp(k, "tracking_method");
        if (!strcmp(s, "thermal")) return !strcmp(k, "stream_fps");
        if (!strcmp(s, "network")) return !strcmp(k, "capture");
        if (!strcmp(s, "support_proxy")) return strncmp(k, "network_", 8) != 0;
        return false;
    }

    static bool validate_change(const ca_config &before, const ca_config &after,
                                char *error, size_t error_size)
    {
        ca_config supported;
        ca_config_defaults(&supported);
        effective(supported);
        for (size_t i = 0; i < APC_Config::count(); i++) {
            const auto &field = APC_Config::fields()[i];
            if (editable(field) || !memcmp((const char *)&before + field.offset,
                                          (const char *)&after + field.offset, field.size) ||
                !memcmp((const char *)&supported + field.offset, (const char *)&after + field.offset, field.size)) continue;
            snprintf(error, error_size, "[%s] %s is not configurable on the native ZT30 build",
                     field.section, field.key);
            errno = ENOTSUP;
            return false;
        }
        return true;
    }

    // Runtime normalization is deliberately separate from parsing/validation:
    // the web editor must compare the original saved values, not clamped ones.
    static bool effective(ca_config &config)
    {
        bool changed = config.main_resolution != CA_VIDEO_720P ||
            config.sub_resolution != CA_VIDEO_720P || config.recording_resolution != CA_VIDEO_720P ||
            config.main_codec != CA_VIDEO_H264 || config.sub_codec != CA_VIDEO_H264 ||
            config.position_targeting || config.osd_recording || config.osd_cross ||
            config.orientation != CA_MOUNT_AUTO || config.autorecord != CA_AUTORECORD_DISABLED;
        config.main_resolution = config.sub_resolution = config.recording_resolution = CA_VIDEO_720P;
        config.main_codec = config.sub_codec = CA_VIDEO_H264;
        config.position_targeting = config.osd_recording = config.osd_cross = false;
        config.orientation = CA_MOUNT_AUTO;
        config.autorecord = CA_AUTORECORD_DISABLED;
        return changed;
    }
};
