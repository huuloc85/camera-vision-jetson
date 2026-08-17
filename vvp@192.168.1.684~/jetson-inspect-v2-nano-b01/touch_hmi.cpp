// -*- coding: utf-8 -*-
// hmi/touch_hmi.cpp — Touch HMI render layer + TouchApp main loop
// Không chạm camera, GPIO, hay thuật toán detect — chỉ render UI
#include "hmi/touch_hmi.h"
#include "core/logger.h"
#include "core/time_utils.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <unistd.h>
#include <csignal>

using core::now_sec;

extern volatile sig_atomic_t g_signal_received;

namespace {
constexpr double EXIT_HOLD_SECONDS = 2.0;
constexpr const char* PASSWORD_LABELS[12] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "DEL", "0", "OK"
};

struct PasswordLayout {
    int modal_w = 600;
    int modal_h = 560;
    int modal_x = (TouchHMI::W - modal_w) / 2;
    int modal_y = 20;
    int key_w = 152;
    int key_h = 64;
    int key_gap = 12;
    int keys_x = modal_x + (modal_w - (key_w * 3 + key_gap * 2)) / 2;
    int keys_y = modal_y + 163;
    int cancel_x = modal_x + 60;
    int cancel_y = modal_y + 480;
    int cancel_w = modal_w - 120;
    int cancel_h = 52;
};
}

static cv::Scalar state_color(AppState state) {
    switch (state) {
        case AppState::IDLE:
        case AppState::RESULT_SHOWN:
            return Theme::GREEN;
        case AppState::BUSY:
        case AppState::INITIALIZING:
            return Theme::YELLOW;
        case AppState::ALARM:
        case AppState::DISCONNECTED:
            return Theme::RED;
        default:
            return Theme::TXT_DIM;
    }
}

static std::string state_label(AppState state) {
    switch (state) {
        case AppState::IDLE:          return "READY";
        case AppState::RESULT_SHOWN:  return "RESULT";
        case AppState::BUSY:          return "BUSY";
        case AppState::INITIALIZING:  return "INIT";
        case AppState::ALARM:         return "ALARM";
        case AppState::DISCONNECTED:  return "OFFLINE";
        default:                      return "UNKNOWN";
    }
}

static void put_centered_text(cv::Mat& canvas, const std::string& text,
                              int x, int y, int w, int h,
                              double scale, cv::Scalar color, int thickness = 1) {
    int bl;
    auto sz = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, thickness, &bl);
    cv::putText(canvas, text, {x + (w - sz.width) / 2, y + (h + sz.height) / 2},
                cv::FONT_HERSHEY_SIMPLEX, scale, color, thickness, cv::LINE_AA);
}

// ══════════════════════════════════════════════════════
// TouchHMI
// ══════════════════════════════════════════════════════
TouchHMI::TouchHMI(VisionService* svc) : svc_(svc) {
    build_buttons();
}

void TouchHMI::build_buttons() {
    int Y = BTN_Y, BH = BTN_H, G = 10;

    // Normal (PLC) mode
    normal_.emplace_back("Can Chinh", G,           Y, 210, BH, Theme::BTN_CHECK, 0.62);
    normal_.emplace_back("Reset",     G + 220,     Y, 135, BH, Theme::BTN_RESET, 0.60);
    normal_.emplace_back("Mat Khau",  G + 365,     Y, 205, BH, Theme::BTN_PARAM, 0.45);
    normal_.emplace_back("Thu Nho",   W - G - 330, Y, 130, BH, Theme::BTN_RESET, 0.52);
    normal_.emplace_back("Thoat",     W - G - 190, Y, 190, BH, Theme::BTN_EXIT,  0.72);

    // Vision calibration mode
    int x = G;
    calib_.emplace_back("Quay Lai", x, Y, 120, BH, Theme::BTN_BACK, 0.52); x += 128;
    calib_.emplace_back("ROI", x, Y, 70, BH, Theme::BTN_THRESH, 0.58); x += 78;
    calib_.emplace_back("Reset Ts", x, Y, 100, BH, Theme::BTN_RESET, 0.46); x += 108;
    calib_.emplace_back("Nhi Phan", x, Y, 150, BH, Theme::BTN_THRESH, 0.46); x += 158;
    calib_.emplace_back("Area", x, Y, 70, BH, Theme::BTN_PARAM, 0.52); x += 78;
    calib_.emplace_back("Spike", x, Y, 75, BH, Theme::BTN_PARAM, 0.48); x += 83;
    calib_.emplace_back("Thr", x, Y, 60, BH, Theme::BTN_PARAM, 0.55);
    calib_.emplace_back("-",           W-G-200,  Y,  90, BH, Theme::BTN_MINUS,  1.1);
    calib_.emplace_back("+",           W-G-100,  Y,  90, BH, Theme::BTN_PLUS,   1.1);

    // ROI calibration mode
    x = G;
    roi_calib_.emplace_back("Quay Lai", x, Y, 120, BH, Theme::BTN_BACK, 0.52); x += 128;
    roi_calib_.emplace_back("Vision", x, Y, 90, BH, Theme::BTN_CHECK, 0.52); x += 98;
    roi_calib_.emplace_back("Reset ROI", x, Y, 110, BH, Theme::BTN_RESET, 0.44); x += 118;
    roi_calib_.emplace_back("Luu ROI", x, Y, 100, BH, Theme::BTN_PLUS, 0.48); x += 108;
}

std::vector<UIButton>& TouchHMI::buttons() {
    if (!svc_->state.calibration_mode) return normal_;
    if (roi_page_active()) return roi_calib_;
    return calib_;
}

