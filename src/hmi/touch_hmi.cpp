// -*- coding: utf-8 -*-
// hmi/touch_hmi.cpp — Touch HMI render layer + TouchApp main loop
// Không chạm camera, GPIO, hay thuật toán detect — chỉ render UI
#include "hmi/touch_hmi.h"
#include "core/logger.h"
#include "core/time_utils.h"

#include <fstream>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

using core::now_sec;

static double get_jetson_temp() {
    static double last_temp = 0.0, last_read = 0.0;
    double now = now_sec();
    if (now - last_read >= 2.0) {
        std::ifstream ifs("/sys/devices/virtual/thermal/thermal_zone0/temp");
        if (ifs) { long v = 0; if (ifs >> v) last_temp = v / 1000.0; }
        last_read = now;
    }
    return last_temp;
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
    normal_.emplace_back("Can Chinh", G,           Y, 180, BH, Theme::BTN_CHECK, 0.62);
    normal_.emplace_back("Reset",     G + 192,     Y, 130, BH, Theme::BTN_RESET, 0.60);
    normal_.emplace_back("Den",       G + 334,     Y, 100, BH, Theme::BTN_LIGHT_ON, 0.68);
    normal_.emplace_back("Thu Nho",   W - G - 310, Y, 130, BH, Theme::BTN_RESET, 0.58);
    normal_.emplace_back("Thoat",     W - G - 160, Y, 150, BH, Theme::BTN_EXIT,  0.72);

    // Calibration mode
    int x = G;
    calib_.emplace_back("<- Quay Lai", x,        Y, 142, BH, Theme::BTN_BACK,   0.52); x += 152;
    calib_.emplace_back("Reset Ts",    x,        Y, 110, BH, Theme::BTN_RESET,  0.48); x += 118;
    calib_.emplace_back("Thresh",      x,        Y,  90, BH, Theme::BTN_THRESH, 0.52); x += 98;
    calib_.emplace_back("Den",         x,        Y,  80, BH, Theme::BTN_LIGHT_ON, 0.52); x += 92;
    calib_.emplace_back("P1",          x,        Y,  55, BH, Theme::BTN_PARAM,  0.68);
    calib_.emplace_back("P2",          x + 62,   Y,  55, BH, Theme::BTN_PARAM,  0.68);
    calib_.emplace_back("P3",          x + 124,  Y,  55, BH, Theme::BTN_PARAM,  0.68);
    calib_.emplace_back("-",           W-G-200,  Y,  90, BH, Theme::BTN_MINUS,  1.1);
    calib_.emplace_back("+",           W-G-100,  Y,  90, BH, Theme::BTN_PLUS,   1.1);
}

std::vector<UIButton>& TouchHMI::buttons() {
    return svc_->state.calibration_mode ? calib_ : normal_;
}

// ── Drawing subroutines ───────────────────────────────
void TouchHMI::draw_btn(cv::Mat& canvas, UIButton& btn) {
    if (!btn.visible) return;
    int x = btn.x, y = btn.y, w = btn.w, h = btn.h;
    cv::Scalar base = btn.color;
    cv::Scalar bg   = btn.pressed ? Theme::darken(base, 0.5) : base;

    // Dynamic state overrides
    if (btn.name == "P1" || btn.name == "P2" || btn.name == "P3") {
        int idx = btn.name[1] - '1';
        if (svc_->state.calibration_mode && svc_->state.params.selected_param == idx)
            bg = Theme::BTN_PARAM_S;
    }
    if (btn.name == "Den")
        bg = svc_->gpio.light_on_ ? Theme::BTN_LIGHT_ON : Theme::BTN_LIGHT_OFF;

    cv::Scalar body = btn.pressed ? Theme::darken(Theme::BG_CARD, 0.78) : Theme::BG_CARD;
    Draw::rrect(canvas, x, y, w, h, body, 5);
    cv::rectangle(canvas, {x, y}, {x+w, y+h}, btn.pressed ? bg : Theme::DIVIDER, 1);
    cv::rectangle(canvas, {x, y}, {x+5, y+h}, bg, -1);
    cv::line(canvas, {x+7, y+1}, {x+w-4, y+1}, Theme::lighten(body, 16), 1, cv::LINE_AA);

    std::string label = (btn.name == "Thoat" && btn.pressed) ? "Giu 2s" : btn.name;
    cv::Scalar txt_c = btn.pressed ? cv::Scalar(200,200,200) : cv::Scalar(255,255,255);
    put_centered_text(canvas, label, x+6, y, w-6, h, btn.fscale, txt_c, 2);
}

