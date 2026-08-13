// -*- coding: utf-8 -*-
// vision/camera.cpp — LibcameraCapture (Argus / libcamera / V4L2)
#include "vision/camera.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/time_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using core::sleep_sec;

// Preserve the camera luminance and remove color for detection/HMI display.
// Keep three channels so existing colored contour overlays remain visible.
static cv::Mat to_monochrome_frame(const cv::Mat &frame) {
  if (frame.empty())
    return frame;
  if (frame.channels() == 1) {
    cv::Mat result;
    cv::cvtColor(frame, result, cv::COLOR_GRAY2BGR);
    return result;
  }
  if (frame.channels() != 3)
    return frame;

  cv::Mat gray, result;
  cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
  cv::cvtColor(gray, result, cv::COLOR_GRAY2BGR);
  return result;
}

static int env_int(const char *name, int fallback) {
  if (const char *env = std::getenv(name)) {
    int parsed = std::atoi(env);
    if (parsed > 0)
      return parsed;
  }
  return fallback;
}

struct CameraFocusConfig {
  bool automatic = DetectionConfig::CAMERA_FOCUS_AUTOMATIC;
  int absolute = DetectionConfig::CAMERA_FOCUS_ABSOLUTE;
};

static CameraFocusConfig load_camera_focus_config() {
  CameraFocusConfig config;
  const char *env_path = std::getenv("JETSON_INSPECT_CONFIG");
  const std::string path = (env_path && *env_path)
      ? env_path
      : "config/app_config.json";

  try {
    cv::FileStorage fs(path, cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON);
    if (!fs.isOpened()) {
      log_msg(LOG_WARNING,
              "Camera focus config not found: %s; using compiled defaults",
              path.c_str());
      return config;
    }

    const cv::FileNode camera = fs["camera"];
    const cv::FileNode automatic = camera["focus_automatic_continuous"];
    const cv::FileNode absolute = camera["focus_absolute"];
    if (!automatic.empty())
      config.automatic = static_cast<int>(automatic) != 0;
    if (!absolute.empty())
      config.absolute = static_cast<int>(absolute);
  } catch (const cv::Exception &e) {
    log_msg(LOG_WARNING, "Cannot read camera focus config %s: %s",
            path.c_str(), e.what());
  }
  return config;
}

static void apply_v4l2_focus_config(int camera_index) {
  const CameraFocusConfig config = load_camera_focus_config();

#if defined(__linux__)
  const std::string device = "/dev/video" + std::to_string(camera_index);
  const int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    log_msg(LOG_WARNING, "Cannot open %s to apply focus config: %s",
            device.c_str(), std::strerror(errno));
    return;
  }

  auto set_control = [&](unsigned int id, int value, const char *name) {
    v4l2_control control{};
    control.id = id;
    control.value = value;
    if (::ioctl(fd, VIDIOC_S_CTRL, &control) < 0) {
      log_msg(LOG_WARNING, "Camera control %s=%d failed: %s",
              name, value, std::strerror(errno));
      return false;
    }
    return true;
  };

  // Apply each control with its own ioctl. This matches cameras that reject
  // autofocus and absolute focus in one VIDIOC_S_EXT_CTRLS request.
  const bool automatic_ok = set_control(V4L2_CID_FOCUS_AUTO,
                                        config.automatic ? 1 : 0,
                                        "focus_automatic_continuous");
  bool absolute_ok = true;
  if (!config.automatic) {
    absolute_ok = set_control(V4L2_CID_FOCUS_ABSOLUTE, config.absolute,
                              "focus_absolute");
  }
  ::close(fd);

  if (automatic_ok && absolute_ok) {
    if (config.automatic) {
      log_msg(LOG_WARNING, "Camera focus config: autofocus=ON");
    } else {
      log_msg(LOG_WARNING, "Camera focus config: autofocus=OFF focus=%d",
              config.absolute);
    }
  }
#else
  (void)camera_index;
  log_msg(LOG_WARNING, "Camera focus config is only supported on V4L2/Linux");
#endif
}

