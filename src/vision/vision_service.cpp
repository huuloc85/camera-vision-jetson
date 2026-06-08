// -*- coding: utf-8 -*-
// vision/vision_service.cpp — Core Vision Service (camera + GPIO + pipeline)
// No UI code, no OpenCV windows. Interacts with HMI via InspectionResult struct.
#include "vision/vision_service.h"
#include "core/logger.h"
#include "core/time_utils.h"

#include <chrono>
#include <thread>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

extern char** environ;

using core::now_sec;
using core::sleep_sec;

struct ScopedBusySignal {
    explicit ScopedBusySignal(GPIOController& gpio) : gpio_(gpio) {
        gpio_.set_busy(true);
    }
    ~ScopedBusySignal() {
        gpio_.set_busy(false);
    }
    GPIOController& gpio_;
};

// ══════════════════════════════════════════════════════
// VisionService — Constructor / Destructor
// ══════════════════════════════════════════════════════
VisionService::VisionService()
    : processor_(state.params)
    , classifier_(state.params)
{
    cv::setNumThreads(1);

    state.transition(AppState::INITIALIZING);

    if (!camera_.start()) {
        log_msg(LOG_CRITICAL, "Camera init failed!");
        state.transition(AppState::ALARM);
    } else {
        sleep_sec(1.0);
        state.transition(AppState::IDLE);
    }

    watchdog_heartbeat_.store(now_sec());
    last_capture_time_    = now_sec();
    last_trigger_activity_ = now_sec();

    start_watchdog();
    start_camera_keepalive();
}

VisionService::~VisionService() { stop(); }

bool VisionService::start() { return camera_.is_running(); }

void VisionService::stop() {
    running_ = false;
    if (watchdog_thread_.joinable()) watchdog_thread_.join();
    if (camera_keepalive_thread_.joinable()) camera_keepalive_thread_.join();
    gpio.cleanup();
    camera_.close();
}

void VisionService::heartbeat() {
    watchdog_heartbeat_.store(now_sec());
}

bool VisionService::should_render_waiting_frame(double now) {
    if (now - last_render_time_ < DetectionConfig::WAITING_RENDER_INTERVAL) {
        return false;
    }
    last_render_time_ = now;
    return true;
}

void VisionService::request_auto_restart() {
    auto_restart();
}

// ══════════════════════════════════════════════════════
// Frame capture helpers
// ══════════════════════════════════════════════════════
cv::Mat VisionService::safe_capture(bool flush) {
    const int MAX_RETRIES = 3;
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        std::unique_lock<std::timed_mutex> lock(cam_lock_, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::milliseconds(
                (int)(DetectionConfig::CAPTURE_TIMEOUT * 1000))))
            continue;

        if (flush && DetectionConfig::CAPTURE_FLUSH_COUNT > 0) {
            for (int i = 0; i < DetectionConfig::CAPTURE_FLUSH_COUNT; i++) {
                cv::Mat junk = camera_.capture();
                if (junk.empty()) break;
            }
            if (DetectionConfig::CAPTURE_SETTLE_MS > 0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(DetectionConfig::CAPTURE_SETTLE_MS));
        }

        cv::Mat frame = camera_.capture();
        if (!frame.empty()) {
            last_capture_time_ = now_sec();
            return frame;
        }
        log_msg(LOG_WARNING, "Camera capture failed (%d/%d)", attempt, MAX_RETRIES);
        sleep_sec(0.1);
    }

    log_msg(LOG_ERROR, "Camera %d attempts failed -> recovery", MAX_RETRIES);
    if (restart_camera()) {
        std::lock_guard<std::timed_mutex> lock(cam_lock_);
        cv::Mat frame = camera_.capture();
        if (!frame.empty()) { last_capture_time_ = now_sec(); return frame; }
    }
    log_msg(LOG_CRITICAL, "Camera all recovery failed");
    auto_restart();
    return cv::Mat();
}

bool VisionService::restart_camera() {
    if (camera_recovering_) return false;
    camera_recovering_ = true;
    camera_sleeping_   = false;
    log_msg(LOG_WARNING, "Attempting camera recovery...");
    watchdog_heartbeat_.store(now_sec());
    {
        std::lock_guard<std::timed_mutex> lock(cam_lock_);
        camera_.stop();
        sleep_sec(0.5);
        watchdog_heartbeat_.store(now_sec());
        if (camera_.start()) {
            sleep_sec(1.0);
            last_capture_time_ = now_sec();
            log_msg(LOG_WARNING, "Camera recovery OK");
            camera_recovering_ = false;
            return true;
        }
    }
    log_msg(LOG_CRITICAL, "Camera recovery failed");
    camera_recovering_ = false;
    return false;
}