void TouchHMI::draw_counter_cards(cv::Mat& canvas) {
    char buf[64]; int bl;
    auto& st = svc_->state;
    snprintf(buf, sizeof(buf), "%d", st.total_ok); std::string ok_v = buf;
    snprintf(buf, sizeof(buf), "%d", st.total_ng); std::string ng_v = buf;
    snprintf(buf, sizeof(buf), "%d", st.total_ok + st.total_ng); std::string total_v = buf;

    double rate = st.cycle_rate();
    cv::Scalar rate_c;
    if (rate >= 0.1) {
        double sps = 1.0 / rate;
        snprintf(buf, sizeof(buf), "%.1fs/p", sps);
        rate_c = (sps <= 0.5) ? Theme::GREEN : (sps <= 1.0 ? Theme::YELLOW : Theme::ORANGE);
    } else { snprintf(buf, sizeof(buf), "--s/p"); rate_c = Theme::TXT_DIM; }
    std::string rate_v = buf;

    struct StatCard { const char* label; std::string value; cv::Scalar color; int w; };
    StatCard cards[] = {
        {"RATE", rate_v, rate_c, 86},
        {"OK", ok_v, Theme::GREEN, 74},
        {"NG", ng_v, Theme::RED, 74},
        {"TOTAL", total_v, Theme::TXT, 92},
    };

    int gap = 8, card_h = BAR_H - 10, card_y = 5;
    int total_w = -gap;
    for (const auto& c : cards) total_w += c.w + gap;
    int rx = W - total_w - 10;

    for (const auto& c : cards) {
        Draw::rrect(canvas, rx, card_y, c.w, card_h, Theme::BG_CARD, 4);
        cv::rectangle(canvas, {rx, card_y}, {rx+3, card_y+card_h}, c.color, -1);
        cv::putText(canvas, c.label, {rx+10, card_y+13},
                    cv::FONT_HERSHEY_SIMPLEX, 0.30, Theme::TXT_DIM, 1, cv::LINE_AA);
        auto vsz = cv::getTextSize(c.value, cv::FONT_HERSHEY_SIMPLEX, 0.55, 2, &bl);
        cv::putText(canvas, c.value, {rx+c.w-vsz.width-8, card_y+card_h-9},
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, c.color, 2, cv::LINE_AA);
        rx += c.w + gap;
    }
}

