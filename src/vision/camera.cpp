// -*- coding: utf-8 -*-
// vision/camera.cpp — LibcameraCapture (Argus / libcamera / V4L2)
#include "vision/camera.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/time_utils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using core::sleep_sec;

// ── Color correction — match Pi libcamera image quality ──
static cv::Mat color_correct_frame(const cv::Mat &frame) {
  if (frame.empty())
    return frame;
  constexpr double B = DetectionConfig::CC_BRIGHTNESS;
  constexpr double C = DetectionConfig::CC_CONTRAST;
  constexpr double S = DetectionConfig::CC_SATURATION;
  if (C == 1.0 && B == 0.0 && S == 1.0)
    return frame;

  cv::Mat result;
  if (C != 1.0 || B != 0.0) {
    frame.convertTo(result, -1, C, B);
  } else {
    result = frame;
  }
  if (S != 1.0) {
    cv::Mat gray, gray3ch;
    cv::cvtColor(result, gray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(gray, gray3ch, cv::COLOR_GRAY2BGR);
    cv::addWeighted(result, S, gray3ch, 1.0 - S, 0, result);
  }
  return result;
}

LibcameraCapture::LibcameraCapture() {}
LibcameraCapture::~LibcameraCapture() { close(); }

bool LibcameraCapture::start() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  if (running_)
    return true;

  int sensor_id = 0;
  if (const char *env = std::getenv("JETSON_CAM_SENSOR_ID")) {
    int parsed = std::atoi(env);
    if (parsed >= 0 && parsed <= 3)
      sensor_id = parsed;
  }

  auto has_valid_frame = [this]() -> bool {
    for (int i = 0; i < 12; i++) {
      cv::Mat f;
      if (cap_.grab() && cap_.retrieve(f) && !f.empty())
        return true;
      sleep_sec(0.03);
    }
    return false;
  };

  // ISP tuning for inspection: keep exposure/color neutral and preserve edges.
  char argus_pipeline[1024];
  std::snprintf(argus_pipeline, sizeof(argus_pipeline),
                "nvarguscamerasrc sensor-id=%d "
                "tnr-mode=1 tnr-strength=0.15 "
                "ee-mode=1 ee-strength=0.75 "
                "exposurecompensation=0.0 "
                "saturation=1.0 "
                "wbmode=1 ! "
                "video/x-raw(memory:NVMM),width=1920,height=1080,framerate=60/"
                "1,format=NV12 ! "
                "nvvidconv flip-method=2 ! video/x-raw,format=BGRx ! "
                "videoconvert ! video/x-raw,format=BGR ! "
                "queue max-size-buffers=1 leaky=downstream ! "
                "appsink max-buffers=1 drop=true sync=false",
                sensor_id);

  const std::vector<std::pair<const char *, std::string>> pipelines = {
      {"Argus", argus_pipeline},
      {"libcamera", "libcamerasrc ! "
                    "video/x-raw,width=1920,height=1080,framerate=30/1 ! "
                    "videoconvert ! video/x-raw,format=BGR ! "
                    "queue max-size-buffers=1 leaky=downstream ! "
                    "appsink max-buffers=1 drop=true sync=false"},
  };

  for (const auto &p : pipelines) {
    cap_.open(p.second, cv::CAP_GSTREAMER);
    if (!cap_.isOpened())
      continue;
    if (has_valid_frame()) {
      log_msg(LOG_WARNING, "Camera started via %s (sensor-id=%d)", p.first,
              sensor_id);
      running_ = true;
      return true;
    }
    log_msg(LOG_WARNING, "%s pipeline opened but no valid frames, fallback...",
            p.first);
    cap_.release();
  }

  // Fallback: V4L2 (USB cams or no Argus)
  log_msg(LOG_WARNING, "GStreamer pipelines failed, trying V4L2...");
  cap_.open(0, cv::CAP_V4L2);
  if (cap_.isOpened()) {
    cap_.set(cv::CAP_PROP_FRAME_WIDTH, 1920);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);
    cap_.set(cv::CAP_PROP_FPS, 60);
    cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);
    if (!has_valid_frame())
      cap_.release();
  }

  if (!cap_.isOpened()) {
    log_msg(LOG_ERROR, "Cannot open camera");
    return false;
  }
  running_ = true;
  log_msg(LOG_WARNING, "Camera started via V4L2: 1920x1080");
  return true;
}

void LibcameraCapture::stop() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  if (cap_.isOpened())
    cap_.release();
  running_ = false;
}

void LibcameraCapture::close() { stop(); }

cv::Mat LibcameraCapture::capture() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  cv::Mat frame;
  if (cap_.isOpened()) {
    if (cap_.grab()) {
      cap_.retrieve(frame);
      frame = color_correct_frame(frame);
    }
  }
  return frame;
}