void VisionService::sleep_camera() {
    if (camera_sleeping_ || camera_recovering_) return;
    std::lock_guard<std::timed_mutex> lock(cam_lock_);
    camera_.stop();
    camera_sleeping_ = true;
    gpio.light_off();
    log_msg(LOG_WARNING, "Camera SLEEP");
}

bool VisionService::wake_camera() {
    if (!camera_sleeping_) return true;
    log_msg(LOG_WARNING, "Camera WAKE");
    {
        std::lock_guard<std::timed_mutex> lock(cam_lock_);
        camera_sleeping_ = false;
        watchdog_heartbeat_.store(now_sec());
        camera_.stop();
        sleep_sec(0.3);
        if (!camera_.start()) {
            log_msg(LOG_ERROR, "Camera wake failed");
            camera_sleeping_ = true;
        } else {
            sleep_sec(DetectionConfig::CAMERA_WAKE_SETTLE);
            for (int i = 0; i < 3; i++) {
                cv::Mat f = camera_.capture();
                if (f.empty()) break;
            }
            last_capture_time_ = now_sec();
            keepalive_fail_count_ = 0;
            gpio.toggle_light();
            if (!gpio.light_on_) gpio.toggle_light();
            log_msg(LOG_WARNING, "Camera WAKE OK");
            return true;
        }
    }
    return restart_camera();
}

void VisionService::auto_restart() {
    bool expected = false;
    if (!restart_in_progress_.compare_exchange_strong(expected, true)) return;
    running_ = false;
    log_msg(LOG_CRITICAL, "AUTO-RESTART");
    std::thread([]() { sleep_sec(5); _exit(1); }).detach();
    gpio.cleanup();
    camera_.close();
    sleep_sec(0.5);
    char self[4096];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len > 0) { self[len] = 0; execl(self, self, nullptr); }
    _exit(1);
}

void VisionService::force_quit() {
    bool expected = false;
    if (!cleanup_done_.compare_exchange_strong(expected, true)) return;
    stop();
}

// ══════════════════════════════════════════════════════
// Background threads
// ══════════════════════════════════════════════════════
void VisionService::start_watchdog() {
    watchdog_thread_ = std::thread([this]() {
        while (running_) {
            sleep_sec(2);
            double elapsed = now_sec() - watchdog_heartbeat_.load();
            if (elapsed > DetectionConfig::MAIN_LOOP_TIMEOUT) {
                log_msg(LOG_CRITICAL, "WATCHDOG: hung %.1fs -> AUTO-RESTART", elapsed);
                auto_restart();
            }
        }
    });
}

void VisionService::start_camera_keepalive() {
    camera_keepalive_thread_ = std::thread([this]() {
        while (running_) {
            sleep_sec(0.5);
            if (state.calibration_mode) {
                last_capture_time_     = now_sec();
                last_trigger_activity_ = now_sec();
                continue;
            }
            if (camera_recovering_) continue;
            if (camera_sleeping_) continue;
            if (now_sec() - last_trigger_activity_ >= DetectionConfig::CAMERA_SLEEP_TIMEOUT) {
                sleep_camera();
                last_trigger_activity_ = now_sec();
                continue;
            }
            if (now_sec() - last_capture_time_ >= DetectionConfig::CAMERA_KEEPALIVE_INTERVAL) {
                std::unique_lock<std::timed_mutex> lock(cam_lock_, std::defer_lock);
                if (!lock.try_lock()) continue;
                cv::Mat f = camera_.capture();
                if (!f.empty()) {
                    last_capture_time_ = now_sec();
                    keepalive_fail_count_ = 0;
                } else {
                    keepalive_fail_count_++;
                    if (keepalive_fail_count_ >= 3) {
                        keepalive_fail_count_ = 0;
                        lock.unlock();
                        restart_camera();
                        last_trigger_activity_ = now_sec();
                    }
                }
            }
        }
    });
}

