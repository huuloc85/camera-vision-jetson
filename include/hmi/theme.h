// -*- coding: utf-8 -*-
// hmi/theme.h — Color palette & drawing primitives for TouchHMI
// Industrial Premium Dark Theme (BGR color space for OpenCV)
#pragma once

#include <opencv2/opencv.hpp>
#include <algorithm>

struct Theme {
    // ─── Backgrounds ─────────────────────────
    static inline const cv::Scalar BG{18, 20, 22};
    static inline const cv::Scalar BG_PANEL{27, 30, 34};
    static inline const cv::Scalar BG_CARD{38, 42, 48};

    // ─── Accents ─────────────────────────────
    static inline const cv::Scalar CYAN{205, 185, 55};
    static inline const cv::Scalar GREEN{80, 205, 95};
    static inline const cv::Scalar RED{70, 72, 225};
    static inline const cv::Scalar YELLOW{48, 190, 238};
    static inline const cv::Scalar ORANGE{45, 132, 230};

    // ─── Text ────────────────────────────────
    static inline const cv::Scalar TXT{235, 238, 244};
    static inline const cv::Scalar TXT2{162, 170, 180};
    static inline const cv::Scalar TXT_DIM{92, 98, 108};

    // ─── Buttons ─────────────────────────────
    static inline const cv::Scalar BTN_CHECK{170, 132, 36};
    static inline const cv::Scalar BTN_BACK{60, 150, 65};
    static inline const cv::Scalar BTN_RESET{64, 68, 78};
    static inline const cv::Scalar BTN_EXIT{58, 52, 185};
    static inline const cv::Scalar BTN_THRESH{166, 112, 54};
    static inline const cv::Scalar BTN_PARAM{57, 62, 72};
    static inline const cv::Scalar BTN_PARAM_S{190, 148, 42};
    static inline const cv::Scalar BTN_PLUS{48, 158, 58};
    static inline const cv::Scalar BTN_MINUS{38, 102, 198};

    // ─── Status panels ───────────────────────
    static inline const cv::Scalar STATUS_OK{25, 66, 30};
    static inline const cv::Scalar STATUS_NG{42, 30, 84};
    static inline const cv::Scalar STATUS_WAIT{38, 58, 68};

    // ─── Light button ────────────────────────
    static inline const cv::Scalar BTN_LIGHT_ON{25, 160, 42};
    static inline const cv::Scalar BTN_LIGHT_OFF{48, 44, 58};

    // ─── Dividers ────────────────────────────
    static inline const cv::Scalar DIVIDER{54, 59, 68};
    static inline const cv::Scalar DIVIDER_GLOW{77, 84, 96};

    // ─── Color helpers ────────────────────────
    static cv::Scalar darken(cv::Scalar c, double f = 0.55) {
        return cv::Scalar(c[0]*f, c[1]*f, c[2]*f);
    }
    static cv::Scalar lighten(cv::Scalar c, int a = 28) {
        return cv::Scalar(
            std::min(255.0, c[0]+a),
            std::min(255.0, c[1]+a),
            std::min(255.0, c[2]+a));
    }
    static cv::Scalar blend(cv::Scalar a, cv::Scalar b, double t) {
        return cv::Scalar(
            a[0]*(1-t) + b[0]*t,
            a[1]*(1-t) + b[1]*t,
            a[2]*(1-t) + b[2]*t);
    }
};

// ══════════════════════════════════════════════════════
// Drawing Primitives — shared across all HMI screens
// ══════════════════════════════════════════════════════
struct Draw {
    static void rrect(cv::Mat& img, int x, int y, int w, int h,
                      cv::Scalar color, int r = 8);
};