static int requested_fourcc() {
  const char *env = std::getenv("JETSON_CAM_FOURCC");
  std::string fourcc = (env && *env) ? env : "MJPG";
  if (fourcc.size() != 4)
    return 0;
  return cv::VideoWriter::fourcc(fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
}

static std::string fourcc_string(double value) {
  int v = (int)value;
  char s[5] = {
      (char)(v & 0xFF),
      (char)((v >> 8) & 0xFF),
      (char)((v >> 16) & 0xFF),
      (char)((v >> 24) & 0xFF),
      0};
  for (int i = 0; i < 4; i++) {
    if (s[i] < 32 || s[i] > 126)
      s[i] = '?';
  }
  return std::string(s);
}

static void configure_v4l2(cv::VideoCapture &cap) {
  int width = env_int("JETSON_CAM_WIDTH", DetectionConfig::CAMERA_FRAME_WIDTH);
  int height = env_int("JETSON_CAM_HEIGHT", DetectionConfig::CAMERA_FRAME_HEIGHT);
  int fps = env_int("JETSON_CAM_FPS", DetectionConfig::CAMERA_FPS);
  int fourcc = requested_fourcc();
  if (fourcc != 0)
    cap.set(cv::CAP_PROP_FOURCC, fourcc);
  cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
  cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
  cap.set(cv::CAP_PROP_FPS, fps);
  cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
}

static void log_camera_mode(cv::VideoCapture &cap, const char *backend) {
  log_msg(LOG_WARNING, "%s mode: %.0fx%.0f @ %.0ffps fourcc=%s buffers=%.0f",
          backend,
          cap.get(cv::CAP_PROP_FRAME_WIDTH),
          cap.get(cv::CAP_PROP_FRAME_HEIGHT),
          cap.get(cv::CAP_PROP_FPS),
          fourcc_string(cap.get(cv::CAP_PROP_FOURCC)).c_str(),
          cap.get(cv::CAP_PROP_BUFFERSIZE));
}

LibcameraCapture::LibcameraCapture() {}
LibcameraCapture::~LibcameraCapture() { close(); }

bool LibcameraCapture::start() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  if (running_)
    return true;

  auto has_valid_frame = [this]() -> bool {
    for (int i = 0; i < 12; i++) {
      cv::Mat f;
      if (cap_.grab() && cap_.retrieve(f) && !f.empty())
        return true;
      sleep_sec(0.03);
    }
    return false;
  };

  int v4l2_index = 0;
  if (const char *env = std::getenv("JETSON_CAM_V4L2_INDEX")) {
    int parsed = std::atoi(env);
    if (parsed >= 0 && parsed <= 9)
      v4l2_index = parsed;
  }

  apply_v4l2_focus_config(v4l2_index);

  int width = env_int("JETSON_CAM_WIDTH", DetectionConfig::CAMERA_FRAME_WIDTH);
  int height = env_int("JETSON_CAM_HEIGHT", DetectionConfig::CAMERA_FRAME_HEIGHT);
  int fps = env_int("JETSON_CAM_FPS", DetectionConfig::CAMERA_FPS);

  // USB MJPG decode through OpenCV V4L2 is CPU-bound on Orin and measured
  // 66-102 ms per 1920x1080 frame. Try NVIDIA's hardware decoders first.
  // The appsink still returns the same full-resolution BGR frame expected by
  // the detection and HMI paths. Any negotiation failure falls back to V4L2.
  bool try_hw_mjpeg = true;
  if (const char *env = std::getenv("JETSON_CAM_HW_MJPEG"))
    try_hw_mjpeg = std::string(env) != "0";

  if (try_hw_mjpeg) {
    char nvv4l2_pipeline[1024];
    std::snprintf(nvv4l2_pipeline, sizeof(nvv4l2_pipeline),
                  "v4l2src device=/dev/video%d io-mode=2 ! "
                  "image/jpeg,width=%d,height=%d,framerate=%d/1 ! "
                  "nvv4l2decoder mjpeg=1 ! "
                  "video/x-raw(memory:NVMM),format=Y42B ! nvvidconv ! "
                  "video/x-raw,format=BGRx,width=%d,height=%d,framerate=%d/1 ! "
                  "videoconvert ! "
                  "video/x-raw,format=BGR,width=%d,height=%d,framerate=%d/1 ! "
                  "appsink name=appsink max-buffers=1 drop=true sync=false",
                  v4l2_index, width, height, fps,
                  width, height, fps,
                  width, height, fps);

    cap_.open(std::string(nvv4l2_pipeline), cv::CAP_GSTREAMER,
              {cv::CAP_PROP_OPEN_TIMEOUT_MSEC, 5000,
               cv::CAP_PROP_READ_TIMEOUT_MSEC, 100});
    if (cap_.isOpened() && has_valid_frame()) {
      running_ = true;
      log_msg(LOG_WARNING,
              "Camera started via HW-MJPEG nvv4l2decoder: /dev/video%d",
              v4l2_index);
      log_msg(LOG_WARNING,
              "HW-MJPEG mode: %dx%d @ %dfps Y42B->BGR grayscale buffers=1",
              width, height, fps);
      return true;
    }
    log_msg(LOG_WARNING, "HW-MJPEG nvv4l2decoder unavailable, fallback...");
    if (cap_.isOpened())
      cap_.release();
  }

  // USB cameras show up as /dev/videoN. Try V4L2 first to avoid slow Argus
  // failures on Jetsons without a CSI camera attached.
  cap_.open(v4l2_index, cv::CAP_V4L2);
  if (cap_.isOpened()) {
    configure_v4l2(cap_);
    if (has_valid_frame()) {
      running_ = true;
      log_msg(LOG_WARNING, "Camera started via V4L2: /dev/video%d", v4l2_index);
      log_camera_mode(cap_, "V4L2");
      return true;
    }
    log_msg(LOG_WARNING, "V4L2 /dev/video%d opened but no valid frames, fallback...",
            v4l2_index);
    cap_.release();
  }

  int sensor_id = 0;
  if (const char *env = std::getenv("JETSON_CAM_SENSOR_ID")) {
    int parsed = std::atoi(env);
    if (parsed >= 0 && parsed <= 3)
      sensor_id = parsed;
  }

  // Leave ISP image controls at the camera/driver defaults.
  char argus_pipeline[1024];
  std::snprintf(argus_pipeline, sizeof(argus_pipeline),
                "nvarguscamerasrc sensor-id=%d "
                "! "
                "video/x-raw(memory:NVMM),width=%d,height=%d,framerate=%d/"
                "1,format=NV12 ! "
                "nvvidconv flip-method=2 ! video/x-raw,format=BGRx ! "
                "videoconvert ! video/x-raw,format=BGR ! "
                "queue max-size-buffers=1 leaky=downstream ! "
                "appsink max-buffers=1 drop=true sync=false",
                sensor_id,
                env_int("JETSON_CAM_WIDTH", DetectionConfig::CAMERA_FRAME_WIDTH),
                env_int("JETSON_CAM_HEIGHT", DetectionConfig::CAMERA_FRAME_HEIGHT),
                env_int("JETSON_CAM_FPS", DetectionConfig::CAMERA_FPS));

  char libcamera_pipeline[512];
  std::snprintf(libcamera_pipeline, sizeof(libcamera_pipeline),
                "libcamerasrc ! "
                "video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
                "videoconvert ! video/x-raw,format=BGR ! "
                "queue max-size-buffers=1 leaky=downstream ! "
                "appsink max-buffers=1 drop=true sync=false",
                env_int("JETSON_CAM_WIDTH", DetectionConfig::CAMERA_FRAME_WIDTH),
                env_int("JETSON_CAM_HEIGHT", DetectionConfig::CAMERA_FRAME_HEIGHT),
                env_int("JETSON_CAM_FPS", DetectionConfig::CAMERA_FPS));

  const std::vector<std::pair<const char *, std::string>> pipelines = {
      {"Argus", argus_pipeline},
      {"libcamera", libcamera_pipeline},
  };

  for (const auto &p : pipelines) {
    cap_.open(p.second, cv::CAP_GSTREAMER);
    if (!cap_.isOpened())
      continue;
    if (has_valid_frame()) {
      log_msg(LOG_WARNING, "Camera started via %s (sensor-id=%d)", p.first,
              sensor_id);
      log_camera_mode(cap_, p.first);
      running_ = true;
      return true;
    }
    log_msg(LOG_WARNING, "%s pipeline opened but no valid frames, fallback...",
            p.first);
    cap_.release();
  }

  // Fallback: V4L2 (USB cams or no Argus)
  log_msg(LOG_WARNING, "GStreamer pipelines failed, trying V4L2...");
  cap_.open(v4l2_index, cv::CAP_V4L2);
  if (cap_.isOpened()) {
    configure_v4l2(cap_);
    if (!has_valid_frame())
      cap_.release();
  }

  if (!cap_.isOpened()) {
    log_msg(LOG_ERROR, "Cannot open camera");
    return false;
  }
  running_ = true;
  log_msg(LOG_WARNING, "Camera started via V4L2: /dev/video%d", v4l2_index);
  log_camera_mode(cap_, "V4L2");
  return true;
}