// ══════════════════════════════════════════════════════
// Internal frame processing
// ══════════════════════════════════════════════════════
VisionService::FullProcessResult
VisionService::process_frame_internal(const cv::Mat& frame, bool fast) {
    FullProcessResult res;
    cv::Mat roi = processor_.warp_roi(frame);
    res.roi_vis = fast ? roi.clone() : processor_.sharpen(roi);

    cv::Mat det_roi;
    double scale;
    if (fast) { det_roi = processor_.warp_roi_fast(frame); scale = 2.0; }
    else       { det_roi = roi;                             scale = 1.0; }

    auto [gray, thresh] = processor_.preprocess(det_roi);
    res.thresh = thresh;

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(thresh, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
    if (contours.empty()) {
        res.has_metrics = false;
        res.result      = ProductResult::WAIT;
        res.info_text   = "No contour";
        return res;
    }

    int max_idx = 0; double max_area = 0;
    for (int i = 0; i < (int)contours.size(); i++) {
        double a = cv::contourArea(contours[i]);
        if (a > max_area) { max_area = a; max_idx = i; }
    }
    auto& lc = contours[max_idx];

    std::vector<cv::Point> lc_full;
    if (fast && scale != 1.0) {
        lc_full.resize(lc.size());
        for (size_t i = 0; i < lc.size(); i++)
            lc_full[i] = cv::Point((int)(lc[i].x * scale), (int)(lc[i].y * scale));
    } else { lc_full = lc; }

    auto br = cv::boundingRect(lc_full);
    if (fast && scale != 1.0) {
        auto brh = cv::boundingRect(lc);
        res.metrics = ImageProcessor::calculate_metrics(lc, brh.x, brh.y,
                                                        brh.width, brh.height, thresh);
        res.metrics.width       = br.width;
        res.metrics.height      = br.height;
        res.metrics.area        = cv::contourArea(lc_full);
        res.metrics.spike_min_w = (int)(res.metrics.spike_min_w * scale);
        res.metrics.spike_max_w = (int)(res.metrics.spike_max_w * scale);
        res.metrics.top_width   = (int)(res.metrics.top_width   * scale);
    } else {
        res.metrics = ImageProcessor::calculate_metrics(lc_full, br.x, br.y,
                                                        br.width, br.height, thresh);
    }
    res.has_metrics = true;

    auto [result, info] = classifier_.classify(lc_full, res.metrics);
    res.result    = result;
    res.info_text = info;

    if (result != ProductResult::WAIT) {
        std::vector<std::vector<cv::Point>> ctrs = {lc_full};
        cv::drawContours(res.roi_vis, ctrs, -1, cv::Scalar(0, 255, 0), 2);
    }
    return res;
}

VisionService::FastDetectResult
VisionService::detect_only(const cv::Mat& frame) {
    FastDetectResult res;
    res.det_roi      = processor_.warp_roi_fast(frame);
    auto [gray, thresh] = processor_.preprocess(res.det_roi);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(thresh, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
    if (contours.empty()) {
        res.result      = ProductResult::WAIT;
        res.info_text   = "No contour";
        res.has_metrics = false;
        return res;
    }

    int max_idx = 0; double max_area = 0;
    for (int i = 0; i < (int)contours.size(); i++) {
        double a = cv::contourArea(contours[i]);
        if (a > max_area) { max_area = a; max_idx = i; }
    }
    auto& lc  = contours[max_idx];
    auto brh  = cv::boundingRect(lc);
    res.metrics = ImageProcessor::calculate_metrics(lc, brh.x, brh.y,
                                                    brh.width, brh.height, thresh);
    // Scale ×2 to full-res
    res.contour.resize(lc.size());
    for (size_t i = 0; i < lc.size(); i++)
        res.contour[i] = cv::Point(lc[i].x * 2, lc[i].y * 2);
    res.metrics.width       = brh.width  * 2;
    res.metrics.height      = brh.height * 2;
    res.metrics.area        = cv::contourArea(res.contour);
    res.metrics.spike_min_w *= 2;
    res.metrics.spike_max_w *= 2;
    res.metrics.top_width   *= 2;
    res.has_metrics = true;

    auto [result, info] = classifier_.classify(res.contour, res.metrics);
    res.result    = result;
    res.info_text = info;
    return res;
}

// ══════════════════════════════════════════════════════
// Public API — called by TouchApp main loop
// ══════════════════════════════════════════════════════
InspectionResult VisionService::process_trigger(double trigger_time) {
    InspectionResult out;
    double t_start = now_sec();
    log_msg(LOG_WARNING, ">>> TRIGGER (delay=%.0fms)",
            trigger_time > 0 ? (t_start - trigger_time) * 1000.0 : 0.0);

    watchdog_heartbeat_.store(t_start);
    last_trigger_activity_ = t_start;

    state.transition(AppState::BUSY);
    ScopedBusySignal busy_signal(gpio);

    double event_time = (trigger_time > 0) ? trigger_time : t_start;

    if (camera_sleeping_) {
        if (!wake_camera()) {
            log_msg(LOG_ERROR, "Trigger: camera wake failed -> OK");
            gpio.signal_result(SerialConfig::PIN_OK);
            state.transition(AppState::IDLE);
            out.label = "OK"; out.result = ProductResult::OK;
            return out;
        }
    }

    try {
        double debounce_s = DetectionConfig::TRIGGER_DEBOUNCE_MS / 1000.0;
        if (event_time - last_trigger_time_ < debounce_s) {
            log_msg(LOG_WARNING, "Trigger: debounce -> OK");
            gpio.signal_result(SerialConfig::PIN_OK);
            last_trigger_time_ = event_time;
            state.transition(AppState::RESULT_SHOWN);
            out.label = "OK"; out.result = ProductResult::OK;
            return out;
        }

        double cap_t0 = now_sec();
        cv::Mat frame = safe_capture(true);
        if (frame.empty()) {
            log_msg(LOG_ERROR, "Trigger: camera failed -> OK");
            gpio.signal_result(SerialConfig::PIN_OK);
            last_trigger_time_ = event_time;
            state.transition(AppState::IDLE);
            out.label = "OK"; out.result = ProductResult::OK;
            return out;
        }

        auto det = detect_only(frame);
        double cap_dur = now_sec() - cap_t0;
        capture_ms_.store(cap_dur * 1000.0);
        capture_fps_.store(cap_dur > 0 ? 1.0 / cap_dur : 0);

        if (det.result == ProductResult::WAIT) {
            log_msg(LOG_WARNING, "Trigger result: WAIT -> OK");
            gpio.signal_result(SerialConfig::PIN_OK);
            last_trigger_time_ = event_time;
            state.transition(AppState::IDLE);
            out.label = "OK"; out.result = ProductResult::OK;
            return out;
        }

        if (det.result == ProductResult::OK) {
            gpio.signal_result(SerialConfig::PIN_OK);   // CRITICAL PATH first
            state.increment_ok();
            log_msg(LOG_WARNING, "Trigger result: OK");
            out.label  = "OK";
            out.result = ProductResult::OK;
        } else {
            gpio.signal_result(SerialConfig::PIN_NG);
            state.increment_ng();
            log_msg(LOG_WARNING, "Trigger result: NG");
            out.label  = "NG";
            out.result = ProductResult::NG;
        }

        out.metrics     = det.metrics;
        out.has_metrics = det.has_metrics;
        out.info_text   = det.info_text;
        out.cycle_ms    = cap_dur * 1000.0;

        // Full-res ROI for HMI display (runs AFTER signal_result)
        try {
            cv::Mat roi_vis = processor_.warp_roi(frame);
            if (!det.contour.empty()) {
                std::vector<std::vector<cv::Point>> ctrs = {det.contour};
                cv::drawContours(roi_vis, ctrs, -1, cv::Scalar(0, 255, 0), 2);
            }
            out.roi_vis         = roi_vis;
            last_result_frame_  = roi_vis;
        } catch (...) {}

        state.last_label     = out.label;
        state.last_info_text = out.info_text;
        last_trigger_time_   = event_time;

        state.transition(AppState::RESULT_SHOWN);
        return out;

    } catch (std::exception& e) {
        log_msg(LOG_ERROR, "Trigger error: %s -> OK", e.what());
        gpio.signal_result(SerialConfig::PIN_OK);
        last_trigger_time_ = event_time;
        state.transition(AppState::ALARM);
        out.label = "OK"; out.result = ProductResult::OK;
        return out;
    }
}

InspectionResult VisionService::process_frame_for_calibration() {
    watchdog_heartbeat_.store(now_sec());
    InspectionResult out;

    cv::Mat frame = safe_capture(false);
    if (frame.empty()) {
        out.result = ProductResult::WAIT;
        out.label  = "WAIT";
        return out;
    }

    double cap_t0 = now_sec();
    auto res = process_frame_internal(frame);
    double cap_dur = now_sec() - cap_t0;
    capture_ms_.store(cap_dur * 1000.0);
    capture_fps_.store(cap_dur > 0 ? 1.0 / cap_dur : 0);

    ProductResult calib_result = res.result;
    if (res.has_metrics && calib_result == ProductResult::WAIT)
        calib_result = ProductResult::NG;

    out.result      = calib_result;
    out.has_metrics = res.has_metrics;
    out.metrics     = res.metrics;
    out.info_text   = res.info_text;
    out.roi_vis     = state.params.show_thresh && !res.thresh.empty()
                        ? [&]{ cv::Mat t; cv::cvtColor(res.thresh, t, cv::COLOR_GRAY2BGR); return t; }()
                        : res.roi_vis;
    out.thresh      = res.thresh;
    out.cycle_ms    = cap_dur * 1000.0;

    switch (calib_result) {
        case ProductResult::OK:   out.label = "OK";   break;
        case ProductResult::NG:   out.label = "NG";   break;
        default:                  out.label = "WAIT";
    }
    state.last_label     = out.label;
    state.last_info_text = out.info_text;
    last_result_frame_   = out.roi_vis;
    return out;
}
