// -*- coding: utf-8 -*-
// vision/vision_service.h — Core Vision Service
// Camera + GPIO + pipeline. Không vẽ UI, không biết OpenCV window.
// Giao tiếp với HMI qua shared InspectionResult (single-process)
// hoặc có thể mở rộng sang local TCP sau này.
#pragma once

#include "core/types.h"
#include "core/config.h"
#include "vision/camera.h"
#include "vision/image_processor.h"
#include "vision/classifier.h"
#include "vision/detection_state.h"
#include "gpio/gpio_controller.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>

// ══════════════════════════════════════════════════════
// VisionService — All vision + GPIO, no UI
// ══════════════════════════════════════════════════════
class VisionService {
public:
    VisionService();
    virtual ~VisionService();

    // Start background threads and camera
    bool start();
    void stop();

    // ─── Main step — called by App main loop ──────────
    // Returns result (WAIT = nothing detected yet)
    InspectionResult process_trigger(double trigger_time = 0.0);
    InspectionResult process_frame_for_calibration();
    InspectionResult process_frame_for_roi_calibration();
    bool set_roi_rectangle_from_frame(const cv::Point2f& first,
                                      const cv::Point2f& second,
                                      const cv::Size& frame_size);
    std::array<cv::Point2f, 4> roi_points_for_frame(const cv::Size& frame_size) const;
    void save_roi_settings();
    void reset_roi_settings();

    // ─── Accessors (thread-safe reads) ────────────────
    bool has_pending_trigger() const { return gpio.has_pending_trigger(); }
    double consume_trigger()         { return gpio.consume_trigger(); }
    AppState current_state() const   { return state.app_state; }
    void heartbeat();
    bool should_render_waiting_frame(double now);
    const cv::Mat& last_result_frame() const { return last_result_frame_; }
    void request_auto_restart();

    // ─── State & params (HMI writes, vision reads) ────
    DetectionState state;
    GPIOController gpio;

    std::atomic<double> capture_fps_{0};
    std::atomic<double> capture_ms_{0};
    std::atomic<bool>   running_{true};

protected:
    LibcameraCapture camera_;
    ImageProcessor   processor_;
    ProductClassifier classifier_;

    std::timed_mutex cam_lock_;
    cv::Mat last_result_frame_;
    double  last_render_time_    = 0;
    double  last_trigger_time_   = 0;
    std::atomic<double> last_trigger_activity_{0};

    std::atomic<double> watchdog_heartbeat_;
    std::atomic<bool>   camera_recovering_{false};
    std::atomic<bool>   camera_sleeping_{false};
    std::atomic<bool>   realtime_capture_enabled_{false};
    std::atomic<bool>   realtime_capture_request_{false};
    std::atomic<double> realtime_capture_request_time_{0.0};
    std::mutex          realtime_frame_mutex_;
    std::condition_variable realtime_frame_cv_;
    cv::Mat             realtime_frame_;
    bool                realtime_frame_ready_ = false;
    double              realtime_frame_time_ = 0.0;
    std::atomic<bool>   cleanup_done_{false};
    std::atomic<bool>   restart_in_progress_{false};
    std::thread watchdog_thread_;
    std::thread camera_keepalive_thread_;

    // ─── Camera lifecycle ──────────────────────────────
    cv::Mat safe_capture(bool flush = true, bool require_signal = true,
                         double not_before = 0.0);
    cv::Mat request_realtime_frame(double not_before = 0.0);
    bool    restart_camera();
    void    sleep_camera();
    bool    wake_camera();
    void    auto_restart();

    // ─── Background threads ────────────────────────────
    void start_watchdog();
    void start_camera_keepalive();

    // ─── Frame processing internals ───────────────────
    struct FullProcessResult {
        cv::Mat       roi_vis;
        ShapeMetrics  metrics;
        bool          has_metrics = false;
        ProductResult result;
        std::string   info_text;
        cv::Mat       thresh;
    };
    FullProcessResult process_frame_internal(const cv::Mat& frame, bool fast = false);

    struct FastDetectResult {
        ProductResult result;
        std::string   info_text;
        std::vector<cv::Point> contour;
        std::vector<cv::Point> frame_contour;
        ShapeMetrics  metrics;
        bool          has_metrics = false;
        cv::Mat       det_roi;
    };
    FastDetectResult detect_only(const cv::Mat& frame);

    void force_quit();
};