void LibcameraCapture::stop() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  try {
    if (cap_.isOpened())
      cap_.release();
  } catch (const cv::Exception &e) {
    log_msg(LOG_ERROR, "Camera stop exception: %s", e.what());
  } catch (const std::exception &e) {
    log_msg(LOG_ERROR, "Camera stop error: %s", e.what());
  } catch (...) {
    log_msg(LOG_ERROR, "Camera stop unknown error");
  }
  running_ = false;
}

void LibcameraCapture::close() { stop(); }

cv::Mat LibcameraCapture::capture() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  cv::Mat frame;
  try {
    if (cap_.isOpened()) {
      if (cap_.grab()) {
        cap_.retrieve(frame);
        frame = to_monochrome_frame(frame);
      }
    }
  } catch (const cv::Exception &e) {
    running_ = false;
    log_msg(LOG_ERROR, "Camera capture exception: %s", e.what());
  } catch (const std::exception &e) {
    running_ = false;
    log_msg(LOG_ERROR, "Camera capture error: %s", e.what());
  } catch (...) {
    running_ = false;
    log_msg(LOG_ERROR, "Camera capture unknown error");
  }
  return frame;
}

bool LibcameraCapture::grab_only() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  return cap_.isOpened() && cap_.grab();
}

cv::Mat LibcameraCapture::retrieve_grabbed() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  cv::Mat frame;
  if (cap_.isOpened() && cap_.retrieve(frame))
    frame = to_monochrome_frame(frame);
  return frame;
}

int LibcameraCapture::discard_frames(int count) {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  if (!cap_.isOpened() || count <= 0)
    return 0;

  int discarded = 0;
  while (discarded < count && cap_.grab())
    discarded++;
  return discarded;
}
