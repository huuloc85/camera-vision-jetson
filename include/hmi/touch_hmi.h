// -*- coding: utf-8 -*-
// hmi/touch_hmi.h — Touch HMI UI layer for Jetson (1024×600)
// Chỉ render UI — không chạm GPIO, camera, hay thuật toán detect
#pragma once

#include "hmi/theme.h"
#include "core/types.h"
#include "vision/vision_service.h"
#include <string>
#include <vector>

// ══════════════════════════════════════════════════════
// UIButton — touch-friendly industrial button
// ══════════════════════════════════════════════════════
struct UIButton {
    std::string name;
    std::string icon;
    int x, y, w, h;
    cv::Scalar  color;
    double      fscale;
    bool        pressed = false;
    bool        visible = true;
    double      pressed_since = 0;

    UIButton(const std::string& n, int x_, int y_, int w_, int h_,
             cv::Scalar col, double fs = 0.65, const std::string& ico = "")
        : name(n), icon(ico), x(x_), y(y_), w(w_), h(h_), color(col), fscale(fs) {}

    bool contains(int px, int py) const {
        return visible && px >= x && px <= x+w && py >= y && py <= y+h;
    }
};

// ══════════════════════════════════════════════════════
// TouchHMI — renders one frame to cv::Mat
// Input: latest InspectionResult from VisionService
// Output: composed HMI canvas (cv::Mat)
// ══════════════════════════════════════════════════════
class TouchHMI {
public:
    static constexpr int W       = 1024;
    static constexpr int H       = 600;
    static constexpr int BAR_H   = 52;
    static constexpr int BTN_H   = 64;
    static constexpr int BTN_BAR = 96;
    static constexpr int BTN_Y   = H - BTN_BAR + 12;

    explicit TouchHMI(VisionService* svc);

    // Render full HMI frame from latest vision result
    cv::Mat draw(const cv::Mat& frame, const InspectionResult& result);

    // Button interaction
    void handle(const std::string& name);
    bool handle_password_touch(int x, int y);
    void handle_key(int key);
    bool password_active() const { return password_active_; }
    std::vector<UIButton>& buttons();

    // Hold-repeat state for +/- buttons
    std::string held_;
    double      held_t_  = 0;
    double      repeat_  = 0.12;

    std::vector<UIButton> normal_;   // Normal / PLC mode buttons
    std::vector<UIButton> calib_;    // Calibration mode buttons

private:
    VisionService* svc_;
    std::string result_label_ = "READY";
    bool        is_minimized_ = false;
    bool        password_active_ = false;
    std::string password_input_;
    double      password_error_until_ = 0;

    void build_buttons();
    void open_password_prompt();
    bool check_password() const;

    // ─── Drawing subroutines ──────────────────
    void draw_status_bar(cv::Mat& canvas, const InspectionResult& result);
    void draw_btn_bar(cv::Mat& canvas);
    void draw_btn(cv::Mat& canvas, UIButton& btn);
    void draw_fps(cv::Mat& canvas);
    void draw_counter_cards(cv::Mat& canvas);
    void draw_result_panel(cv::Mat& canvas, int rpx, int rpy, int rph,
                           const InspectionResult& result);
    void draw_password_modal(cv::Mat& canvas);
};

// ══════════════════════════════════════════════════════
// TouchApp — owns VisionService + TouchHMI, runs main loop
// ══════════════════════════════════════════════════════
class TouchApp {
public:
    TouchApp();
    void run();

private:
    VisionService vision_;
    TouchHMI      hmi_;

    void on_mouse(int event, int x, int y, int flags);
    static void on_mouse_callback(int event, int x, int y, int flags, void* ud);

    std::string run_waiting_state();
    std::string run_calibration_mode();
    std::string run_trigger_cycle(double trigger_time);
};
