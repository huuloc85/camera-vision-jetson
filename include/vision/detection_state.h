// -*- coding: utf-8 -*-
// vision/detection_state.h — Counter, params, persistence
// Tách DetectionState ra riêng — không phụ thuộc HMI hay GPIO
#pragma once

#include "core/types.h"
#include <string>
#include <deque>
#include <atomic>

// ══════════════════════════════════════════════════════
// DetectionState — persisted counters + tunable params
// ══════════════════════════════════════════════════════
class DetectionState {
public:
    // ─── Counters ────────────────────────────
    int product_id = 0;
    int total_ok   = 0;
    int total_ng   = 0;

    // ─── Runtime params (tunable via HMI) ────
    DetectionParams params;
    RoiParams       roi_params;

    // ─── Last result (for HMI display) ───────
    std::string last_label     = "READY";
    cv::Scalar  last_color{255, 255, 0};
    std::string last_info_text;

    // ─── App state machine ───────────────────
    AppState    app_state      = AppState::INITIALIZING;
    std::atomic<bool> calibration_mode{false};

    DetectionState();

    void reset_counters();
    void increment_ok();
    void increment_ng();
    double cycle_rate() const;

    // Transition state machine
    void transition(AppState next);

    // Persist counters + params to disk (.counter_state.json)
    // Called by HMI after params.adjust() / params.reset()
    void save_state();

private:
    static constexpr int CYCLE_WINDOW = 100;
    std::deque<double> cycle_times_;
    std::string state_file_;

    void load_state();
    void record_cycle();
};
