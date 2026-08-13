// -*- coding: utf-8 -*-
// vision/vision_service.cpp — Core Vision Service (camera + GPIO + pipeline)
// No UI code, no OpenCV windows. Interacts with HMI via InspectionResult struct.
#include "vision/vision_service.h"
#include "core/logger.h"
#include "core/time_utils.h"

#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

extern char** environ;

using core::now_sec;
using core::sleep_sec;

namespace {
bool frame_has_signal(const cv::Mat& frame, double* mean_out = nullptr,
                      double* max_out = nullptr) {
    if (frame.empty()) return false;
    try {
        cv::Mat sample, gray;
        cv::resize(frame, sample, {64, 36}, 0, 0, cv::INTER_NEAREST);
        if (sample.channels() == 1)
            gray = sample;
        else if (sample.channels() == 3)
            cv::cvtColor(sample, gray, cv::COLOR_BGR2GRAY);
        else if (sample.channels() == 4)
            cv::cvtColor(sample, gray, cv::COLOR_BGRA2GRAY);
        else
            return false;

        cv::Scalar mean, stddev;
        cv::meanStdDev(gray, mean, stddev);
        double max_value = 0.0;
        cv::minMaxLoc(gray, nullptr, &max_value);
        if (mean_out) *mean_out = mean[0];
        if (max_out) *max_out = max_value;

        // Driver failures commonly return a correctly-sized frame containing
        // only zeros or sensor noise. A real industrial image has either a
        // visible average level or enough local contrast to pass this guard.
        return max_value >= 12.0 && (mean[0] >= 1.0 || stddev[0] >= 2.0);
    } catch (const cv::Exception&) {
        return false;
    }
}

bool prime_camera_frames(LibcameraCapture& camera, int max_attempts = 12,
                         int required_consecutive = 2) {
    int consecutive = 0;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        cv::Mat frame = camera.capture();
        if (!frame.empty() && frame_has_signal(frame)) {
            if (++consecutive >= required_consecutive)
                return true;
        } else {
            consecutive = 0;
        }
        sleep_sec(0.03);
    }
    return false;
}
}

// ══════════════════════════════════════════════════════
// VisionService — Constructor / Destructor
// ══════════════════════════════════════════════════════
VisionService::VisionService()
    : processor_(state.params, state.roi_params)
    , classifier_(state.params)
{
    cv::setNumThreads(DetectionConfig::OPENCV_NUM_THREADS);

    state.transition(AppState::INITIALIZING);

    if (!camera_.start()) {
        log_msg(LOG_CRITICAL, "Camera init failed!");
        state.transition(AppState::ALARM);
    } else {
        sleep_sec(1.0);
        if (prime_camera_frames(camera_))
            log_msg(LOG_WARNING, "Camera primed: 2 consecutive valid frames");
        else
            log_msg(LOG_WARNING, "Camera prime incomplete; trigger capture will retry");
        log_msg(LOG_WARNING,
                "Camera trigger mode: realtime capture, publish-on-trigger");
        state.transition(gpio.is_connected() ? AppState::IDLE : AppState::DISCONNECTED);
    }

    watchdog_heartbeat_.store(now_sec());
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
    if (!state.calibration_mode &&
        (state.app_state == AppState::IDLE ||
         state.app_state == AppState::RESULT_SHOWN ||
         state.app_state == AppState::DISCONNECTED)) {
        if (gpio.is_connected() && camera_.is_running()) {
            if (state.app_state == AppState::DISCONNECTED)
                state.transition(AppState::IDLE);
        } else if (!gpio.is_connected() && state.app_state != AppState::DISCONNECTED) {
            state.transition(AppState::DISCONNECTED);
        }
    }
}

std::array<cv::Point2f, 4>
VisionService::roi_points_for_frame(const cv::Size& frame_size) const {
    return processor_.roi_points_for_frame(frame_size);
}