// ── Drawing subroutines ───────────────────────────────
void TouchHMI::draw_btn(cv::Mat& canvas, UIButton& btn) {
    if (!btn.visible) return;
    int x = btn.x, y = btn.y, w = btn.w, h = btn.h;
    cv::Scalar base = btn.color;
    bool selected = false;

    // Dynamic state overrides
    if (btn.name == "Area" || btn.name == "Spike" || btn.name == "Thr") {
        int idx = btn.name == "Area" ? 0 : (btn.name == "Spike" ? 1 : 2);
        if (svc_->state.calibration_mode && svc_->state.params.selected_param == idx)
            selected = true;
    }
    cv::Scalar border = selected ? Theme::BTN_PARAM_S : base;
    cv::Scalar body = (btn.pressed || selected)
        ? Theme::darken(border, btn.pressed ? 0.52 : 0.36)
        : Theme::BG_CARD;
    cv::rectangle(canvas, {x, y}, {x+w, y+h}, body, -1);

    double exit_progress = 0.0;
    if (btn.name == "Thoat" && btn.pressed) {
        exit_progress = std::clamp((now_sec() - btn.pressed_since) / EXIT_HOLD_SECONDS,
                                   0.0, 1.0);
        int fill_w = static_cast<int>((w - 2) * exit_progress);
        if (fill_w > 0) {
            cv::rectangle(canvas, {x + 1, y + 1}, {x + fill_w, y + h - 1},
                          Theme::BTN_EXIT, -1);
        }
    }
    cv::rectangle(canvas, {x, y}, {x+w, y+h}, border,
                  (btn.pressed || selected) ? 3 : 1, cv::LINE_AA);

    std::string label = btn.name;
    if      (btn.name == "Can Chinh") label = "CAN CHINH";
    else if (btn.name == "Reset")     label = "RESET";
    else if (btn.name == "Thu Nho")   label = "THU NHO";
    else if (btn.name == "Phong To")  label = "PHONG TO";
    else if (btn.name == "Thoat")     label = "THOAT";
    else if (btn.name == "Quay Lai")  label = "QUAY LAI";
    else if (btn.name == "Nhi Phan")  label = "ANH NHI PHAN";
    else if (btn.name == "Reset Ts")  label = "RESET TS";
    else if (btn.name == "Reset ROI") label = "RESET ROI";
    else if (btn.name == "Luu ROI")   label = "LUU ROI";
    if (btn.name == "Mat Khau") {
        label = password_protection_enabled_ ? "MAT KHAU: BAT" : "MAT KHAU: TAT";
    }
    if (btn.name == "Thoat" && btn.pressed) {
        label = exit_progress >= 1.0
            ? "DANG THOAT"
            : "GIU " + std::to_string(static_cast<int>(exit_progress * 100.0)) + "%";
    }
    cv::Scalar txt_c = btn.pressed ? Theme::TXT2 : Theme::TXT;
    put_centered_text(canvas, label, x+6, y, w-6, h, btn.fscale, txt_c, 2);
}

void TouchHMI::draw_counter_cards(cv::Mat& canvas) {
    char buf[64]; int bl;
    auto& st = svc_->state;
    snprintf(buf, sizeof(buf), "%d", st.total_ok); std::string ok_v = buf;
    snprintf(buf, sizeof(buf), "%d", st.total_ng); std::string ng_v = buf;
    snprintf(buf, sizeof(buf), "%d", st.total_ok + st.total_ng); std::string total_v = buf;

    struct StatCard { const char* label; std::string value; cv::Scalar color; int w; };
    StatCard cards[] = {
        {"OK", ok_v, Theme::GREEN, 74},
        {"NG", ng_v, Theme::RED, 74},
        {"TOTAL", total_v, Theme::TXT, 92},
    };

    int gap = 8, card_h = BAR_H - 10, card_y = 5;
    int total_w = -gap;
    for (const auto& c : cards) total_w += c.w + gap;
    int rx = W - total_w - 10;

    for (const auto& c : cards) {
        cv::rectangle(canvas, {rx, card_y}, {rx+c.w, card_y+card_h}, Theme::BG_CARD, -1);
        cv::rectangle(canvas, {rx, card_y}, {rx+3, card_y+card_h}, c.color, -1);
        cv::rectangle(canvas, {rx, card_y}, {rx+c.w, card_y+card_h}, Theme::DIVIDER, 1);
        cv::putText(canvas, c.label, {rx+10, card_y+13},
                    cv::FONT_HERSHEY_SIMPLEX, 0.30, Theme::TXT_DIM, 1, cv::LINE_AA);
        auto vsz = cv::getTextSize(c.value, cv::FONT_HERSHEY_SIMPLEX, 0.55, 2, &bl);
        cv::putText(canvas, c.value, {rx+c.w-vsz.width-8, card_y+card_h-9},
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, c.color, 2, cv::LINE_AA);
        rx += c.w + gap;
    }
}