void TouchHMI::draw_status_bar(cv::Mat& canvas, const InspectionResult& /*result*/) {
    Draw::rrect(canvas, 0, 0, W, BAR_H, Theme::BG_PANEL, 0);
    cv::line(canvas, {0, BAR_H-1}, {W, BAR_H-1}, Theme::DIVIDER_GLOW, 2);

    AppState app_state = svc_->current_state();
    cv::Scalar app_c = state_color(app_state);

    double pulse = (sin(now_sec() * 4.0) + 1.0) / 2.0;
    cv::Scalar dot_c = Theme::blend(Theme::darken(app_c, 0.5), app_c, pulse);
    cv::circle(canvas, {19, BAR_H/2}, 6, dot_c, -1, cv::LINE_AA);
    cv::circle(canvas, {19, BAR_H/2}, 9, Theme::darken(dot_c, 0.48), 1, cv::LINE_AA);

    std::string state_txt = state_label(app_state);
    int bx = 38, state_w = 120, bh2 = BAR_H - 14;
    Draw::rrect(canvas, bx, 7, state_w, bh2, Theme::BG_CARD, 4);
    cv::rectangle(canvas, {bx, 7}, {bx+4, 7+bh2}, app_c, -1);
    int bl;
    cv::putText(canvas, "STATE", {bx+14, 20},
                cv::FONT_HERSHEY_SIMPLEX, 0.28, Theme::TXT_DIM, 1, cv::LINE_AA);
    auto ssz = cv::getTextSize(state_txt, cv::FONT_HERSHEY_SIMPLEX, 0.48, 2, &bl);
    cv::putText(canvas, state_txt, {bx+state_w-ssz.width-9, 41},
                cv::FONT_HERSHEY_SIMPLEX, 0.48, app_c, 2, cv::LINE_AA);

    bool cal = svc_->state.calibration_mode;
    std::string badge_txt = cal ? "CALIBRATION" : "PLC MODE";
    cv::Scalar  badge_c   = cal ? Theme::YELLOW  : Theme::CYAN;
    bx += state_w + 8;
    int bw = 132;
    Draw::rrect(canvas, bx, 7, bw, bh2, Theme::BG_CARD, 4);
    cv::rectangle(canvas, {bx, 7}, {bx+4, 7+bh2}, badge_c, -1);
    cv::putText(canvas, "MODE", {bx+14, 20},
                cv::FONT_HERSHEY_SIMPLEX, 0.28, Theme::TXT_DIM, 1, cv::LINE_AA);
    auto bsz = cv::getTextSize(badge_txt, cv::FONT_HERSHEY_SIMPLEX, 0.42, 2, &bl);
    cv::putText(canvas, badge_txt, {bx+bw-bsz.width-8, 41},
                cv::FONT_HERSHEY_SIMPLEX, 0.42, badge_c, 2, cv::LINE_AA);

    // Jetson temperature
    double temp = get_jetson_temp();
    if (temp > 0.0) {
        char t_buf[32]; snprintf(t_buf, sizeof(t_buf), "%.1f C", temp);
        cv::Scalar t_color = (temp < 60.0) ? Theme::GREEN
                           : ((temp < 75.0) ? Theme::YELLOW : Theme::RED);
        int tx = bx + bw + 8, tw = 72;
        Draw::rrect(canvas, tx, 7, tw, bh2, Theme::BG_CARD, 4);
        auto t_sz = cv::getTextSize(t_buf, cv::FONT_HERSHEY_SIMPLEX, 0.45, 1, &bl);
        cv::putText(canvas, t_buf, {tx+(tw-t_sz.width)/2, 7+(bh2+t_sz.height)/2},
                    cv::FONT_HERSHEY_SIMPLEX, 0.45, t_color, 1, cv::LINE_AA);
    }

    // Right side: counters or calibration params
    if (!svc_->state.calibration_mode) {
        draw_counter_cards(canvas);
    } else {
        std::string names[] = {"Area", "Spike", "Thresh"};
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", svc_->state.params.min_area);       std::string v1 = buf;
        snprintf(buf, sizeof(buf), "%.2f", svc_->state.params.min_spike_ratio); std::string v2 = buf;
        snprintf(buf, sizeof(buf), "%d",   svc_->state.params.threshold);       std::string v3 = buf;
        std::string vals[] = {v1, v2, v3};
        int px = W - 345;
        for (int i = 0; i < 3; i++) {
            bool sel = (i == svc_->state.params.selected_param);
            cv::Scalar c = sel ? Theme::CYAN : Theme::TXT2;
            std::string info = names[i] + "=" + vals[i];
            if (sel) {
                int bl2;
                auto isz = cv::getTextSize(info, cv::FONT_HERSHEY_SIMPLEX, 0.45, 1, &bl2);
                Draw::rrect(canvas, px-4, 12, isz.width+8, BAR_H-24,
                            Theme::darken(Theme::CYAN, 0.15), 3);
            }
            cv::putText(canvas, info, {px, BAR_H/2+5},
                        cv::FONT_HERSHEY_SIMPLEX, 0.45, c, sel ? 2 : 1, cv::LINE_AA);
            auto sz = cv::getTextSize(info, cv::FONT_HERSHEY_SIMPLEX, 0.45, 1, &bl);
            px += sz.width + 16;
        }
    }
}