bool VisionService::move_roi_edge_from_frame(int edge, const cv::Point2f& delta,
                                             const cv::Size& frame_size) {
    if (edge < 0 || edge >= 4 || frame_size.width <= 0 || frame_size.height <= 0)
        return false;

    // Convert a frame-space movement into the persisted 1920×1080 reference
    // coordinates. Using two points also handles the optional source remap.
    const cv::Point2f ref_zero = processor_.reference_point_from_frame(
        {0.0f, 0.0f}, frame_size);
    const cv::Point2f ref_delta_end = processor_.reference_point_from_frame(
        delta, frame_size);
    const cv::Point2f ref_delta = ref_delta_end - ref_zero;
    RoiParams candidate = state.roi_params;
    const int next = (edge + 1) % 4;
    candidate.points[edge] += ref_delta;
    candidate.points[next] += ref_delta;
    if (!candidate.valid())
        return false;

    state.roi_params = candidate;
    processor_.refresh_roi(frame_size);
    return true;
}

void VisionService::save_roi_settings() {
    state.save_state();
    log_msg(LOG_WARNING, "ROI saved: P1=(%.0f,%.0f) P2=(%.0f,%.0f) "
                         "P3=(%.0f,%.0f) P4=(%.0f,%.0f)",
            state.roi_params.points[0].x, state.roi_params.points[0].y,
            state.roi_params.points[1].x, state.roi_params.points[1].y,
            state.roi_params.points[2].x, state.roi_params.points[2].y,
            state.roi_params.points[3].x, state.roi_params.points[3].y);
}

void VisionService::reset_roi_settings() {
    state.roi_params.reset();
    processor_.refresh_roi();
    state.save_state();
    log_msg(LOG_WARNING, "ROI reset to factory defaults");
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
cv::Mat VisionService::request_realtime_frame() {
    if (!realtime_capture_enabled_)
        return cv::Mat();

    {
        std::lock_guard<std::mutex> lock(realtime_frame_mutex_);
        realtime_frame_ready_ = false;
        realtime_frame_.release();
        realtime_capture_request_time_ = now_sec();
        realtime_capture_request_ = true;
    }

    std::unique_lock<std::mutex> lock(realtime_frame_mutex_);
    realtime_frame_cv_.wait_for(
        lock, std::chrono::milliseconds(DetectionConfig::LATEST_FRAME_WAIT_MS),
        [this]() {
            return realtime_frame_ready_ || !running_ || camera_recovering_;
        });
    if (!realtime_frame_ready_)
        realtime_capture_request_ = false;
    return realtime_frame_ready_ ? realtime_frame_ : cv::Mat();
}

cv::Mat VisionService::safe_capture(bool flush, bool require_signal) {
    const int MAX_RETRIES = 3;
    auto wait_for_recovery = [this]() {
        const double deadline = now_sec() + DetectionConfig::CAPTURE_TIMEOUT +
                                DetectionConfig::CAMERA_WAKE_SETTLE + 1.0;
        while (running_ && camera_recovering_ && now_sec() < deadline) {
            watchdog_heartbeat_.store(now_sec());
            sleep_sec(0.02);
        }
        return running_ && !camera_recovering_ && camera_.is_running();
    };

    if (camera_recovering_ && !wait_for_recovery()) {
        log_msg(LOG_ERROR, "Camera recovery did not finish before capture");
        return cv::Mat();
    }

    // The realtime worker drains V4L2 with grab() continuously. A trigger
    // waits only for the next retrieve(), instead of flushing old frames in
    // the critical path.
    if (flush && realtime_capture_enabled_) {
        for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
            cv::Mat frame = request_realtime_frame();
            if (!frame.empty()) {
                double mean = 0.0, max_value = 0.0;
                if (!require_signal || frame_has_signal(frame, &mean, &max_value))
                    return frame;
                log_msg(LOG_WARNING,
                        "Camera realtime frame rejected (%d/%d mean=%.2f max=%.0f)",
                        attempt, MAX_RETRIES, mean, max_value);
            } else {
                log_msg(LOG_WARNING, "Camera realtime frame timeout (%d/%d)",
                        attempt, MAX_RETRIES);
            }
            sleep_sec(0.01);
        }

        log_msg(LOG_ERROR, "Camera realtime capture failed -> recovery");
        const bool recovered = camera_recovering_
            ? wait_for_recovery()
            : restart_camera();
        if (recovered) {
            for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
                cv::Mat frame = request_realtime_frame();
                if (!frame.empty() &&
                    (!require_signal || frame_has_signal(frame)))
                    return frame;
                sleep_sec(0.01);
            }
        }
        log_msg(LOG_CRITICAL, "Camera realtime recovery failed");
        return cv::Mat();
    }

    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        std::unique_lock<std::timed_mutex> lock(cam_lock_, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::milliseconds(
                (int)(DetectionConfig::CAPTURE_TIMEOUT * 1000))))
            continue;

        if (flush && DetectionConfig::CAPTURE_FLUSH_COUNT > 0) {
            int discarded = camera_.discard_frames(DetectionConfig::CAPTURE_FLUSH_COUNT);
            if (discarded < DetectionConfig::CAPTURE_FLUSH_COUNT) {
                log_msg(LOG_WARNING, "Camera buffer flush incomplete (%d/%d)",
                        discarded, DetectionConfig::CAPTURE_FLUSH_COUNT);
            }
            if (DetectionConfig::CAPTURE_SETTLE_MS > 0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(DetectionConfig::CAPTURE_SETTLE_MS));
        }

        cv::Mat frame = camera_.capture();
        if (!frame.empty()) {
            double mean = 0.0, max_value = 0.0;
            if (!require_signal || frame_has_signal(frame, &mean, &max_value)) {
                return frame;
            }
            log_msg(LOG_WARNING,
                    "Camera black frame rejected (%d/%d mean=%.2f max=%.0f)",
                    attempt, MAX_RETRIES, mean, max_value);
        } else {
            log_msg(LOG_WARNING, "Camera capture failed (%d/%d)", attempt, MAX_RETRIES);
        }
        lock.unlock();
        sleep_sec(0.1);
    }

    log_msg(LOG_ERROR, "Camera %d attempts failed -> recovery", MAX_RETRIES);
    const bool recovered = camera_recovering_
        ? wait_for_recovery()
        : restart_camera();
    if (recovered) {
        for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
            cv::Mat frame;
            {
                std::lock_guard<std::timed_mutex> lock(cam_lock_);
                frame = camera_.capture();
            }
            if (!frame.empty() &&
                (!require_signal || frame_has_signal(frame))) {
                return frame;
            }
            log_msg(LOG_WARNING, "Camera recovery frame invalid (%d/%d)",
                    attempt, MAX_RETRIES);
            sleep_sec(0.1);
        }
    }
    log_msg(LOG_CRITICAL, "Camera all recovery failed");
    return cv::Mat();
}

