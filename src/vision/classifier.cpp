// -*- coding: utf-8 -*-
// vision/classifier.cpp — Product OK/NG classification logic
#include "vision/classifier.h"
#include "core/config.h"
#include <cstdio>

std::pair<ProductResult, std::string> ProductClassifier::classify(
    const std::vector<cv::Point>& /*contour*/,
    const ShapeMetrics& metrics) const
{
    char buf[128];

    if (metrics.area <= params_.min_area) {
        snprintf(buf, sizeof(buf), "Area:%.0f", metrics.area);
        return {ProductResult::WAIT, buf};
    }
    if (metrics.height < DetectionConfig::MIN_HEIGHT) {
        snprintf(buf, sizeof(buf), "Low H:%dpx", metrics.height);
        return {ProductResult::WAIT, buf};
    }
    if (metrics.solidity < DetectionConfig::MIN_SOLIDITY) {
        snprintf(buf, sizeof(buf), "Light? Sol:%.2f", metrics.solidity);
        return {ProductResult::WAIT, buf};
    }

    double sr = metrics.spike_ratio;
    if (sr > params_.min_spike_ratio) {
        snprintf(buf, sizeof(buf), "SPIKE len:%d/H:%dpx r:%.2f",
                 metrics.spike_min_w, metrics.spike_max_w, sr);
        return {ProductResult::NG, buf};
    }

    snprintf(buf, sizeof(buf), "OK len:%d/H:%dpx r:%.2f",
             metrics.spike_min_w, metrics.spike_max_w, sr);
    return {ProductResult::OK, buf};
}
