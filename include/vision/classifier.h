// -*- coding: utf-8 -*-
// vision/classifier.h — Product OK/NG classification
#pragma once

#include "core/types.h"
#include <vector>
#include <opencv2/opencv.hpp>

class ProductClassifier {
public:
    explicit ProductClassifier(const DetectionParams& params) : params_(params) {}

    // Returns (result, info_text)
    std::pair<ProductResult, std::string> classify(
        const std::vector<cv::Point>& contour,
        const ShapeMetrics& metrics) const;

private:
    const DetectionParams& params_;
};
