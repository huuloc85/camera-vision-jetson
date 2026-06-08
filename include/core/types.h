// -*- coding: utf-8 -*-
// core/types.h — Shared types & data structures for jetson-inspect-v2
// Refactored from full_calb.h — all modules include this instead of each other
#pragma once

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <deque>

// ══════════════════════════════════════════════════════
// System State Machine
// ══════════════════════════════════════════════════════
enum class AppState {
    DISCONNECTED,   // HMI chưa kết nối / khởi động
    INITIALIZING,   // Camera + GPIO đang khởi tạo
    IDLE,           // Sẵn sàng nhận trigger
    BUSY,           // Đang chụp / xử lý ảnh
    RESULT_SHOWN,   // Vừa có kết quả, chờ trigger tiếp
    ALARM           // Lỗi camera / GPIO / timeout
};

inline const char* state_name(AppState s) {
    switch (s) {
        case AppState::DISCONNECTED:  return "DISCONNECTED";
        case AppState::INITIALIZING:  return "INITIALIZING";
        case AppState::IDLE:          return "IDLE";
        case AppState::BUSY:          return "BUSY";
        case AppState::RESULT_SHOWN:  return "RESULT_SHOWN";
        case AppState::ALARM:         return "ALARM";
        default:                      return "UNKNOWN";
    }
}

// ══════════════════════════════════════════════════════
// Product Inspection Result
// ══════════════════════════════════════════════════════
enum class ProductResult { OK, NG, WAIT };

// ══════════════════════════════════════════════════════
// Shape Metrics — output from ImageProcessor
// ══════════════════════════════════════════════════════
struct ShapeMetrics {
    double area            = 0;
    double perimeter       = 0;
    double solidity        = 0;
    int    width           = 0;
    int    height          = 0;
    int    top_points      = 0;
    int    top_width       = 0;
    double top_width_ratio = 0;
    double spike_ratio     = 0.0;
    int    spike_min_w     = 0;
    int    spike_max_w     = 0;
};

// ══════════════════════════════════════════════════════
// Inspection Result — passed between Vision and HMI
// ══════════════════════════════════════════════════════
struct InspectionResult {
    ProductResult result    = ProductResult::WAIT;
    std::string   label;          // "OK" / "NG" / "WAIT" / "READY"
    std::string   info_text;      // e.g. "OK len:12/H:45px r:0.15"
    ShapeMetrics  metrics;
    bool          has_metrics = false;
    cv::Mat       roi_vis;        // Full-res visual for HMI display
    cv::Mat       thresh;         // Threshold debug image
    double        cycle_ms   = 0; // Processing time in ms
};

// ══════════════════════════════════════════════════════
// Detection Parameters — tunable at runtime
// ══════════════════════════════════════════════════════
struct DetectionParams {
    double min_area        = 2500.0;
    double min_spike_ratio = 0.05;
    int    threshold       = 170;
    bool   show_thresh     = false;
    int    selected_param  = 0;    // 0=area, 1=spike, 2=threshold

    std::string adjust(bool increase);
    void reset();
    std::string summary() const;
};

// ══════════════════════════════════════════════════════
// Alarm / Error Info
// ══════════════════════════════════════════════════════
struct AlarmInfo {
    std::string code;     // "CAMERA_TIMEOUT", "GPIO_FAIL", etc.
    std::string message;
    double      timestamp = 0;
};