void TouchHMI::draw_btn_bar(cv::Mat& canvas) {
    int y = H - BTN_BAR;
    cv::rectangle(canvas, {0, y}, {W, H}, Theme::BG_PANEL, -1);
    cv::line(canvas, {0, y}, {W, y}, Theme::DIVIDER_GLOW, 2);
}

void TouchHMI::draw_fps(cv::Mat& canvas) {
    char buf[64];
    double fps = svc_->capture_fps_.load();
    double ms  = svc_->capture_ms_.load();
    if (fps <= 0) snprintf(buf, sizeof(buf), "-- ms | -- fps");
    else          snprintf(buf, sizeof(buf), "%.0f ms | %.0f fps", ms, fps);
    int bl;
    auto sz = cv::getTextSize(buf, cv::FONT_HERSHEY_SIMPLEX, 0.42, 1, &bl);
    int pad = 5;
    int bw = sz.width + pad*2 + 6, bh = sz.height + pad*2 + 4;
    int bx = W - bw - 8, by = BAR_H + 6;
    if (by + bh <= canvas.rows && bx >= 0 && bx + bw <= canvas.cols) {
        cv::Mat roi = canvas(cv::Rect(bx, by, bw, bh));
        cv::Mat fill(roi.size(), CV_8UC3, Theme::BG);
        cv::addWeighted(fill, 0.75, roi, 0.25, 0, roi);
        Draw::rrect(canvas, bx, by, bw, bh, cv::Scalar(0,0,0,0), 4);
        cv::rectangle(canvas, {bx, by}, {bx+bw, by+bh}, Theme::TXT_DIM, 1);
        cv::Scalar c = (ms <= 80) ? Theme::GREEN : (ms <= 150 ? Theme::YELLOW : Theme::RED);
        cv::circle(canvas, {bx+8, by+bh/2}, 3, c, -1, cv::LINE_AA);
        cv::putText(canvas, buf, {bx+16, by+sz.height+pad},
                    cv::FONT_HERSHEY_SIMPLEX, 0.42, c, 1, cv::LINE_AA);
    }
}

