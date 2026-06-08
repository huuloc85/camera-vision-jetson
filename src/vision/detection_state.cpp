// -*- coding: utf-8 -*-
// vision/detection_state.cpp — Counters + params + JSON persistence
#include "vision/detection_state.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/time_utils.h"

#include <fstream>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

#if defined(__has_include)
#  if __has_include(<filesystem>)
#    include <filesystem>
namespace fs = std::filesystem;
#  elif __has_include(<experimental/filesystem>)
#    include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#  else
#    error "No filesystem support found"
#  endif
#else
#  include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

using core::now_sec;

// ══════════════════════════════════════════════════════
// DetectionState
// ══════════════════════════════════════════════════════
DetectionState::DetectionState() {
    // Locate state file next to executable
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = 0;
        state_file_ = fs::path(buf).parent_path() / ".counter_state.json";
    } else {
        state_file_ = ".counter_state.json";
    }
    load_state();
}

void DetectionState::save_state() {
    try {
        std::string tmp = state_file_ + ".tmp";
        std::ofstream f(tmp);
        f << "{\"product_id\":"    << product_id
          << ",\"total_ok\":"      << total_ok
          << ",\"total_ng\":"      << total_ng
          << ",\"min_area\":"      << params.min_area
          << ",\"min_spike_ratio\":" << params.min_spike_ratio
          << ",\"threshold\":"     << params.threshold << "}";
        f.close();
        fs::rename(tmp, state_file_);
    } catch (...) {}
}

void DetectionState::load_state() {
    try {
        std::ifstream f(state_file_);
        if (!f.is_open()) return;
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        auto get_int = [&](const std::string& key) -> int {
            auto pos = content.find("\"" + key + "\"");
            if (pos == std::string::npos) return -1;
            pos = content.find(':', pos);
            if (pos == std::string::npos) return -1;
            return std::atoi(content.c_str() + pos + 1);
        };
        auto get_double = [&](const std::string& key) -> double {
            auto pos = content.find("\"" + key + "\"");
            if (pos == std::string::npos) return -1;
            pos = content.find(':', pos);
            if (pos == std::string::npos) return -1;
            return std::atof(content.c_str() + pos + 1);
        };

        int v;
        if ((v = get_int("product_id")) >= 0) product_id = v;
        if ((v = get_int("total_ok"))   >= 0) total_ok   = v;
        if ((v = get_int("total_ng"))   >= 0) total_ng   = v;
        if ((v = get_int("threshold"))  >= 0) params.threshold = v;
        double d;
        if ((d = get_double("min_area"))        >= 0) params.min_area        = d;
        if ((d = get_double("min_spike_ratio")) >= 0) params.min_spike_ratio = d;
    } catch (...) {}
}

void DetectionState::reset_counters() {
    product_id = total_ok = total_ng = 0;
    cycle_times_.clear();
    save_state();
}

void DetectionState::record_cycle() {
    cycle_times_.push_back(now_sec());
    while (cycle_times_.size() > CYCLE_WINDOW)
        cycle_times_.pop_front();
}

void DetectionState::increment_ok() {
    product_id++;
    total_ok++;
    record_cycle();
    save_state();
}

void DetectionState::increment_ng() {
    product_id++;
    total_ng++;
    record_cycle();
    save_state();
}

double DetectionState::cycle_rate() const {
    if (cycle_times_.size() < 2) return 0;
    double elapsed = cycle_times_.back() - cycle_times_.front();
    if (elapsed <= 0) return 0;
    return (cycle_times_.size() - 1) / elapsed;
}

void DetectionState::transition(AppState next) {
    if (app_state == next) return;
    log_msg(LOG_WARNING, "State: %s -> %s",
            state_name(app_state), state_name(next));
    app_state = next;
}
