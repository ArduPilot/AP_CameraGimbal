#!/usr/bin/env python3
"""Native settings must preserve legacy INI values and reject unsupported writes."""
from pathlib import Path
import subprocess
import tempfile

WEB = Path(__file__).resolve().parents[1]
SOURCE = r'''
#define main camera_web_main
#include "mt11-web.cpp"
#undef main
#include <cassert>
#include <string>
#include "camera_app/camera_definition.h"

static const char *initial =
    "# Preserve this legacy format and unknown key\n"
    "[general]\ntimezone=Australia/Canberra\nfuture_key=kept\n"
    "[stream.main]\nresolution=3840x2160\ncodec=h265\n"
    "[capture]\nphoto_scope=all\n"
    "[thermal]\nstream_fps=5\n";

static std::string contents(const char *path)
{
    size_t n; char *p = read_file(path, MAX_CONFIG, &n);
    if (!p) return {};
    std::string result(p, n); free(p); return result;
}

static void denied(const std::string &candidate)
{
    char error[512] = {};
    auto before = contents(REPLACEMENT_CONFIG_PATH);
    auto backup = contents(REPLACEMENT_CONFIG_BACKUP_PATH);
    assert(!save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                        candidate.data(), candidate.size(), error, sizeof(error)));
    assert(error[0]);
    assert(contents(REPLACEMENT_CONFIG_PATH) == before);
    assert(contents(REPLACEMENT_CONFIG_BACKUP_PATH) == backup);
}

int main()
{
    char error[512] = {};
    FILE *f = fopen(REPLACEMENT_CONFIG_PATH, "w"); assert(f); fputs(initial, f); fclose(f);
    ca_config loaded; ca_config_defaults(&loaded);
    assert(ca_config_load(&loaded, REPLACEMENT_CONFIG_PATH, error, sizeof(error)) == 0);
    assert(loaded.main_resolution == CA_VIDEO_2160P && loaded.main_codec == CA_VIDEO_H265);
    assert(APC_ZT30_Config::effective(loaded));
    assert(loaded.main_resolution == CA_VIDEO_720P && loaded.main_codec == CA_VIDEO_H264);
    assert(contents(REPLACEMENT_CONFIG_PATH) == initial); // startup never rewrites INI

    for (const auto &p : replacement_parameters) {
        if (parameter_shown(&p)) assert(APC_Config::find(p.section, p.key));
        if (!strcmp(p.form_name, "main_resolution") || !strcmp(p.form_name, "main_codec") ||
            !strcmp(p.form_name, "autorecord") || !strcmp(p.form_name, "osd_cross") ||
            !strcmp(p.form_name, "orientation") || !strcmp(p.form_name, "position_targeting") ||
            !strcmp(p.form_name, "network_primary_address")) assert(!parameter_shown(&p));
        if (!strcmp(p.form_name, "timezone") || !strcmp(p.form_name, "photo_scope") ||
            !strcmp(p.form_name, "raw_stream_fps")) assert(parameter_shown(&p));
    }
    for (const auto &p : replacement_parameters) {
        if (strcmp(p.form_name, "main_codec")) continue;
        APC_HTTPRequest request;
        char body[] = "main_codec=h265";
        request.body = body; request.body_len = strlen(body);
        ini_update updates[2]; size_t count = 0;
        assert(!collect_described_parameters(&request, &p, 1, updates, 2, &count, error, sizeof(error)));
        assert(count == 0 && strstr(error, "not configurable"));
    }
    denied(std::string(initial) + "[stream.sub]\ncodec=h265\n");
    denied(std::string(initial) + "[recording]\nautorecord=true\n");
    denied(std::string(initial) + "[network]\nprimary_address=192.168.1.99/24\n");
    denied(std::string(initial) + "[mount]\norientation=inverted\n");
    denied(std::string(initial) + "[thermal]\nstream_fps=6\n"); // duplicate
    denied(std::string(initial) + "[mavlink]\nsystem_id=invalid\n");
    denied(std::string(initial) + "[image]\nbrightness=invalid\n");
    auto nul = std::string(initial); nul.push_back(0); nul += "extra"; denied(nul);

    std::string supported = initial;
    supported.replace(supported.find("stream_fps=5"), 12, "stream_fps=6");
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       supported.data(), supported.size(), error, sizeof(error)));
    assert(contents(REPLACEMENT_CONFIG_PATH) == supported);
    assert(contents(REPLACEMENT_CONFIG_BACKUP_PATH) == initial);
    assert(supported.find("future_key=kept") != std::string::npos);

    ca_config_defaults(&loaded);
    assert(ca_config_load(&loaded, REPLACEMENT_CONFIG_PATH, error, sizeof(error)) == 0);
    for (const char *name : {"VIDEO_MAIN_RES", "VIDEO_MAIN_CODEC", "REC_AUTOSTART",
                             "OSD_CROSS", "MOUNT_ORIENT", "MAV_POS_TARGET"}) {
        int index = ca_config_param_find(name); assert(index >= 0);
        auto previous = loaded;
        auto saved = contents(REPLACEMENT_CONFIG_PATH);
        assert(ca_config_param_assign(&loaded, index, 1) < 0 && errno == ENOTSUP);
        assert(!memcmp(&loaded, &previous, sizeof(loaded)));
        assert(ca_config_param_save(&loaded, REPLACEMENT_CONFIG_PATH, index, 1) < 0 && errno == ENOTSUP);
        assert(contents(REPLACEMENT_CONFIG_PATH) == saved);
    }
    int scope = ca_config_param_find("PHOTO_SCOPE"); assert(scope >= 0);
    assert(ca_config_param_save(&loaded, REPLACEMENT_CONFIG_PATH, scope, CA_PHOTO_SCOPE_THERMAL) == 0);
    assert(loaded.photo_scope == CA_PHOTO_SCOPE_THERMAL);
    int rate = ca_config_param_find("RAW_STREAM_FPS"); assert(rate >= 0);
    assert(ca_config_param_save(&loaded, REPLACEMENT_CONFIG_PATH, rate, 7) == 0);
    assert(loaded.raw_stream_fps == 7);
    for (const char *name : {"OSD_CROSS", "TRACK_METHOD", "MAV_POS_TARGET", "REC_AUTOSTART",
                            "VIDEO_MAIN_RES", "VIDEO_MAIN_CODEC"}) assert(ca_camera_param_find(name) < 0);
    for (const char *name : {"PHOTO_SCOPE", "RAW_STREAM_FPS", "LOG_DISARMED"}) {
        int index = ca_camera_param_find(name); assert(index >= 0);
        ca_camera_parameter parameter;
        assert(ca_camera_param_info(index, &parameter));
        assert(parameter.config_index >= 0);
    }
    // Editing legacy values back to the supported mode must be possible.
    const char *clean = "[stream.main]\nresolution=1280x720\ncodec=h264\n";
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       clean, strlen(clean), error, sizeof(error)));
    // Repair corrupt current configuration, including missing files.
    f = fopen(REPLACEMENT_CONFIG_PATH, "w"); assert(f);
    fputs("[stream.main]\ncodec=bogus\n", f); fclose(f);
    denied("[stream.main]\ncodec=h265\n");
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       clean, strlen(clean), error, sizeof(error)));
    assert(unlink(REPLACEMENT_CONFIG_PATH) == 0);
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       clean, strlen(clean), error, sizeof(error)));
    // A proxy toggle must not materialize old proxy network settings on this
    // platform, whose network addresses are OS-owned.
    const char *legacy = "[support_proxy]\nenabled=false\nhost=localhost\nnetwork_address=192.168.9.2/24\n";
    f = fopen(REPLACEMENT_CONFIG_PATH, "w"); assert(f); fputs(legacy, f); fclose(f);
    std::string enabled = legacy; enabled.replace(enabled.find("false"), 5, "true");
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       enabled.data(), enabled.size(), error, sizeof(error)));
    puts("PASS native ZT30 setting visibility, strict INI saves, startup normalization and numeric write policy");
}
'''
with tempfile.TemporaryDirectory(prefix='zt30-parameters-') as directory:
    root = Path(directory)
    source = root / 'test.cpp'; source.write_text(SOURCE)
    binary = root / 'test'
    subprocess.run(['c++', '-std=gnu++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-missing-field-initializers',
                    '-DMT11_WEB_TEST', '-DCAMERA_APP_ZT30_DEV', '-DAPCAM_TARGET=APCAM_TARGET_ZT30',
                    f'-I{WEB}', f'-I{WEB.parent / "include"}', f'-I{WEB.parent / "camera_app/include"}',
                    f'-DAPP_REQUEST_PATH="{root / "request"}"',
                    f'-DAPP_REQUEST_LOCK_PATH="{root / "request.lock"}"',
                    f'-DAPP_STARTED_PATH="{root / "started"}"',
                    f'-DAPP_DIR="{root}"', f'-DREPLACEMENT_CONFIG_PATH="{root / "camera.ini"}"',
                    f'-DREPLACEMENT_CONFIG_BACKUP_PATH="{root / "camera.ini.web.bak"}"',
                    str(source), str(WEB.parent / 'camera_app/src/config.cpp'),
                    str(WEB.parent / 'camera_app/src/protocol/camera_definition.cpp'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True)
