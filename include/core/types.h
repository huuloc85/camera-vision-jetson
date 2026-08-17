// -*- coding: utf-8 -*-
// core/types.h — Shared types & data structures for jetson-inspect-v2
// Refactored from full_calb.h — all modules include this instead of each other
#pragma once

#include <opencv2/opencv.hpp>
#include <array>
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
    cv::Mat       roi_vis;        // Visual frame published to the HMI
    cv::Mat       thresh;         // Threshold debug image
    std::vector<cv::Point> frame_contour; // Contour in roi_vis coordinates
    bool          roi_view     = false; // Frame/contour already use rotated ROI coordinates
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
// ROI Parameters — four corners in 1920×1080 reference coordinates
// P1=top-left, P2=top-right, P3=bottom-right, P4=bottom-left
// ══════════════════════════════════════════════════════
struct RoiParams {
    static constexpr int REF_WIDTH  = 1920;
    static constexpr int REF_HEIGHT = 1080;

    std::array<cv::Point2f, 4> points{{
        {604.0f, 421.0f}, {1327.0f, 403.0f},
        {1338.0f, 943.0f}, {595.0f, 973.0f}
    }};
    int selected_edge = 0; // 0=top, 1=right, 2=bottom, 3=left

    void reset();
    bool valid() const;
};

// ══════════════════════════════════════════════════════
// Alarm / Error Info
// ══════════════════════════════════════════════════════
struct AlarmInfo {
    std::string code;     // "CAMERA_TIMEOUT", "GPIO_FAIL", etc.
    std::string message;
    double      timestamp = 0;
};