bool VisionService::restart_camera() {
    bool expected = false;
    if (!camera_recovering_.compare_exchange_strong(expected, true)) return false;
    camera_sleeping_   = false;
    log_msg(LOG_WARNING, "Attempting camera recovery...");
    watchdog_heartbeat_.store(now_sec());

    bool started = false;
    try {
        std::lock_guard<std::timed_mutex> lock(cam_lock_);
        camera_.stop();
        sleep_sec(0.5);
        watchdog_heartbeat_.store(now_sec());
        if (camera_.start()) {
            sleep_sec(1.0);
            if (!prime_camera_frames(camera_))
                log_msg(LOG_WARNING, "Camera recovery prime incomplete");
            started = camera_.is_running();
        }
    } catch (const cv::Exception& e) {
        log_msg(LOG_ERROR, "Camera recovery exception: %s", e.what());
    } catch (const std::exception& e) {
        log_msg(LOG_ERROR, "Camera recovery error: %s", e.what());
    } catch (...) {
        log_msg(LOG_ERROR, "Camera recovery unknown error");
    }

    camera_recovering_ = false;
    realtime_frame_cv_.notify_all();
    if (started) {
        log_msg(LOG_WARNING, "Camera recovery OK");
        return true;
    }
    log_msg(LOG_CRITICAL, "Camera recovery failed");
    return false;
}

void VisionService::sleep_camera() {
    if (camera_sleeping_ || camera_recovering_) return;
    std::lock_guard<std::timed_mutex> lock(cam_lock_);
    camera_.stop();
    camera_sleeping_ = true;
    log_msg(LOG_WARNING, "Camera SLEEP");
}