void TouchHMI::draw_result_panel(cv::Mat& canvas, int rpx, int rpy, int rph,
                                  const InspectionResult& result) {
    cv::Scalar panel_bg, accent_c, text_c;
    if      (result.label == "OK")   { panel_bg = Theme::STATUS_OK;   accent_c = text_c = Theme::GREEN;  }
    else if (result.label == "NG")   { panel_bg = Theme::STATUS_NG;   accent_c = text_c = Theme::RED;    }
    else if (result.label == "WAIT") { panel_bg = Theme::STATUS_WAIT; accent_c = text_c = Theme::YELLOW; }
    else                             { panel_bg = Theme::BG_CARD;     accent_c = text_c = Theme::CYAN;   }

    cv::rectangle(canvas, {rpx, rpy}, {W, rpy+rph}, Theme::BG_PANEL, -1);
    cv::rectangle(canvas, {rpx, rpy}, {rpx+6, rpy+rph}, accent_c, -1);
    cv::rectangle(canvas, {rpx+8, rpy+8}, {W-8, rpy+rph-8}, panel_bg, -1);
    cv::rectangle(canvas, {rpx, rpy}, {W-1, rpy+rph}, Theme::DIVIDER, 1);

    int panel_w = W - rpx;

    // Large result label
    cv::putText(canvas, "RESULT", {rpx+18, rpy+28},
                cv::FONT_HERSHEY_SIMPLEX, 0.36, Theme::TXT_DIM, 1, cv::LINE_AA);

    int result_y = rpy + 88;
    if (!result.label.empty()) {
        int bl;
        double scale = 1.6;
        int thick = 4;
        auto sz = cv::getTextSize(result.label, cv::FONT_HERSHEY_SIMPLEX, scale, thick, &bl);
        while (scale > 0.65 && sz.width > panel_w - 16) {
            scale -= 0.1;
            thick = scale < 1.0 ? 2 : 4;
            sz = cv::getTextSize(result.label, cv::FONT_HERSHEY_SIMPLEX, scale, thick, &bl);
        }
        int tx = rpx + 8 + (panel_w - 16 - sz.width) / 2;
        cv::putText(canvas, result.label, {tx+2, result_y+2}, cv::FONT_HERSHEY_SIMPLEX,
                    scale, cv::Scalar(0,0,0), thick + 1, cv::LINE_AA);
        cv::putText(canvas, result.label, {tx, result_y}, cv::FONT_HERSHEY_SIMPLEX,
                    scale, text_c, thick, cv::LINE_AA);
    }

    // Metrics
    int cur_y = result_y + 36;
    if (!result.info_text.empty() && (result.label == "OK" || result.label == "NG")) {
        double mf = 0.38;
        auto find_val = [&](const std::string& key) -> std::string {
            auto pos = result.info_text.find(key);
            if (pos == std::string::npos) return "";
            pos += key.length();
            std::string val;
            while (pos < result.info_text.size() &&
                   result.info_text[pos] != ' ' && result.info_text[pos] != '/')
                val += result.info_text[pos++];
            return val;
        };
        struct ML { std::string label, value; cv::Scalar color; };
        std::vector<ML> mlines;
        auto lv = find_val("len:"), hv = find_val("H:"), rv = find_val("r:");
        if (!lv.empty()) mlines.push_back({"Len", lv, Theme::TXT});
        if (!hv.empty()) mlines.push_back({"H",   hv, Theme::TXT});
        if (!rv.empty()) mlines.push_back({"R",   rv, Theme::CYAN});
        for (auto& m : mlines) {
            cv::putText(canvas, m.label, {rpx+18, cur_y},
                        cv::FONT_HERSHEY_SIMPLEX, mf, Theme::TXT_DIM, 1, cv::LINE_AA);
            int bl;
            auto vsz = cv::getTextSize(m.value, cv::FONT_HERSHEY_SIMPLEX, mf, 1, &bl);
            cv::putText(canvas, m.value, {W-vsz.width-8, cur_y},
                        cv::FONT_HERSHEY_SIMPLEX, mf, m.color, 1, cv::LINE_AA);
            cur_y += 24;
        }
        cur_y += 8;
    }

    // Calibration params (always shown)
    {
        char buf[32];
        auto& p = svc_->state.params;
        snprintf(buf, sizeof(buf), "%.0f", p.min_area);        std::string a_v = buf;
        snprintf(buf, sizeof(buf), "%.2f", p.min_spike_ratio); std::string s_v = buf;
        snprintf(buf, sizeof(buf), "%d",   p.threshold);       std::string t_v = buf;
        cv::line(canvas, {rpx+16, cur_y-6}, {W-14, cur_y-6}, Theme::DIVIDER, 1);
        cv::putText(canvas, "CONFIG", {rpx+18, cur_y+10},
                    cv::FONT_HERSHEY_SIMPLEX, 0.30, Theme::TXT_DIM, 1, cv::LINE_AA);
        cur_y += 28;
        struct PL { std::string lbl, val; };
        PL params[] = {{"Area", a_v}, {"Spike", s_v}, {"Thr", t_v}};
        for (auto& pp : params) {
            cv::putText(canvas, pp.lbl, {rpx+18, cur_y},
                        cv::FONT_HERSHEY_SIMPLEX, 0.34, Theme::TXT_DIM, 1, cv::LINE_AA);
            int bl;
            auto vsz = cv::getTextSize(pp.val, cv::FONT_HERSHEY_SIMPLEX, 0.34, 1, &bl);
            cv::putText(canvas, pp.val, {W-vsz.width-8, cur_y},
                        cv::FONT_HERSHEY_SIMPLEX, 0.34, Theme::TXT2, 1, cv::LINE_AA);
            cur_y += 20;
        }
    }
}

