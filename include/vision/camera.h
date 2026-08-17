// -*- coding: utf-8 -*-
// vision/camera.h — Hikrobot MVS GigE camera wrapper
#pragma once

#include <opencv2/core.hpp>
#include <atomic>
#include <cstdint>
#include <mutex>

class MvsCamera {
public:
    MvsCamera();
    ~MvsCamera();

    bool start();
    void stop();
    void close();
    cv::Mat capture(std::uint32_t* frame_number = nullptr);
    int discard_frames(int count);
    bool is_running() const { return running_; }

private:
    std::atomic<bool> running_{false};
    std::mutex capture_mutex_;
    void *mvs_handle_{nullptr};
};
