// -*- coding: utf-8 -*-
// core/config.h — All compile-time and runtime config constants
// Centralized config — no more scattered #define across files
#pragma once

#include <opencv2/opencv.hpp>
#include <string>

// ══════════════════════════════════════════════════════
// CUDA GPU Acceleration (Jetson Nano Maxwell 128-core)
// ══════════════════════════════════════════════════════
#if defined(__has_include)
#  if __has_include(<opencv2/cudaimgproc.hpp>)
#    include <opencv2/cudaimgproc.hpp>
#    include <opencv2/cudafilters.hpp>
#    include <opencv2/cudawarping.hpp>
#    include <opencv2/cudaarithm.hpp>
#    define USE_CUDA_ACCEL 1
#  endif
#endif
#ifndef USE_CUDA_ACCEL
#  define USE_CUDA_ACCEL 0
#endif

// ══════════════════════════════════════════════════════
// Direct GPIO Config — Jetson 40-pin header → PLC/opto inputs
// ══════════════════════════════════════════════════════
// Uses BOARD numbering, matching Jetson.GPIO.setmode(GPIO.BOARD):
//   BOARD 15 = OK
//   BOARD 13 = NG
//   BOARD 16 = BUSY
//   BOARD 22 = PLC TRIGGER input from opto, active-HIGH rising edge
struct GPIOConfig {
    static constexpr int BOARD_OK      = 15;
    static constexpr int BOARD_NG      = 13;
    static constexpr int BOARD_BUSY    = 16;
    static constexpr int BOARD_TRIGGER = 22;
    static constexpr bool OUTPUT_ACTIVE_LOW = false;
    static constexpr bool BUSY_ACTIVE_LOW = false;
    static constexpr bool TRIGGER_ACTIVE_LOW = false;

    // Logical pin IDs used by VisionService
    static constexpr int PIN_OK = 1;
    static constexpr int PIN_NG = 2;

    static std::string pin_name(int pin) {
        if (pin == PIN_OK) return "OK";
        if (pin == PIN_NG) return "NG";
        return "PIN(" + std::to_string(pin) + ")";
    }
};

// ══════════════════════════════════════════════════════
// Camera & Detection Config
// ══════════════════════════════════════════════════════
struct DetectionConfig {
    static constexpr int    CAMERA_FRAME_WIDTH          = 1280;
    static constexpr int    CAMERA_FRAME_HEIGHT         = 720;
    static constexpr int    CAMERA_FPS                  = 120;
    static constexpr double MIN_AREA                    = 2500.0;
    static constexpr int    MIN_HEIGHT                  = 80;
    static constexpr double MIN_SOLIDITY                = 0.50;
    static constexpr int    FIXED_THRESHOLD             = 170;
    static constexpr double WAITING_RENDER_INTERVAL     = 0.2;
    static constexpr double MAIN_LOOP_TIMEOUT           = 15.0;
    static constexpr double CAPTURE_TIMEOUT             = 2.0;
    static constexpr int    TRIGGER_FRESH_FRAME_WAIT_MS = 35;
    static constexpr int    TRIGGER_MAX_STALE_FRAME_MS  = 80;
    static constexpr double CAMERA_KEEPALIVE_INTERVAL   = 2.0;
    static constexpr double CAMERA_SLEEP_TIMEOUT        = 300.0;
    static constexpr double CAMERA_WAKE_SETTLE          = 1.0;
    static constexpr int    CAPTURE_FLUSH_COUNT         = 0;
    static constexpr double SPIKE_RATIO_THRESHOLD       = 0.05;
    static constexpr double SPIKE_CAP_ZONE              = 0.50;
    static constexpr bool   EDGE_WHITE_ON_BLACK_MODE    = false;
    static constexpr bool   ROTATE_ROI_180              = false;
    static constexpr bool   ROTATE_ROI_USING_SRC_REMAP  = false;
    static constexpr bool   TRIGGER_RENDER_FULL_ROI     = true;
};

// ══════════════════════════════════════════════════════
// Display Config
// ══════════════════════════════════════════════════════
struct DisplayConfig {
    static constexpr int    FONT            = cv::FONT_HERSHEY_SIMPLEX;
    static constexpr double SCALE_LARGE     = 1.4;
    static constexpr double SCALE_MEDIUM    = 1.2;
    static constexpr double SCALE_SMALL     = 0.8;
    static constexpr int    THICKNESS       = 3;
    static constexpr int    THICKNESS_THIN  = 2;
    static constexpr int    STATUS_H        = 60;
    static constexpr int    PANEL_W         = 380;
};