void TouchHMI::draw_status_bar(cv::Mat& canvas, const InspectionResult& result) {
    cv::rectangle(canvas, {0, 0}, {W, BAR_H}, Theme::BG_PANEL, -1);
    cv::line(canvas, {0, BAR_H-1}, {W, BAR_H-1}, Theme::DIVIDER_GLOW, 2);

    AppState app_state = svc_->current_state();
    cv::Scalar app_c = state_color(app_state);

    cv::circle(canvas, {22, BAR_H/2}, 7, app_c, -1, cv::LINE_AA);
    cv::circle(canvas, {22, BAR_H/2}, 11, Theme::DIVIDER, 1, cv::LINE_AA);
    cv::putText(canvas, "CAP INSPECT", {42, 23},
                cv::FONT_HERSHEY_SIMPLEX, 0.45, Theme::TXT, 2, cv::LINE_AA);
    cv::putText(canvas, "TOUCH HMI", {42, 41},
                cv::FONT_HERSHEY_SIMPLEX, 0.30, Theme::TXT_DIM, 1, cv::LINE_AA);

    int bl;
    const int bh2 = BAR_H - 14;
    auto draw_badge = [&](int x, int width, const std::string& name,
                          const std::string& value, cv::Scalar color) {
        cv::rectangle(canvas, {x, 7}, {x+width, 7+bh2}, Theme::BG_CARD, -1);
        cv::rectangle(canvas, {x, 7}, {x+4, 7+bh2}, color, -1);
        cv::rectangle(canvas, {x, 7}, {x+width, 7+bh2}, Theme::DIVIDER, 1);
        cv::putText(canvas, name, {x+14, 20}, cv::FONT_HERSHEY_SIMPLEX,
                    0.28, Theme::TXT_DIM, 1, cv::LINE_AA);
        auto size = cv::getTextSize(value, cv::FONT_HERSHEY_SIMPLEX, 0.42, 2, &bl);
        cv::putText(canvas, value, {x+width-size.width-8, 41},
                    cv::FONT_HERSHEY_SIMPLEX, 0.42, color, 2, cv::LINE_AA);
    };

    draw_badge(192, 126, "STATE", state_label(app_state), app_c);

    bool cal = svc_->state.calibration_mode;
    std::string badge_txt = cal ? "CALIBRATION" : "PLC MODE";
    cv::Scalar badge_c = cal ? Theme::YELLOW : Theme::CYAN;
    draw_badge(326, 138, "MODE", badge_txt, badge_c);

    const char* camera_index = std::getenv("JETSON_CAM_V4L2_INDEX");
    const std::string camera_source = std::string("USB /dev/video") +
        ((camera_index && *camera_index) ? camera_index : "0");
    cv::putText(canvas, camera_source, {482, 23},
                cv::FONT_HERSHEY_SIMPLEX, 0.34, Theme::TXT_DIM, 1, cv::LINE_AA);
    char cycle_buf[32];
    snprintf(cycle_buf, sizeof(cycle_buf), "%.0f ms", result.cycle_ms);
    cv::putText(canvas, cycle_buf, {482, 41}, cv::FONT_HERSHEY_SIMPLEX,
                0.34, Theme::TXT_DIM, 1, cv::LINE_AA);

    draw_counter_cards(canvas);
}

void TouchHMI::draw_btn_bar(cv::Mat& canvas) {
    int y = H - BTN_BAR;
    cv::rectangle(canvas, {0, y}, {W, H}, Theme::BG_PANEL, -1);
    cv::line(canvas, {0, y}, {W, y}, Theme::DIVIDER_GLOW, 2);
}