// ── Main draw ─────────────────────────────────────────
cv::Mat TouchHMI::draw(const cv::Mat& frame, const InspectionResult& result) {
    cv::Mat canvas(H, W, CV_8UC3, Theme::BG);

    int cy = BAR_H, ch = H - BAR_H - BTN_BAR;
    int result_panel_w = 158;
    int video_cw = W - result_panel_w;
    cv::rectangle(canvas, {0, cy}, {video_cw, cy+ch}, Theme::darken(Theme::BG, 0.82), -1);

    cv::Mat frame_vis;
    if (!frame.empty()) {
        if      (frame.channels() == 1) cv::cvtColor(frame, frame_vis, cv::COLOR_GRAY2BGR);
        else if (frame.channels() == 3) frame_vis = frame;
        else if (frame.channels() == 4) cv::cvtColor(frame, frame_vis, cv::COLOR_BGRA2BGR);
    }

    int xo = 0, yo = cy, nw = video_cw, nh = ch;
    int fw = frame_vis.cols, fh = frame_vis.rows;
    if (fw > 0 && fh > 0) {
        double scale = std::min((double)video_cw / fw, (double)ch / fh);
        nw = (int)(fw * scale); nh = (int)(fh * scale);
        if (nw > 0 && nh > 0) {
            cv::Mat scaled;
            cv::resize(frame_vis, scaled, {nw, nh}, 0, 0,
                       scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
            xo = (video_cw - nw) / 2;
            yo = cy + (ch - nh) / 2;
            scaled.copyTo(canvas(cv::Rect(xo, yo, nw, nh)));
            cv::rectangle(canvas, {xo-1, yo-1}, {xo+nw+1, yo+nh+1}, Theme::DIVIDER_GLOW, 1);
        }
    } else {
        put_centered_text(canvas, "WAITING CAMERA", 0, cy, video_cw, ch,
                          0.72, Theme::TXT_DIM, 2);
    }

    // Right result panel
    draw_result_panel(canvas, W - result_panel_w, BAR_H, H - BAR_H - BTN_BAR, result);

    // Status bar, button bar, FPS
    draw_status_bar(canvas, result);
    draw_btn_bar(canvas);
    for (auto& b : buttons()) draw_btn(canvas, b);
    draw_fps(canvas);
    if (password_active_) draw_password_modal(canvas);
    return canvas;
}

void TouchHMI::open_password_prompt() {
    password_active_ = true;
    password_input_.clear();
    password_error_until_ = 0;
}

bool TouchHMI::check_password() const {
    const char* env = std::getenv("HMI_CALIB_PASSWORD");
    std::string expected = (env && *env) ? env : "1234";
    return password_input_ == expected;
}

void TouchHMI::draw_password_modal(cv::Mat& canvas) {
    cv::Mat overlay(canvas.size(), canvas.type(), cv::Scalar(0, 0, 0));
    cv::addWeighted(overlay, 0.55, canvas, 0.45, 0, canvas);

    int mw = 390, mh = 455;
    int mx = (W - mw) / 2, my = 62;
    Draw::rrect(canvas, mx, my, mw, mh, Theme::BG_PANEL, 5);
    cv::rectangle(canvas, {mx, my}, {mx+mw, my+mh}, Theme::DIVIDER_GLOW, 1);
    cv::rectangle(canvas, {mx, my}, {mx+mw, my+5}, Theme::CYAN, -1);

    cv::putText(canvas, "MAT KHAU CAN CHINH", {mx+42, my+38},
                cv::FONT_HERSHEY_SIMPLEX, 0.68, Theme::CYAN, 2, cv::LINE_AA);

    std::string stars(password_input_.size(), '*');
    Draw::rrect(canvas, mx+45, my+60, mw-90, 48, Theme::BG_CARD, 3);
    int bl;
    auto psz = cv::getTextSize(stars, cv::FONT_HERSHEY_SIMPLEX, 0.9, 2, &bl);
    cv::putText(canvas, stars, {mx+(mw-psz.width)/2, my+92},
                cv::FONT_HERSHEY_SIMPLEX, 0.9, Theme::TXT, 2, cv::LINE_AA);

    if (now_sec() < password_error_until_) {
        cv::putText(canvas, "SAI MAT KHAU", {mx+120, my+128},
                    cv::FONT_HERSHEY_SIMPLEX, 0.48, Theme::RED, 2, cv::LINE_AA);
    }

    const char* labels[12] = {"1","2","3","4","5","6","7","8","9","DEL","0","OK"};
    int bw = 84, bh = 48, gap = 12;
    int sx = mx + (mw - (bw * 3 + gap * 2)) / 2;
    int sy = my + 154;
    for (int i = 0; i < 12; i++) {
        int col = i % 3, row = i / 3;
        int x = sx + col * (bw + gap);
        int y = sy + row * (bh + gap);
        std::string label = labels[i];
        cv::Scalar c = (label == "OK") ? Theme::BTN_PLUS
                     : (label == "DEL" ? Theme::BTN_MINUS : Theme::BTN_PARAM);
        Draw::rrect(canvas, x, y, bw, bh, Theme::BG_CARD, 4);
        cv::rectangle(canvas, {x, y}, {x+bw, y+bh}, Theme::DIVIDER, 1);
        cv::rectangle(canvas, {x, y}, {x+4, y+bh}, c, -1);
        auto sz = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX,
                                  label.size() > 1 ? 0.56 : 0.82, 2, &bl);
        cv::putText(canvas, label, {x+(bw-sz.width)/2, y+(bh+sz.height)/2},
                    cv::FONT_HERSHEY_SIMPLEX, label.size() > 1 ? 0.56 : 0.82,
                    Theme::TXT, 2, cv::LINE_AA);
    }

    int cancel_x = mx + 46, cancel_y = my + mh - 42, cancel_w = mw - 92, cancel_h = 34;
    Draw::rrect(canvas, cancel_x, cancel_y, cancel_w, cancel_h, Theme::BTN_RESET, 4);
    put_centered_text(canvas, "HUY", cancel_x, cancel_y, cancel_w, cancel_h,
                      0.52, Theme::TXT, 2);
}

