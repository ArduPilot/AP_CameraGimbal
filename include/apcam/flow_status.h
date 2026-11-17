#pragma once

#include <string.h>

#include "survey_runtime.h"

inline bool apcam_flow_status_path(const char *config, char *path, size_t size,
                                   const char *directory = nullptr)
{
    char survey[4096];
    if (!apcam_survey_status_path(config, survey, sizeof(survey), directory)) return false;
    const char *name = strrchr(survey, '/') + 1;
    const int n = snprintf(path, size, "%.*sapcam-flow-%s", int(name - survey), survey,
                           name + strlen("apcam-survey-"));
    return n >= 0 && size_t(n) < size;
}