void TouchHMI::draw_result_panel(cv::Mat& canvas, int rpx, int rpy, int rph,
    const InspectionResult& result) {
    const bool calibration = svc_->state.calibration_mode;
    const bool roi_cal = calibration && roi_page_active();
    cv::Scalar accent_c = Theme::CYAN;
    if (roi_cal) {
        accent_c = Theme::YELLOW;
    } else if (result.label == "OK") {
        accent_c = Theme::GREEN;
    } else if (result.label == "NG" || result.label == "GPIO FAIL") {
        accent_c = Theme::RED;
    } else if (result.label == "WAIT") {
        accent_c = Theme::YELLOW;
    }

    cv::rectangle(canvas, {rpx, rpy}, {W, rpy+rph}, Theme::BG_PANEL, -1);
    cv::rectangle(canvas, {rpx, rpy}, {rpx+5, rpy+rph}, accent_c, -1);
    cv::rectangle(canvas, {rpx, rpy}, {W-1, rpy+rph}, Theme::DIVIDER, 1);

    const int panel_w = W - rpx;
    const std::string title = roi_cal ? "ROI CONFIG"
                              : (calibration ? "CONFIG" : "INSPECTION");
    cv::putText(canvas, title, {rpx+18, rpy+30},
                cv::FONT_HERSHEY_SIMPLEX, 0.40,
                calibration ? Theme::TXT : Theme::TXT_DIM,
                calibration ? 2 : 1, cv::LINE_AA);

    char buf[64];
    int cur_y = calibration ? rpy + 64 : rpy + 180;
    auto draw_row = [&](const std::string& label, const std::string& value,
                        cv::Scalar value_color = Theme::TXT2) {
        if (cur_y > rpy + rph - 18) return;
        cv::putText(canvas, label, {rpx+18, cur_y}, cv::FONT_HERSHEY_SIMPLEX,
                    0.32, Theme::TXT_DIM, 1, cv::LINE_AA);
        int baseline = 0;
        double value_scale = 0.36;
        auto label_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX,
                                          0.32, 1, &baseline);
        auto size = cv::getTextSize(value, cv::FONT_HERSHEY_SIMPLEX,
                                    value_scale, 2, &baseline);
        while (value_scale > 0.26 && W-size.width-14 <= rpx+18+label_size.width+6) {
            value_scale -= 0.02;
            size = cv::getTextSize(value, cv::FONT_HERSHEY_SIMPLEX,
                                   value_scale, 2, &baseline);
        }
        cv::putText(canvas, value, {W-size.width-14, cur_y}, cv::FONT_HERSHEY_SIMPLEX,
                    value_scale, value_color, 2, cv::LINE_AA);
        cur_y += 25;
    };

    auto draw_adjust_card = [&](int param_index, const std::string& label,
                                const std::string& value,
                                const std::string& measured,
                                cv::Scalar measured_color) {
        const int row_h = 38;
        const cv::Rect row(rpx + 10, cur_y - 18, panel_w - 36, row_h);
        const bool selected = svc_->state.params.selected_param == param_index;
        cv::rectangle(canvas, row,
                      selected ? cv::Scalar(64, 72, 82) : Theme::BG_CARD, -1);
        cv::rectangle(canvas, row,
                      selected ? Theme::CYAN : Theme::DIVIDER_GLOW,
                      selected ? 2 : 1);
        cv::putText(canvas, label, {rpx+18, cur_y-1},
                    cv::FONT_HERSHEY_SIMPLEX, 0.34, Theme::TXT_DIM, 1, cv::LINE_AA);
        int baseline = 0;
        const auto value_size = cv::getTextSize(value, cv::FONT_HERSHEY_SIMPLEX,
                                                0.48, 2, &baseline);
        cv::putText(canvas, value,
                    {row.x + row.width - value_size.width - 8, cur_y-1},
                    cv::FONT_HERSHEY_SIMPLEX, 0.48,
                    selected ? Theme::CYAN : measured_color, 2, cv::LINE_AA);
        if (!measured.empty()) {
            cv::putText(canvas, measured, {rpx+18, cur_y+15},
                        cv::FONT_HERSHEY_SIMPLEX, 0.30,
                        measured_color, 1, cv::LINE_AA);
        }
        cur_y += row_h + 4;
    };

    if (!calibration) {
        const cv::Rect result_box(rpx+18, rpy+54, panel_w-36, 96);
        cv::rectangle(canvas, result_box, cv::Scalar(24, 29, 35), -1);
        cv::rectangle(canvas, {result_box.x, result_box.y},
                      {result_box.x+result_box.width, result_box.y+5}, accent_c, -1);
        cv::rectangle(canvas, result_box, Theme::DIVIDER, 1);
        const std::string hero = result.label.empty() ? "READY" : result.label;
        put_centered_text(canvas, hero, result_box.x, result_box.y,
                          result_box.width, result_box.height,
                          hero == "READY" ? 0.88 : 1.55, accent_c, 4);
    }

    if (roi_cal) {
        const auto& pts = svc_->state.roi_params.points;
        snprintf(buf, sizeof(buf), "%.0f, %.0f", pts[0].x, pts[0].y);
        draw_row("X, Y", buf, Theme::TXT2);
        snprintf(buf, sizeof(buf), "%.0f x %.0f",
                 pts[1].x - pts[0].x, pts[3].y - pts[0].y);
        draw_row("W x H", buf, Theme::YELLOW);
        draw_row("COORD", "1920 x 1080", Theme::CYAN);
        draw_row("ACTION", "DRAG RECTANGLE", Theme::TXT);
        draw_row("SAVE", "LUU ROI", Theme::GREEN);
    } else if (calibration) {
        const auto& p = svc_->state.params;
        snprintf(buf, sizeof(buf), "%.0f", p.min_area);
        const std::string min_area = buf;
        const std::string measured_area = result.has_metrics
            ? std::to_string(static_cast<int>(std::round(result.metrics.area)))
            : "--";
        draw_adjust_card(0, "Min Area", min_area, measured_area,
                         !result.has_metrics ? Theme::TXT
                             : (result.metrics.area >= p.min_area
                                 ? Theme::GREEN : Theme::RED));

        snprintf(buf, sizeof(buf), "%.2f", p.min_spike_ratio);
        const std::string min_spike = buf;
        std::string measured_spike = "--";
        if (result.has_metrics) {
            snprintf(buf, sizeof(buf), "%.3f", result.metrics.spike_ratio);
            measured_spike = buf;
        }
        draw_adjust_card(1, "Min Spike", min_spike, measured_spike,
                         !result.has_metrics ? Theme::TXT
                             : (result.metrics.spike_ratio >= p.min_spike_ratio
                                 ? Theme::GREEN : Theme::RED));

        snprintf(buf, sizeof(buf), "%d", p.threshold);
        draw_adjust_card(2, "Threshold", buf, "", Theme::TXT);
        cv::putText(canvas, "-/+ DE CHINH", {rpx+18, cur_y},
                    cv::FONT_HERSHEY_SIMPLEX, 0.36, Theme::TXT_DIM, 1, cv::LINE_AA);
        cur_y += 28;
        draw_row("RESULT", result.label.empty() ? "READY" : result.label, accent_c);
        if (result.has_metrics) {
            snprintf(buf, sizeof(buf), "%.0f", result.metrics.area);
            draw_row("AREA", buf, Theme::TXT);
            snprintf(buf, sizeof(buf), "%.3f", result.metrics.spike_ratio);
            draw_row("SPIKE", buf, Theme::CYAN);
            snprintf(buf, sizeof(buf), "%.2f", result.metrics.solidity);
            draw_row("SOLIDITY", buf, Theme::TXT);
        }
    } else {
        snprintf(buf, sizeof(buf), "%.0f ms", result.cycle_ms);
        draw_row("CYCLE", result.cycle_ms > 0 ? buf : "--", Theme::TXT);
        if (result.has_metrics) {
            snprintf(buf, sizeof(buf), "%.0f", result.metrics.area);
            draw_row("AREA", buf, Theme::TXT);
            snprintf(buf, sizeof(buf), "%d px", result.metrics.height);
            draw_row("HEIGHT", buf, Theme::TXT);
            snprintf(buf, sizeof(buf), "%.3f", result.metrics.spike_ratio);
            draw_row("SPIKE", buf, Theme::CYAN);
            snprintf(buf, sizeof(buf), "%.2f", result.metrics.solidity);
            draw_row("SOLIDITY", buf, Theme::TXT);
        } else {
            draw_row("DETECTION", "NO CONTOUR", Theme::YELLOW);
        }
    }

    if (!calibration && !result.info_text.empty()) {
        int info_y = std::max(cur_y + 6, rpy + rph - 74);
        cv::putText(canvas, "LY DO", {rpx+18, info_y}, cv::FONT_HERSHEY_SIMPLEX,
                    0.32, Theme::TXT, 2, cv::LINE_AA);
        info_y += 18;
        std::istringstream words(result.info_text);
        std::string line, word;
        while (words >> word && info_y <= rpy+rph-12) {
            const std::string candidate = line.empty() ? word : line + " " + word;
            int baseline = 0;
            const int width = cv::getTextSize(candidate, cv::FONT_HERSHEY_SIMPLEX,
                                               0.29, 1, &baseline).width;
            if (!line.empty() && width > panel_w-36) {
                cv::putText(canvas, line, {rpx+18, info_y}, cv::FONT_HERSHEY_SIMPLEX,
                            0.29, Theme::TXT_DIM, 1, cv::LINE_AA);
                info_y += 16;
                line = word;
            } else {
                line = candidate;
            }
        }
        if (!line.empty() && info_y <= rpy+rph-12)
            cv::putText(canvas, line, {rpx+18, info_y}, cv::FONT_HERSHEY_SIMPLEX,
                        0.29, Theme::TXT_DIM, 1, cv::LINE_AA);
    }
}

