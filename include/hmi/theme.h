// -*- coding: utf-8 -*-
// hmi/theme.h — Color palette & drawing primitives for TouchHMI
// Industrial Premium Dark Theme (BGR color space for OpenCV)
#pragma once

#include <opencv2/opencv.hpp>
#include <algorithm>

struct Theme {
    // ─── Backgrounds ─────────────────────────
    static inline const cv::Scalar BG{18, 22, 26};
    static inline const cv::Scalar BG_PANEL{30, 35, 42};
    static inline const cv::Scalar BG_CARD{45, 52, 62};

    // ─── Accents ─────────────────────────────
    static inline const cv::Scalar CYAN{230, 178, 72};
    static inline const cv::Scalar GREEN{68, 204, 112};
    static inline const cv::Scalar RED{70, 82, 224};
    static inline const cv::Scalar YELLOW{45, 194, 230};
    static inline const cv::Scalar ORANGE{45, 132, 230};

    // ─── Text ────────────────────────────────
    static inline const cv::Scalar TXT{235, 240, 246};
    static inline const cv::Scalar TXT2{185, 194, 204};
    static inline const cv::Scalar TXT_DIM{145, 156, 168};

    // ─── Buttons ─────────────────────────────
    static inline const cv::Scalar BTN_CHECK{230, 178, 72};
    static inline const cv::Scalar BTN_BACK{205, 132, 70};
    static inline const cv::Scalar BTN_RESET{45, 52, 62};
    static inline const cv::Scalar BTN_EXIT{70, 82, 224};
    static inline const cv::Scalar BTN_THRESH{230, 178, 72};
    static inline const cv::Scalar BTN_PARAM{45, 52, 62};
    static inline const cv::Scalar BTN_PARAM_S{230, 178, 72};
    static inline const cv::Scalar BTN_PLUS{68, 204, 112};
    static inline const cv::Scalar BTN_MINUS{70, 82, 224};

    // ─── Status panels ───────────────────────
    static inline const cv::Scalar STATUS_OK{25, 66, 30};
    static inline const cv::Scalar STATUS_NG{42, 30, 84};
    static inline const cv::Scalar STATUS_WAIT{38, 58, 68};

    // ─── Dividers ────────────────────────────
    static inline const cv::Scalar DIVIDER{68, 76, 88};
    static inline const cv::Scalar DIVIDER_GLOW{76, 84, 94};

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
