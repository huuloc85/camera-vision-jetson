// -*- coding: utf-8 -*-
// vision/camera.h — LibcameraCapture / GStreamer capture wrapper
#pragma once

#include <opencv2/opencv.hpp>
#include <atomic>
#include <mutex>

// ══════════════════════════════════════════════════════
// LibcameraCapture — Argus / libcamera / V4L2 fallback
// ══════════════════════════════════════════════════════
class LibcameraCapture {
public:
    LibcameraCapture();
    ~LibcameraCapture();

    bool start();
    void stop();
    void close();
    cv::Mat capture();
    bool grab_only();
    cv::Mat retrieve_grabbed();
    int discard_frames(int count);
    bool is_running() const { return running_; }

private:
    std::atomic<bool> running_{false};
    cv::VideoCapture cap_;
    std::mutex capture_mutex_;
};
