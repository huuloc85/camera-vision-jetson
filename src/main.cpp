// -*- coding: utf-8 -*-
// main.cpp — Entry point for jetson-inspect-v2
//
// Architecture:
//   main() ──► TouchApp::run()
//                 ├── VisionService  (camera + GPIO + detection pipeline)
//                 └── TouchHMI       (UI rendering, touch input)
//
// VisionService owns:
//   LibcameraCapture  (GStreamer pipeline)
//   GPIOController    (Jetson GPIO → PLC)
//   ImageProcessor    (warp ROI, threshold, metrics)
//   ProductClassifier (shape-rule classify)
//   DetectionState    (state machine + counters)
//
// TouchHMI owns:
//   cv::namedWindow + setMouseCallback
//   All draw_*() subroutines
//   Button layout (normal / calib)
//
// Communication: single-process, InspectionResult struct
//   VisionService.process_trigger() → InspectionResult → TouchHMI.draw()

#include "hmi/touch_hmi.h"
#include "core/config.h"
#include "core/logger.h"

#include <cstdlib>
#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <unistd.h>

// ══════════════════════════════════════════════════════
// Signal handling — graceful shutdown on SIGINT / SIGTERM
// ══════════════════════════════════════════════════════
static volatile sig_atomic_t g_signal_received = 0;

static void signal_handler(int sig) {
    g_signal_received = sig;
    log_msg(LOG_WARNING, "Signal %d received — shutting down...", sig);
}

// ══════════════════════════════════════════════════════
// Main
// ══════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    // Register signals
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGHUP,  signal_handler);

    // ── Print startup banner ────────────────────────
    fprintf(stderr,
        "\n"
        "╔══════════════════════════════════════════════╗\n"
        "║      jetson-inspect-v2  (Jetson GPIO build)  ║\n"
        "║  Vision: camera + Jetson GPIO → PLC          ║\n"
        "║  HMI   : OpenCV 1024×600 touch display      ║\n"
        "╚══════════════════════════════════════════════╝\n"
        "\n"
    );

    log_msg(LOG_WARNING, "Startup: PID=%d", (int)getpid());

    // ── Print build info ────────────────────────────
#if USE_CUDA_ACCEL
    log_msg(LOG_WARNING, "Build: CUDA acceleration ENABLED");
#else
    log_msg(LOG_WARNING, "Build: CUDA acceleration DISABLED (CPU-only)");
#endif

    log_msg(LOG_WARNING, "Config: GPIO BOARD OK=%d NG=%d BUSY=%d result_active=%s busy_active=%s TRIGGER=%d trigger_active=%s",
            GPIOConfig::BOARD_OK, GPIOConfig::BOARD_NG,
            GPIOConfig::BOARD_BUSY,
            GPIOConfig::OUTPUT_ACTIVE_LOW ? "LOW" : "HIGH",
            GPIOConfig::BUSY_ACTIVE_LOW ? "LOW" : "HIGH",
            GPIOConfig::BOARD_TRIGGER,
            GPIOConfig::TRIGGER_ACTIVE_LOW ? "LOW" : "HIGH");
    log_msg(LOG_WARNING, "Config: Camera %dx%d@%d  MinArea=%.0f  Threshold=%d",
            DetectionConfig::CAMERA_FRAME_WIDTH,
            DetectionConfig::CAMERA_FRAME_HEIGHT,
            DetectionConfig::CAMERA_FPS,
            DetectionConfig::MIN_AREA,
            DetectionConfig::FIXED_THRESHOLD);

    // ── Launch application ──────────────────────────
    try {
        TouchApp app;
        app.run();
    } catch (const std::exception& e) {
        log_msg(LOG_CRITICAL, "Unhandled exception: %s", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        log_msg(LOG_CRITICAL, "Unknown fatal exception");
        return EXIT_FAILURE;
    }

    log_msg(LOG_WARNING, "Clean exit.");
    return EXIT_SUCCESS;
}
