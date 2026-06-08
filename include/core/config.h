// -*- coding: utf-8 -*-
// core/config.h — All compile-time and runtime config constants
// Centralized config — no more scattered #define across files
#pragma once

#include <opencv2/opencv.hpp>

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
// UART Serial Config — Jetson ↔ ESP32 via /dev/ttyTHS1
// ══════════════════════════════════════════════════════
// Architecture: Jetson → UART → ESP32 → Opto → PLC
//   Jetson pin 8  (TX) → ESP32 GPIO16 (RX)
//   Jetson pin 10 (RX) ← ESP32 GPIO17 (TX)
//   ESP32 GPIO4 = PLC TRIGGER INPUT
//   ESP32 GPIO5 = OK, GPIO6 = NG, GPIO7 = BUSY, GPIO8 = LIGHT
struct SerialConfig {
    static constexpr const char* UART_DEVICE  = "/dev/ttyTHS1";
    static constexpr int         BAUD_RATE    = 115200;

    // Commands: Jetson → ESP32
    static constexpr const char* CMD_LIGHT_ON  = "LIGHT_ON";
    static constexpr const char* CMD_LIGHT_OFF = "LIGHT_OFF";
    static constexpr const char* CMD_OK_ON     = "OK_ON";
    static constexpr const char* CMD_NG_ON     = "NG_ON";
    static constexpr const char* CMD_BUSY_ON   = "BUSY_ON";
    static constexpr const char* CMD_BUSY_OFF  = "BUSY_OFF";

    // Responses: ESP32 → Jetson
    static constexpr const char* MSG_TRIGGER   = "TRIGGER";
    static constexpr const char* MSG_READY     = "ESP32 READY";

    // Virtual pin IDs
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
    static constexpr int    CAMERA_FRAME_WIDTH          = 1920;
    static constexpr int    CAMERA_FRAME_HEIGHT         = 1080;
    static constexpr double MIN_AREA                    = 2500.0;
    static constexpr int    MIN_HEIGHT                  = 80;
    static constexpr double MIN_SOLIDITY                = 0.50;
    static constexpr int    FIXED_THRESHOLD             = 170;
    static constexpr double WAITING_RENDER_INTERVAL     = 0.2;
    static constexpr double MAIN_LOOP_TIMEOUT           = 15.0;
    static constexpr double CAPTURE_TIMEOUT             = 2.0;
    static constexpr double CAMERA_KEEPALIVE_INTERVAL   = 2.0;
    static constexpr double CAMERA_SLEEP_TIMEOUT        = 300.0;
    static constexpr double CAMERA_WAKE_SETTLE          = 1.0;
    static constexpr int    CAPTURE_FLUSH_COUNT         = 0;
    static constexpr int    CAPTURE_SETTLE_MS           = 0;
    static constexpr int    TRIGGER_DEBOUNCE_MS         = 50;
    static constexpr double SPIKE_RATIO_THRESHOLD       = 0.05;
    static constexpr double SPIKE_CAP_ZONE              = 0.50;
    static constexpr bool   EDGE_WHITE_ON_BLACK_MODE    = false;
    static constexpr bool   ROTATE_ROI_180              = false;
    static constexpr bool   ROTATE_ROI_USING_SRC_REMAP  = false;

    // Keep camera frames neutral; thresholding is sensitive to over-brightening.
    static constexpr double CC_BRIGHTNESS  = 0.0;
    static constexpr double CC_CONTRAST    = 1.0;
    static constexpr double CC_SATURATION  = 1.0;
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
