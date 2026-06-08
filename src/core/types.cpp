// -*- coding: utf-8 -*-
// core/types.cpp — Implementation of DetectionParams methods
#include "core/types.h"
#include "core/config.h"
#include <cstdio>
#include <algorithm>
#include <string>

std::string DetectionParams::adjust(bool increase) {
    struct ParamDef {
        double* ptr_d; int* ptr_i;
        double delta; double min_val; bool is_int;
    };
    ParamDef defs[] = {
        {&min_area,        nullptr,    50,   50,   false},
        {&min_spike_ratio, nullptr,    0.01, 0.00, false},
        {nullptr,          &threshold, 5,    50,   true },
    };

    if (selected_param < 0 || selected_param >= 3) return "";
    auto& p = defs[selected_param];
    double d = increase ? p.delta : -p.delta;

    if (p.is_int) {
        *p.ptr_i = std::max((int)p.min_val, *p.ptr_i + (int)d);
    } else {
        *p.ptr_d = std::max(p.min_val, *p.ptr_d + d);
    }

    char buf[64];
    switch (selected_param) {
        case 0: snprintf(buf, sizeof(buf), "MIN_AREA = %.0f", min_area); break;
        case 1: snprintf(buf, sizeof(buf), "SPIKE_RATIO = %.2f", min_spike_ratio); break;
        case 2: snprintf(buf, sizeof(buf), "THRESHOLD = %d", threshold); break;
        default: buf[0] = 0;
    }
    return buf;
}

void DetectionParams::reset() {
    min_area        = DetectionConfig::MIN_AREA;
    min_spike_ratio = DetectionConfig::SPIKE_RATIO_THRESHOLD;
    threshold       = DetectionConfig::FIXED_THRESHOLD;
}

std::string DetectionParams::summary() const {
    char buf[128];
    snprintf(buf, sizeof(buf), "Area=%.0f, SpikeRatio=%.2f, Thresh=%d",
             min_area, min_spike_ratio, threshold);
    return buf;
}