// ── Main draw ─────────────────────────────────────────
cv::Mat TouchHMI::draw(const cv::Mat& frame, const InspectionResult& result) {
    cv::Mat canvas(H, W, CV_8UC3, Theme::BG);

    const cv::Rect video_area(CONTENT_MARGIN, BAR_H + CONTENT_MARGIN,
                              W - RIGHT_PANEL_W - CONTENT_MARGIN * 2,
                              H - BAR_H - BTN_BAR - CONTENT_MARGIN * 2);
    cv::rectangle(canvas, video_area, Theme::darken(Theme::BG, 0.82), -1);
    cv::rectangle(canvas, video_area, Theme::DIVIDER, 2);

    cv::Mat frame_vis;
    if (!frame.empty()) {
        if      (frame.channels() == 1) cv::cvtColor(frame, frame_vis, cv::COLOR_GRAY2BGR);
        else if (frame.channels() == 3) frame_vis = frame;
        else if (frame.channels() == 4) cv::cvtColor(frame, frame_vis, cv::COLOR_BGRA2BGR);
    }

    int xo = video_area.x, yo = video_area.y;
    int nw = video_area.width, nh = video_area.height;
    int fw = frame_vis.cols, fh = frame_vis.rows;
    if (fw > 0 && fh > 0) {
        double scale = std::min((double)video_area.width / fw,
                                (double)video_area.height / fh);
        nw = (int)(fw * scale); nh = (int)(fh * scale);
        if (nw > 0 && nh > 0) {
            xo = video_area.x + (video_area.width - nw) / 2;
            yo = video_area.y + (video_area.height - nh) / 2;
            displayed_frame_size_ = frame_vis.size();
            displayed_frame_rect_ = cv::Rect(xo, yo, nw, nh);
            // Resize directly into the final canvas ROI. This produces the
            // same pixels/interpolation while avoiding one full temporary
            // image allocation and copy on every product.
            cv::Mat video_roi = canvas(displayed_frame_rect_);
            cv::resize(frame_vis, video_roi, video_roi.size(), 0, 0,
                       scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
            cv::rectangle(canvas, {xo-1, yo-1}, {xo+nw+1, yo+nh+1}, Theme::DIVIDER_GLOW, 1);
        }
    } else {
        displayed_frame_size_ = cv::Size();
        displayed_frame_rect_ = cv::Rect();
        put_centered_text(canvas, "WAITING CAMERA", video_area.x, video_area.y,
                          video_area.width, video_area.height,
                          0.72, Theme::TXT_DIM, 2);
    }

    // Right result panel
    draw_result_panel(canvas, W - RIGHT_PANEL_W, BAR_H,
                      H - BAR_H - BTN_BAR, result);

    // Status bar and button bar
    draw_status_bar(canvas, result);
    draw_btn_bar(canvas);
    for (auto& b : buttons()) draw_btn(canvas, b);
    if (password_active_) draw_password_modal(canvas);
    return canvas;
}

bool TouchHMI::handle_roi_pointer(int event, int x, int y) {
    if (!svc_->state.calibration_mode || !roi_page_active() ||
        displayed_frame_size_.width <= 0 || displayed_frame_size_.height <= 0 ||
        displayed_frame_rect_.width <= 0 || displayed_frame_rect_.height <= 0)
        return false;

    auto canvas_to_frame = [&](int px, int py) {
        px = std::clamp(px, displayed_frame_rect_.x,
                        displayed_frame_rect_.x + displayed_frame_rect_.width - 1);
        py = std::clamp(py, displayed_frame_rect_.y,
                        displayed_frame_rect_.y + displayed_frame_rect_.height - 1);
        float fx = static_cast<float>(px - displayed_frame_rect_.x) *
                   displayed_frame_size_.width / displayed_frame_rect_.width;
        float fy = static_cast<float>(py - displayed_frame_rect_.y) *
                   displayed_frame_size_.height / displayed_frame_rect_.height;
        return cv::Point2f(fx, fy);
    };

    if (event == cv::EVENT_LBUTTONDOWN) {
        if (!displayed_frame_rect_.contains(cv::Point(x, y))) return false;
        roi_dragging_ = true;
        roi_drag_start_frame_ = canvas_to_frame(x, y);
        return true;
    }

    if (event == cv::EVENT_MOUSEMOVE && roi_dragging_) {
        cv::Point2f current = canvas_to_frame(x, y);
        svc_->set_roi_rectangle_from_frame(
            roi_drag_start_frame_, current, displayed_frame_size_);
        return true;
    }

    if (event == cv::EVENT_LBUTTONUP && roi_dragging_) {
        cv::Point2f current = canvas_to_frame(x, y);
        svc_->set_roi_rectangle_from_frame(
            roi_drag_start_frame_, current, displayed_frame_size_);
        roi_dragging_ = false;
        return true;
    }
    return false;
}

void TouchHMI::open_password_prompt(bool disable_protection) {
    password_active_ = true;
    password_disable_pending_ = disable_protection;
    password_input_.clear();
    password_error_until_ = 0;
}

bool TouchHMI::check_password() const {
    const char* env = std::getenv("HMI_CALIB_PASSWORD");
    std::string expected = (env && *env) ? env : "1234";
    return password_input_ == expected;
}

void TouchHMI::complete_password_action() {
    password_active_ = false;
    password_input_.clear();
    password_error_until_ = 0;

    if (password_disable_pending_) {
        password_protection_enabled_ = false;
        password_disable_pending_ = false;
        log_msg(LOG_WARNING, "HMI password protection: OFF");
        return;
    }

    calib_page_ = CalibPage::VISION;
    svc_->state.calibration_mode = true;
}

bool TouchHMI::submit_password(bool force) {
    const char* env = std::getenv("HMI_CALIB_PASSWORD");
    const std::string expected = (env && *env) ? env : "1234";
    if (check_password()) {
        complete_password_action();
        return true;
    }
    if (force || password_input_.size() >= expected.size()) {
        password_input_.clear();
        password_error_until_ = now_sec() + 1.5;
    }
    return false;
}

void TouchHMI::draw_password_modal(cv::Mat& canvas) {
    cv::Mat overlay(canvas.size(), canvas.type(), cv::Scalar(0, 0, 0));
    cv::addWeighted(overlay, 0.55, canvas, 0.45, 0, canvas);

    const PasswordLayout p;
    Draw::rrect(canvas, p.modal_x, p.modal_y, p.modal_w, p.modal_h, Theme::BG_PANEL, 8);
    cv::rectangle(canvas, {p.modal_x, p.modal_y},
                  {p.modal_x+p.modal_w, p.modal_y+p.modal_h}, Theme::DIVIDER_GLOW, 2);
    cv::rectangle(canvas, {p.modal_x, p.modal_y},
                  {p.modal_x+p.modal_w, p.modal_y+7}, Theme::CYAN, -1);

    put_centered_text(canvas,
                      password_disable_pending_ ? "MAT KHAU TAT BAO VE"
                                                : "MAT KHAU CAN CHINH",
                      p.modal_x, p.modal_y+10, p.modal_w, 52,
                      0.78, Theme::CYAN, 2);

    std::string stars(password_input_.size(), '*');
    int password_x = p.modal_x + 60;
    int password_y = p.modal_y + 68;
    int password_w = p.modal_w - 120;
    int password_h = 68;
    Draw::rrect(canvas, password_x, password_y, password_w, password_h, Theme::BG_CARD, 5);
    cv::rectangle(canvas, {password_x, password_y},
                  {password_x+password_w, password_y+password_h}, Theme::DIVIDER, 1);
    int bl;
    auto psz = cv::getTextSize(stars, cv::FONT_HERSHEY_SIMPLEX, 1.15, 3, &bl);
    cv::putText(canvas, stars,
                {p.modal_x+(p.modal_w-psz.width)/2, password_y+46},
                cv::FONT_HERSHEY_SIMPLEX, 1.15, Theme::TXT, 3, cv::LINE_AA);

    if (now_sec() < password_error_until_) {
        put_centered_text(canvas, "SAI MAT KHAU",
                          p.modal_x, p.modal_y+137, p.modal_w, 26,
                          0.54, Theme::RED, 2);
    }

    for (int i = 0; i < 12; i++) {
        int col = i % 3, row = i / 3;
        int x = p.keys_x + col * (p.key_w + p.key_gap);
        int y = p.keys_y + row * (p.key_h + p.key_gap);
        std::string label = PASSWORD_LABELS[i];
        cv::Scalar c = (label == "OK") ? Theme::BTN_PLUS
                     : (label == "DEL" ? Theme::BTN_MINUS : Theme::BTN_PARAM);
        Draw::rrect(canvas, x, y, p.key_w, p.key_h, Theme::BG_CARD, 6);
        cv::rectangle(canvas, {x, y}, {x+p.key_w, y+p.key_h}, Theme::DIVIDER, 2);
        cv::rectangle(canvas, {x, y}, {x+7, y+p.key_h}, c, -1);
        auto sz = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX,
                                  label.size() > 1 ? 0.72 : 1.05, 3, &bl);
        cv::putText(canvas, label,
                    {x+(p.key_w-sz.width)/2, y+(p.key_h+sz.height)/2},
                    cv::FONT_HERSHEY_SIMPLEX, label.size() > 1 ? 0.72 : 1.05,
                    Theme::TXT, 3, cv::LINE_AA);
    }

    Draw::rrect(canvas, p.cancel_x, p.cancel_y, p.cancel_w, p.cancel_h,
                Theme::BTN_RESET, 6);
    cv::rectangle(canvas, {p.cancel_x, p.cancel_y},
                  {p.cancel_x+p.cancel_w, p.cancel_y+p.cancel_h}, Theme::DIVIDER, 2);
    put_centered_text(canvas, "HUY", p.cancel_x, p.cancel_y, p.cancel_w, p.cancel_h,
                      0.68, Theme::TXT, 2);
}