bool VisionService::wake_camera() {
    if (!camera_sleeping_) return true;
    camera_recovering_ = true;
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
            if (!prime_camera_frames(camera_))
                log_msg(LOG_WARNING, "Camera wake prime incomplete");
            log_msg(LOG_WARNING, "Camera WAKE OK");
            camera_recovering_ = false;
            return true;
        }
    }
    camera_recovering_ = false;
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
        int consecutive_failures = 0;
        realtime_capture_enabled_ = true;
        log_msg(LOG_WARNING, "Camera realtime capture worker ON");
        while (running_) {
            bool recover_now = false;
            try {
                if (state.calibration_mode) {
                    last_trigger_activity_ = now_sec();
                    consecutive_failures = 0;
                    sleep_sec(0.01);
                    continue;
                }
                if (camera_recovering_ || camera_sleeping_) {
                    sleep_sec(0.01);
                    continue;
                }
                if (DetectionConfig::CAMERA_SLEEP_TIMEOUT > 0.0 &&
                    now_sec() - last_trigger_activity_ >= DetectionConfig::CAMERA_SLEEP_TIMEOUT) {
                    sleep_camera();
                    last_trigger_activity_ = now_sec();
                    continue;
                }

                // Use the backend's atomic grab+retrieve operation. Splitting
                // these calls is unreliable with a single V4L2 buffer.
                cv::Mat frame = camera_.capture();
                const double frame_done = now_sec();
                if (frame.empty()) {
                    ++consecutive_failures;
                    recover_now = consecutive_failures >=
                                  DetectionConfig::CAMERA_RESTART_FAIL_COUNT;
                    if (!recover_now) {
                        sleep_sec(0.002);
                        continue;
                    }
                } else {
                    consecutive_failures = 0;
                    const bool publish_this_frame =
                        realtime_capture_request_.exchange(false);
                    if (publish_this_frame) {
                        if (realtime_capture_request_time_.load() > frame_done) {
                            realtime_capture_request_ = true;
                            continue;
                        }
                        {
                            std::lock_guard<std::mutex> lock(realtime_frame_mutex_);
                            // VideoCapture may reuse its decode buffer on the
                            // next capture. Clone the trigger frame before the
                            // backend reuses that memory.
                            realtime_frame_ = frame.clone();
                            realtime_frame_ready_ = true;
                        }
                        realtime_frame_cv_.notify_all();
                    }
                }
            } catch (const cv::Exception& e) {
                log_msg(LOG_ERROR, "Camera worker exception: %s", e.what());
                consecutive_failures = DetectionConfig::CAMERA_RESTART_FAIL_COUNT;
                recover_now = true;
            } catch (const std::exception& e) {
                log_msg(LOG_ERROR, "Camera worker error: %s", e.what());
                consecutive_failures = DetectionConfig::CAMERA_RESTART_FAIL_COUNT;
                recover_now = true;
            } catch (...) {
                log_msg(LOG_ERROR, "Camera worker unknown error");
                consecutive_failures = DetectionConfig::CAMERA_RESTART_FAIL_COUNT;
                recover_now = true;
            }

            if (recover_now && running_) {
                log_msg(LOG_WARNING,
                        "Camera worker recovery after %d consecutive failures",
                        consecutive_failures);
                realtime_capture_request_ = false;
                if (!restart_camera())
                    sleep_sec(0.5);
                consecutive_failures = 0;
            }
        }
        realtime_capture_enabled_ = false;
        realtime_frame_cv_.notify_all();
    });
}

