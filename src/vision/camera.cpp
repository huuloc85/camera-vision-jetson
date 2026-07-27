// -*- coding: utf-8 -*-
// vision/camera.cpp — LibcameraCapture (Argus / libcamera / V4L2)
#include "vision/camera.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/time_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using core::sleep_sec;

static int env_int(const char *name, int fallback)
{
  const char *value = std::getenv(name);
  if (!value || !value[0])
    return fallback;
  char *end = nullptr;
  long parsed = std::strtol(value, &end, 10);
  return end && *end == '\0' ? static_cast<int>(parsed) : fallback;
}

static std::string env_string(const char *name, const std::string &fallback)
{
  const char *value = std::getenv(name);
  return value && value[0] ? std::string(value) : fallback;
}

static bool backend_matches(const std::string &selected, const char *backend)
{
  return selected == "auto" || selected == backend ||
         (selected == "usb" && std::string(backend) == "v4l2");
}

static std::string argus_controls_from_env()
{
  std::string controls;
  const int max_exposure_us = env_int(
      "JETSON_CAM_MAX_EXPOSURE_US", env_int("OPENCV_TAIL_CAMERA_MAX_EXPOSURE_US", 0));
  if (max_exposure_us > 0)
  {
    int min_exposure_us = env_int(
        "JETSON_CAM_MIN_EXPOSURE_US", env_int("OPENCV_TAIL_CAMERA_MIN_EXPOSURE_US", 100));
    if (min_exposure_us <= 0 || min_exposure_us > max_exposure_us)
      min_exposure_us = std::min(100, max_exposure_us);
    controls += " exposuretimerange=\"" + std::to_string(min_exposure_us * 1000) +
                " " + std::to_string(max_exposure_us * 1000) + "\"";
  }

  const int max_gain = env_int(
      "JETSON_CAM_MAX_GAIN", env_int("OPENCV_TAIL_CAMERA_MAX_GAIN", 0));
  if (max_gain > 0)
    controls += " gainrange=\"1 " + std::to_string(max_gain) + "\"";

  return controls;
}

LibcameraCapture::LibcameraCapture() {}
LibcameraCapture::~LibcameraCapture() { close(); }

bool LibcameraCapture::start()
{
  std::lock_guard<std::mutex> lock(capture_mutex_);
  if (running_)
    return true;

  int sensor_id = env_int("JETSON_CAM_SENSOR_ID", 0);
  if (sensor_id < 0 || sensor_id > 3)
    sensor_id = 0;

  const std::string backend = env_string(
      "JETSON_CAMERA_BACKEND", env_string("OPENCV_TAIL_CAMERA_BACKEND", "auto"));
  const int v4l2_device = env_int("JETSON_CAM_DEVICE", env_int("OPENCV_TAIL_CAMERA_INDEX", 0));
  const int width = env_int("JETSON_CAM_WIDTH",
                            env_int("OPENCV_TAIL_CAMERA_WIDTH", DetectionConfig::CAMERA_FRAME_WIDTH));
  const int height = env_int("JETSON_CAM_HEIGHT",
                             env_int("OPENCV_TAIL_CAMERA_HEIGHT", DetectionConfig::CAMERA_FRAME_HEIGHT));
  const int fps = env_int("JETSON_CAM_FPS",
                          env_int("OPENCV_TAIL_CAMERA_FPS", DetectionConfig::CAMERA_FPS));
  log_msg(LOG_WARNING, "Camera config: backend=%s sensor-id=%d v4l2=/dev/video%d %dx%d@%d",
          backend.c_str(), sensor_id, v4l2_device, width, height, fps);

  auto has_valid_frame = [this]() -> bool
  {
    for (int i = 0; i < 12; i++)
    {
      cv::Mat f;
      if (cap_.grab() && cap_.retrieve(f) && !f.empty())
        return true;
      sleep_sec(0.03);
    }
    return false;
  };

  const std::string argus_controls = argus_controls_from_env();
  if (!argus_controls.empty())
    log_msg(LOG_WARNING, "Camera Argus controls:%s", argus_controls.c_str());

  char argus_pipeline[1200];
  std::snprintf(argus_pipeline, sizeof(argus_pipeline),
                "nvarguscamerasrc sensor-id=%d%s ! "
                "video/x-raw(memory:NVMM),width=%d,height=%d,framerate=%d/1,"
                "format=NV12 ! "
                "nvvidconv ! video/x-raw,format=BGRx ! "
                "videoconvert ! video/x-raw,format=BGR ! "
                "queue max-size-buffers=1 leaky=downstream ! "
                "appsink max-buffers=1 drop=true sync=false",
                sensor_id, argus_controls.c_str(), width, height, fps);

  const std::string libcamera_pipeline =
      "libcamerasrc ! "
      "video/x-raw,width=" + std::to_string(width) +
      ",height=" + std::to_string(height) +
      ",framerate=" + std::to_string(fps) + "/1 ! "
      "videoconvert ! video/x-raw,format=BGR ! "
      "queue max-size-buffers=1 leaky=downstream ! "
      "appsink max-buffers=1 drop=true sync=false";

  const std::vector<std::pair<const char *, std::string>> pipelines = {
      {"Argus", argus_pipeline},
      {"libcamera", libcamera_pipeline},
  };

  for (const auto &p : pipelines)
  {
    if (!backend_matches(backend, p.first == std::string("Argus") ? "argus" : "libcamera"))
      continue;
    cap_.open(p.second, cv::CAP_GSTREAMER);
    if (!cap_.isOpened())
      continue;
    if (has_valid_frame())
    {
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
  if (!backend_matches(backend, "v4l2"))
  {
    log_msg(LOG_ERROR, "Cannot open camera with backend=%s", backend.c_str());
    return false;
  }

  cap_.open(v4l2_device, cv::CAP_V4L2);
  if (cap_.isOpened())
  {
    cap_.set(cv::CAP_PROP_FRAME_WIDTH, width);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, height);
    cap_.set(cv::CAP_PROP_FPS, fps);
    cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);

    if (has_valid_frame())
    {
      running_ = true;
      log_msg(LOG_WARNING, "Camera started via V4L2: /dev/video%d %dx%d@%d",
              v4l2_device, width, height, fps);
      return true;
    }
    cap_.release();
  }

  log_msg(LOG_ERROR, "Cannot open camera");
  return false;
}

void LibcameraCapture::stop()
{
  std::lock_guard<std::mutex> lock(capture_mutex_);
  if (cap_.isOpened())
    cap_.release();
  running_ = false;
}

void LibcameraCapture::close() { stop(); }

cv::Mat LibcameraCapture::capture()
{
  std::lock_guard<std::mutex> lock(capture_mutex_);
  cv::Mat frame;
  if (cap_.isOpened())
  {
    if (cap_.grab())
    {
      cap_.retrieve(frame);
    }
  }
  return frame;
}