bool TouchHMI::handle_password_touch(int x, int y) {
    if (!password_active_) return false;

    const PasswordLayout p;
    if (x >= p.cancel_x && x <= p.cancel_x + p.cancel_w &&
        y >= p.cancel_y && y <= p.cancel_y + p.cancel_h) {
        password_active_ = false;
        password_disable_pending_ = false;
        password_input_.clear();
        return true;
    }

    for (int i = 0; i < 12; i++) {
        int col = i % 3, row = i / 3;
        int bx = p.keys_x + col * (p.key_w + p.key_gap);
        int by = p.keys_y + row * (p.key_h + p.key_gap);
        if (x < bx || x > bx + p.key_w || y < by || y > by + p.key_h) continue;

        std::string label = PASSWORD_LABELS[i];
        if (label == "DEL") {
            if (!password_input_.empty()) password_input_.pop_back();
        } else if (label == "OK") {
            submit_password(true);
        } else if (password_input_.size() < 8) {
            password_input_ += label;
            submit_password(false);
        }
        return true;
    }
    return true;
}

void TouchHMI::handle_key(int key) {
    if (!password_active_) return;
    if (key >= '0' && key <= '9') {
        if (password_input_.size() < 8) {
            password_input_.push_back((char)key);
            submit_password(false);
        }
    } else if (key == 8 || key == 127) {
        if (!password_input_.empty()) password_input_.pop_back();
    } else if (key == 13 || key == '\n') {
        submit_password(true);
    } else if (key == 27) {
        password_active_ = false;
        password_disable_pending_ = false;
        password_input_.clear();
    }
}