bool TouchHMI::handle_password_touch(int x, int y) {
    if (!password_active_) return false;

    int mw = 390, mh = 455;
    int mx = (W - mw) / 2, my = 62;
    int cancel_x = mx + 46, cancel_y = my + mh - 42, cancel_w = mw - 92, cancel_h = 34;
    if (x >= cancel_x && x <= cancel_x + cancel_w &&
        y >= cancel_y && y <= cancel_y + cancel_h) {
        password_active_ = false;
        password_input_.clear();
        return true;
    }

    const char* labels[12] = {"1","2","3","4","5","6","7","8","9","DEL","0","OK"};
    int bw = 84, bh = 48, gap = 12;
    int sx = mx + (mw - (bw * 3 + gap * 2)) / 2;
    int sy = my + 154;
    for (int i = 0; i < 12; i++) {
        int col = i % 3, row = i / 3;
        int bx = sx + col * (bw + gap);
        int by = sy + row * (bh + gap);
        if (x < bx || x > bx + bw || y < by || y > by + bh) continue;

        std::string label = labels[i];
        if (label == "DEL") {
            if (!password_input_.empty()) password_input_.pop_back();
        } else if (label == "OK") {
            if (check_password()) {
                password_active_ = false;
                password_input_.clear();
                svc_->state.calibration_mode = true;
            } else {
                password_input_.clear();
                password_error_until_ = now_sec() + 1.5;
            }
        } else if (password_input_.size() < 8) {
            password_input_ += label;
        }
        return true;
    }
    return true;
}

void TouchHMI::handle_key(int key) {
    if (!password_active_) return;
    if (key >= '0' && key <= '9') {
        if (password_input_.size() < 8) password_input_.push_back((char)key);
    } else if (key == 8 || key == 127) {
        if (!password_input_.empty()) password_input_.pop_back();
    } else if (key == 13 || key == '\n') {
        if (check_password()) {
            password_active_ = false;
            password_input_.clear();
            svc_->state.calibration_mode = true;
        } else {
            password_input_.clear();
            password_error_until_ = now_sec() + 1.5;
        }
    } else if (key == 27) {
        password_active_ = false;
        password_input_.clear();
    }
}