// ══════════════════════════════════════════════════════
// Internal frame processing
// ══════════════════════════════════════════════════════
VisionService::FullProcessResult
VisionService::process_frame_internal(const cv::Mat& frame, bool fast) {
    FullProcessResult res;
    cv::Mat roi = processor_.warp_roi(frame);
    // Keep calibration preview pixel-aligned with the production ROI.
    res.roi_vis = roi.clone();

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
    if (!gpio.set_busy(true)) {
        state.transition(AppState::ALARM);
        out.result = ProductResult::WAIT;
        out.label = "GPIO FAIL";
        out.info_text = "UART BUSY command failed";
        state.last_label = out.label;
        state.last_info_text = out.info_text;
        return out;
    }

    double event_time = (trigger_time > 0) ? trigger_time : t_start;
    double result_gpio_ms = 0.0;
    double result_sent_at = 0.0;
    auto send_result = [&](int pin) {
        double gpio_t0 = now_sec();
        bool delivered = gpio.signal_result(pin);
        result_sent_at = now_sec();
        result_gpio_ms = (result_sent_at - gpio_t0) * 1000.0;
        if (delivered) return true;
        log_msg(LOG_ERROR, "GPIO result delivery failed");
        state.transition(AppState::ALARM);
        out.result = ProductResult::WAIT;
        out.label = "GPIO FAIL";
        out.info_text = "UART result delivery failed";
        state.last_label = out.label;
        state.last_info_text = out.info_text;
        return false;
    };
    auto remember_result = [&]() {
        state.last_label = out.label;
        state.last_info_text = out.info_text;
    };

    if (camera_sleeping_) {
        if (!wake_camera()) {
            log_msg(LOG_ERROR, "Trigger: camera wake failed -> OK");
            if (!send_result(SerialConfig::PIN_OK)) return out;
            state.transition(AppState::IDLE);
            out.label = "OK"; out.result = ProductResult::OK;
            out.info_text = "Camera wake failed";
            remember_result();
            return out;
        }
    }

    try {
        double debounce_s = DetectionConfig::TRIGGER_DEBOUNCE_MS / 1000.0;
        if (event_time - last_trigger_time_ < debounce_s) {
            log_msg(LOG_WARNING, "Trigger: debounce -> OK");
            if (!send_result(SerialConfig::PIN_OK)) return out;
            last_trigger_time_ = event_time;
            state.transition(AppState::RESULT_SHOWN);
            out.label = "OK"; out.result = ProductResult::OK;
            out.info_text = "Trigger debounced";
            remember_result();
            return out;
        }

        double cap_t0 = now_sec();
        cv::Mat frame = safe_capture(true);
        if (frame.empty()) {
            log_msg(LOG_ERROR, "Trigger: camera failed -> OK");
            if (!send_result(SerialConfig::PIN_OK)) return out;
            last_trigger_time_ = event_time;
            state.transition(AppState::IDLE);
            out.label = "OK"; out.result = ProductResult::OK;
            out.info_text = "Camera capture failed";
            remember_result();
            return out;
        }

        double capture_done = now_sec();
        auto det = detect_only(frame);
        double detect_done = now_sec();

        out.metrics     = det.metrics;
        out.has_metrics = det.has_metrics;
        out.info_text   = det.info_text;
        double hmi_frame_ms = 0.0;
        auto build_hmi_frame = [&]() {
            // Match the PLC result view to Calibration mode: publish the same
            // 1080x804 perspective ROI after the GPIO response. Detection
            // remains on the fast half-resolution ROI.
            double hmi_t0 = now_sec();
            try {
                cv::Mat frame_vis = processor_.warp_roi(frame);
                if (!det.contour.empty()) {
                    std::vector<std::vector<cv::Point>> ctrs = {det.contour};
                    cv::Scalar contour_color = det.result == ProductResult::NG
                        ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
                    cv::drawContours(frame_vis, ctrs, -1, contour_color, 3);
                }
                out.roi_vis = frame_vis;
                last_result_frame_ = frame_vis;
            } catch (...) {
                out.roi_vis = frame;
                last_result_frame_ = frame;
            }
            hmi_frame_ms = (now_sec() - hmi_t0) * 1000.0;
        };
        auto finish_timing = [&]() {
            double done = now_sec();
            double duration = done - t_start;
            double critical_end = result_sent_at > 0.0 ? result_sent_at : detect_done;
            out.cycle_ms = duration * 1000.0;
            capture_ms_.store(out.cycle_ms);
            capture_fps_.store(duration > 0.0 ? 1.0 / duration : 0.0);
            log_msg(LOG_WARNING,
                    "Trigger timing: busy=%.0fms camera=%.0fms detect=%.0fms gpio=%.0fms "
                    "hmi_frame=%.0fms critical=%.0fms total=%.0fms",
                    (cap_t0 - t_start) * 1000.0,
                    (capture_done - cap_t0) * 1000.0,
                    (detect_done - capture_done) * 1000.0,
                    result_gpio_ms, hmi_frame_ms,
                    (critical_end - t_start) * 1000.0,
                    out.cycle_ms);
        };

        if (det.result == ProductResult::WAIT) {
            log_msg(LOG_WARNING, "Trigger result: WAIT -> OK");
            if (!send_result(SerialConfig::PIN_OK)) return out;
            build_hmi_frame();
            finish_timing();
            last_trigger_time_ = event_time;
            state.transition(AppState::RESULT_SHOWN);
            out.label = "OK"; out.result = ProductResult::OK;
            remember_result();
            return out;
        }

        if (det.result == ProductResult::OK) {
            if (!send_result(SerialConfig::PIN_OK)) return out; // CRITICAL PATH first
            state.increment_ok();
            log_msg(LOG_WARNING, "Trigger result: OK");
            out.label  = "OK";
            out.result = ProductResult::OK;
        } else {
            if (!send_result(SerialConfig::PIN_NG)) return out;
            state.increment_ng();
            log_msg(LOG_WARNING, "Trigger result: NG");
            out.label  = "NG";
            out.result = ProductResult::NG;
        }

        state.last_label     = out.label;
        state.last_info_text = out.info_text;
        last_trigger_time_   = event_time;

        build_hmi_frame();
        finish_timing();
        state.transition(AppState::RESULT_SHOWN);
        return out;

    } catch (std::exception& e) {
        log_msg(LOG_ERROR, "Trigger error: %s -> OK", e.what());
        if (!send_result(SerialConfig::PIN_OK)) return out;
        last_trigger_time_ = event_time;
        state.transition(AppState::ALARM);
        out.label = "OK"; out.result = ProductResult::OK;
        out.info_text = e.what();
        remember_result();
        return out;
    }
}