void TouchHMI::handle(const std::string& name) {
    auto& st = svc_->state;
    if      (name == "Can Chinh") {
        if (password_protection_enabled_) {
            open_password_prompt(false);
        } else {
            calib_page_ = CalibPage::VISION;
            st.calibration_mode = true;
        }
    }
    else if (name == "Mat Khau") {
        if (password_protection_enabled_) {
            open_password_prompt(true);
        } else {
            password_protection_enabled_ = true;
            log_msg(LOG_WARNING, "HMI password protection: ON");
        }
    }
    else if (name == "Quay Lai" || name == "<- Quay Lai") {
        st.calibration_mode = false;
        roi_dragging_ = false;
        calib_page_ = CalibPage::VISION;
        st.last_label = "READY";
        st.last_color = cv::Scalar(255,255,0);
        svc_->capture_fps_.store(0);
        svc_->capture_ms_.store(0);
        result_label_ = "READY";
    }
    else if (name == "Reset" || name == "Reset Dem") st.reset_counters();
    else if (name == "Vision")     { calib_page_ = CalibPage::VISION; }
    else if (name == "ROI")        { calib_page_ = CalibPage::ROI; }
    else if (name == "Reset ROI")  { svc_->reset_roi_settings(); }
    else if (name == "Luu ROI")    { svc_->save_roi_settings(); }
    else if (name == "Reset Ts")   { st.params.reset();         st.save_state(); }
    else if (name == "Thresh" || name == "Nhi Phan") { st.params.show_thresh = !st.params.show_thresh; st.save_state(); }
    else if (name == "Area")       st.params.selected_param = 0;
    else if (name == "Spike")      st.params.selected_param = 1;
    else if (name == "Thr")        st.params.selected_param = 2;
    else if (name == "+")          { st.params.adjust(true);  st.save_state(); }
    else if (name == "-")          { st.params.adjust(false); st.save_state(); }
    else if (name == "Thoat")      svc_->running_ = false;
    else if (name == "Thu Nho" || name == "Phong To") {
        is_minimized_ = !is_minimized_;
        for (auto& btn : normal_) {
            if (btn.name == "Thu Nho" || btn.name == "Phong To")
                btn.name = is_minimized_ ? "Phong To" : "Thu Nho";
        }
        if (is_minimized_) {
            cv::setWindowProperty("HMI", cv::WND_PROP_FULLSCREEN, cv::WINDOW_NORMAL);
            cv::resizeWindow("HMI", 640, 380);
        } else {
            cv::setWindowProperty("HMI", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
        }
    }
}

// ══════════════════════════════════════════════════════
// TouchApp
// ══════════════════════════════════════════════════════
TouchApp::TouchApp() : hmi_(&vision_) {
    last_display_result_.label = "READY";
    // Suppress stderr during window creation
    int dn = open("/dev/null", O_WRONLY);
    int bk = dup(2); dup2(dn, 2);

    cv::namedWindow("HMI", cv::WINDOW_NORMAL);
    cv::resizeWindow("HMI", 1024, 600);
    cv::moveWindow("HMI", 0, 0);
    try { cv::setWindowProperty("HMI", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN); }
    catch (...) {}
    cv::waitKey(1);

    dup2(bk, 2); ::close(bk); ::close(dn);
    cv::setMouseCallback("HMI", on_mouse_callback, this);
}

void TouchApp::on_mouse_callback(int event, int x, int y, int flags, void* ud) {
    static_cast<TouchApp*>(ud)->on_mouse(event, x, y, flags);
}

void TouchApp::on_mouse(int event, int x, int y, int /*flags*/) {
    if (hmi_.password_active()) {
        if (event == cv::EVENT_LBUTTONDOWN) hmi_.handle_password_touch(x, y);
        if (event == cv::EVENT_LBUTTONUP) {
            for (auto& b : hmi_.normal_) b.pressed = false;
            for (auto& b : hmi_.calib_) b.pressed = false;
            for (auto& b : hmi_.roi_calib_) b.pressed = false;
            hmi_.held_ = "";
        }
        return;
    }

    if (hmi_.handle_roi_pointer(event, x, y)) return;

    if (event == cv::EVENT_LBUTTONDOWN) {
        for (auto& btn : hmi_.buttons()) {
            if (btn.contains(x, y)) {
                btn.pressed = true;
                btn.pressed_since = now_sec();
                if (btn.name != "Thoat") hmi_.handle(btn.name);
                if (btn.name == "+" || btn.name == "-") {
                    hmi_.held_  = btn.name;
                    hmi_.held_t_ = now_sec();
                }
                break;
            }
        }
    } else if (event == cv::EVENT_LBUTTONUP) {
        double now = now_sec();
        for (auto& b : hmi_.normal_) {
            if (b.pressed && b.name == "Thoat" &&
                now - b.pressed_since >= EXIT_HOLD_SECONDS) {
                hmi_.handle(b.name);
            }
        }
        for (auto& b : hmi_.normal_) b.pressed = false;
        for (auto& b : hmi_.calib_) b.pressed = false;
        for (auto& b : hmi_.roi_calib_) b.pressed = false;
        hmi_.held_ = "";
    }
}

std::string TouchApp::run_waiting_state() {
    if (g_signal_received) return "quit";
    InspectionResult idle = last_display_result_;
    idle.label = vision_.state.last_label;
    idle.info_text = vision_.state.last_info_text;
    if (idle.label == "READY") {
        idle.has_metrics = false;
        idle.cycle_ms = 0;
    }
    vision_.heartbeat();

    double now = now_sec();
    UIButton* exit_btn = nullptr;
    for (auto& b : hmi_.normal_) {
        if (b.name == "Thoat" && b.pressed) {
            exit_btn = &b;
            break;
        }
    }

    if (exit_btn && now - exit_btn->pressed_since >= EXIT_HOLD_SECONDS) {
        cv::Mat canvas = hmi_.draw(last_display_frame_, idle);
        cv::imshow("HMI", canvas);
        cv::waitKey(1);
        hmi_.handle("Thoat");
        return "quit";
    }

    bool render = vision_.should_render_waiting_frame(now);
    if (exit_btn && now - last_hold_render_ >= 0.04) {
        render = true;
        last_hold_render_ = now;
    }
    if (render) {
        cv::Mat canvas = hmi_.draw(last_display_frame_, idle);
        cv::imshow("HMI", canvas);
    }

    int key = cv::waitKey(1) & 0xFF;
    hmi_.handle_key(key);
    if (hmi_.password_active()) return "";
    if      (key == 'q') return "quit";
    else if (key == 'm') {
        cv::setWindowProperty("HMI", cv::WND_PROP_FULLSCREEN, cv::WINDOW_NORMAL);
        cv::resizeWindow("HMI", 640, 380);
    }
    else if (key == 'f')
        cv::setWindowProperty("HMI", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
    return "";
}

std::string TouchApp::run_calibration_mode() {
    while (vision_.state.calibration_mode && vision_.running_ && !g_signal_received) {
        vision_.heartbeat();
        double now = now_sec();

        // Hold repeat for +/-
        if (hmi_.held_ == "+" || hmi_.held_ == "-") {
            if (now - hmi_.held_t_ >= hmi_.repeat_) {
                vision_.state.params.adjust(hmi_.held_ == "+");
                vision_.state.save_state();
                hmi_.held_t_ = now;
            }
        }

        InspectionResult result;
        if (hmi_.roi_page_active())
            result = vision_.process_frame_for_roi_calibration();
        else
            result = vision_.process_frame_for_calibration();
        cv::Mat canvas = hmi_.draw(result.roi_vis, result);
        cv::imshow("HMI", canvas);

        int key = cv::waitKey(1) & 0xFF;
        hmi_.handle_key(key);
        if (hmi_.password_active()) continue;
        if      (key == 'q') return "quit";
        else if (key == 'm') {
            cv::setWindowProperty("HMI", cv::WND_PROP_FULLSCREEN, cv::WINDOW_NORMAL);
            cv::resizeWindow("HMI", 640, 380);
        }
        else if (key == 'f')
            cv::setWindowProperty("HMI", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
    }
    return "";
}

std::string TouchApp::run_trigger_cycle(double trigger_time) {
    const double display_t0 = now_sec();
    InspectionResult result = vision_.process_trigger(trigger_time);
    last_display_result_ = result;
    // Zero-copy cv::Mat reference: freeze exactly one completed trigger image.
    // The waiting loop redraws this cached image and never requests a camera frame.
    if (!result.roi_vis.empty()) {
        last_display_frame_ = result.roi_vis;
        log_msg(LOG_WARNING, "HMI freeze: published %dx%d result=%s",
                last_display_frame_.cols, last_display_frame_.rows,
                result.label.c_str());
    } else {
        if (last_display_frame_.empty() && !vision_.last_result_frame().empty())
            last_display_frame_ = vision_.last_result_frame();
        log_msg(LOG_WARNING, "HMI freeze: trigger image empty, cached=%s",
                last_display_frame_.empty() ? "NO" : "YES");
    }
    cv::Mat canvas = hmi_.draw(last_display_frame_, result);
    const double compose_done = now_sec();
    cv::imshow("HMI", canvas);
    const double submit_done = now_sec();
    // Pump HighGUI without intentionally sleeping in the trigger path. The
    // normal waiting/calibration loops still use waitKey() for touch/key input.
#if CV_VERSION_MAJOR > 4 || \
    (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 5)
    cv::pollKey();
#else
    // JetPack 4 / OpenCV 4.1.1 does not provide pollKey().
    cv::waitKey(1);
#endif
    const double display_done = now_sec();
    log_msg(LOG_WARNING,
            "Trigger displayed: processing=%.0fms present=%.0fms total=%.0fms "
            "(compose=%.1fms submit=%.1fms pump=%.1fms)",
            result.cycle_ms,
            (display_done - display_t0) * 1000.0 - result.cycle_ms,
            (display_done - display_t0) * 1000.0,
            (compose_done - display_t0) * 1000.0 - result.cycle_ms,
            (submit_done - compose_done) * 1000.0,
            (display_done - submit_done) * 1000.0);
    return "";
}

void TouchApp::run() {
    log_msg(LOG_WARNING, "TouchApp started");
    try {
        while (vision_.running_ && !g_signal_received) {
            vision_.heartbeat();

            if (vision_.state.calibration_mode) {
                if (run_calibration_mode() == "quit") break;
                continue;
            }

            if (vision_.has_pending_trigger()) {
                double t = vision_.consume_trigger();
                log_msg(LOG_WARNING, "Main loop: TRIGGER");
                if (run_trigger_cycle(t) == "quit") break;
            } else {
                if (run_waiting_state() == "quit") break;
            }
        }
    } catch (std::exception& e) {
        log_msg(LOG_CRITICAL, "CRASH: %s", e.what());
        vision_.request_auto_restart();
    }
    if (g_signal_received)
        log_msg(LOG_WARNING, "Signal %d received — shutting down...", (int)g_signal_received);
    vision_.running_ = false;
}

// Entry point is in src/main.cpp — do NOT add main() here.