void TouchHMI::handle(const std::string& name) {
    auto& st = svc_->state;
    if      (name == "Can Chinh")   { open_password_prompt(); }
    else if (name == "<- Quay Lai") {
        st.calibration_mode = false;
        st.last_label = "READY";
        st.last_color = cv::Scalar(255,255,0);
        svc_->capture_fps_.store(0);
        svc_->capture_ms_.store(0);
        result_label_ = "READY";
    }
    else if (name == "Reset" || name == "Reset Dem") st.reset_counters();
    else if (name == "Reset Ts")   { st.params.reset();         st.save_state(); }
    else if (name == "Thresh")     st.params.show_thresh = !st.params.show_thresh;
    else if (name == "P1")         st.params.selected_param = 0;
    else if (name == "P2")         st.params.selected_param = 1;
    else if (name == "P3")         st.params.selected_param = 2;
    else if (name == "+")          { st.params.adjust(true);    st.save_state(); }
    else if (name == "-")          { st.params.adjust(false);   st.save_state(); }
    else if (name == "Thoat")      svc_->running_ = false;
    else if (name == "Den")        svc_->gpio.toggle_light();
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
            hmi_.held_ = "";
        }
        return;
    }

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
            if (b.pressed && b.name == "Thoat" && now - b.pressed_since >= 2.0) {
                hmi_.handle(b.name);
            }
        }
        for (auto& b : hmi_.normal_) b.pressed = false;
        for (auto& b : hmi_.calib_) b.pressed = false;
        hmi_.held_ = "";
    }
}

std::string TouchApp::run_waiting_state() {
    InspectionResult idle;
    idle.label = vision_.state.last_label;
    vision_.heartbeat();

    double now = now_sec();
    if (vision_.should_render_waiting_frame(now)) {
        cv::Mat canvas = hmi_.draw(vision_.last_result_frame(), idle);
        cv::imshow("HMI", canvas);
    }

    int key = cv::waitKey(1) & 0xFF;
    hmi_.handle_key(key);
    if (hmi_.password_active()) return "";
    if      (key == 'q') return "quit";
    else if (key == 'l') vision_.gpio.toggle_light();
    else if (key == 'm') {
        cv::setWindowProperty("HMI", cv::WND_PROP_FULLSCREEN, cv::WINDOW_NORMAL);
        cv::resizeWindow("HMI", 640, 380);
    }
    else if (key == 'f')
        cv::setWindowProperty("HMI", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
    return "";
}

std::string TouchApp::run_calibration_mode() {
    while (vision_.state.calibration_mode && vision_.running_) {
        vision_.heartbeat();
        double now = now_sec();

        // Hold repeat for +/-
        if (hmi_.held_ == "+" || hmi_.held_ == "-") {
            if (now - hmi_.held_t_ >= hmi_.repeat_) {
                vision_.state.params.adjust(hmi_.held_ == "+");
                vision_.state.save_state();   // persist tuned params to disk (matches v1)
                hmi_.held_t_ = now;
            }
        }

        InspectionResult result = vision_.process_frame_for_calibration();
        cv::Mat canvas = hmi_.draw(result.roi_vis, result);
        cv::imshow("HMI", canvas);

        int key = cv::waitKey(1) & 0xFF;
        hmi_.handle_key(key);
        if (hmi_.password_active()) continue;
        if      (key == 'q') return "quit";
        else if (key == 'l') vision_.gpio.toggle_light();
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
    InspectionResult result = vision_.process_trigger(trigger_time);
    cv::Mat canvas = hmi_.draw(result.roi_vis, result);
    cv::imshow("HMI", canvas);
    cv::waitKey(1);
    return "";
}

void TouchApp::run() {
    log_msg(LOG_WARNING, "TouchApp started");
    try {
        while (vision_.running_) {
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
    vision_.running_ = false;
}

// Entry point is in src/main.cpp — do NOT add main() here.