InspectionResult VisionService::process_frame_for_calibration() {
    watchdog_heartbeat_.store(now_sec());
    InspectionResult out;

    cv::Mat frame = safe_capture(false, false);
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

InspectionResult VisionService::process_frame_for_roi_calibration() {
    watchdog_heartbeat_.store(now_sec());
    InspectionResult out;
    double t0 = now_sec();
    cv::Mat frame = safe_capture(false, false);
    double duration = now_sec() - t0;
    if (frame.empty()) {
        out.result = ProductResult::WAIT;
        out.label = "WAIT";
        return out;
    }

    capture_ms_.store(duration * 1000.0);
    capture_fps_.store(duration > 0 ? 1.0 / duration : 0.0);
    auto points = processor_.roi_points_for_frame(frame.size());
    cv::Mat preview = frame.clone();
    for (int i = 0; i < 4; ++i) {
        const cv::Point p1(cvRound(points[i].x), cvRound(points[i].y));
        const cv::Point p2(cvRound(points[(i + 1) % 4].x),
                           cvRound(points[(i + 1) % 4].y));
        bool selected = i == state.roi_params.selected_edge;
        cv::line(preview, p1, p2,
                 selected ? cv::Scalar(0, 230, 255) : cv::Scalar(255, 220, 0),
                 selected ? 11 : 7, cv::LINE_AA);
    }
    for (int i = 0; i < 4; ++i) {
        cv::Point p(cvRound(points[i].x), cvRound(points[i].y));
        bool selected = i == state.roi_params.selected_edge ||
                        i == (state.roi_params.selected_edge + 1) % 4;
        cv::circle(preview, p, selected ? 50 : 42, cv::Scalar(0, 0, 0), -1, cv::LINE_AA);
        cv::circle(preview, p, selected ? 42 : 34,
                   selected ? cv::Scalar(0, 230, 255) : cv::Scalar(255, 220, 0),
                   -1, cv::LINE_AA);
        std::string label = "P" + std::to_string(i + 1);
        cv::putText(preview, label, {p.x + 52, p.y - 30},
                    cv::FONT_HERSHEY_SIMPLEX, 1.4,
                    selected ? cv::Scalar(0, 230, 255) : cv::Scalar(255, 220, 0),
                    4, cv::LINE_AA);
    }

    out.result = ProductResult::WAIT;
    out.label = "ROI";
    out.info_text = "Drag any of the 4 ROI edges";
    out.roi_vis = preview;
    out.cycle_ms = duration * 1000.0;
    last_result_frame_ = preview;
    return out;
}
