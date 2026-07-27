// -*- coding: utf-8 -*-
// vision/image_processor.cpp — ROI warp, preprocess, metrics
#include "vision/image_processor.h"
#include "core/logger.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

static std::vector<cv::Point2f> default_roi_points()
{
  return {{604, 421}, {1327, 403}, {1338, 943}, {595, 973}};
}

static int env_int(const char *name, int fallback)
{
  const char *value = std::getenv(name);
  if (!value || !value[0]) return fallback;
  char *end = nullptr;
  long parsed = std::strtol(value, &end, 10);
  return end && *end == '\0' ? static_cast<int>(parsed) : fallback;
}

static bool parse_roi_points_line(const std::string &line, std::vector<cv::Point2f> &points)
{
  std::string cleaned;
  cleaned.reserve(line.size());
  for (char ch : line)
  {
    if (ch == '#' || ch == ';') break;
    if (ch == ',' || ch == '[' || ch == ']' || ch == '(' || ch == ')')
      cleaned.push_back(' ');
    else
      cleaned.push_back(ch);
  }

  std::stringstream ss(cleaned);
  float x = 0.0f, y = 0.0f;
  while (ss >> x >> y)
    points.emplace_back(x, y);
  return points.size() == 4;
}

static bool load_roi_points_file(std::vector<cv::Point2f> &points)
{
  const char *env = std::getenv("JETSON_ROI_POINTS_FILE");
  const char *candidates[] = {
      env ? env : "",
      "config/roi_points.txt",
      "../config/roi_points.txt",
      "/home/vvp/jetson-inspect-v2/config/roi_points.txt",
      nullptr,
  };

  for (const char **path = candidates; *path; ++path)
  {
    if (!*path || !(*path)[0]) continue;
    std::ifstream ifs(*path);
    if (!ifs) continue;

    std::vector<cv::Point2f> loaded;
    std::string line;
    while (std::getline(ifs, line))
    {
      if (parse_roi_points_line(line, loaded))
      {
        points = loaded;
        return true;
      }
    }
  }
  return false;
}

// ── ImageProcessor ───────────────────────────────────
ImageProcessor::ImageProcessor(const DetectionParams &params)
    : params_(params) {
  morph_kernel_ = cv::getStructuringElement(cv::MORPH_ELLIPSE, {5, 5});
  morph_kernel_large_ = cv::getStructuringElement(cv::MORPH_ELLIPSE, {7, 7});
  morph_kernel_xl_ = cv::getStructuringElement(cv::MORPH_ELLIPSE, {9, 9});

  sharpen_kernel_ = (cv::Mat_<float>(3, 3) << 0, -1, 0, -1, 5, -1, 0, -1, 0);

  // Perspective transform: ROI points are calibrated on a 1920x1080 source.
  std::vector<cv::Point2f> roi_pts = default_roi_points();
  if (load_roi_points_file(roi_pts)) {
    log_msg(LOG_WARNING, "ROI config loaded from file: %s",
            std::getenv("JETSON_ROI_POINTS_FILE") ? std::getenv("JETSON_ROI_POINTS_FILE") : "config/roi_points.txt");
  }
  const int camera_width = env_int(
      "JETSON_CAM_WIDTH",
      env_int("OPENCV_TAIL_CAMERA_WIDTH", DetectionConfig::CAMERA_FRAME_WIDTH));
  const int camera_height = env_int(
      "JETSON_CAM_HEIGHT",
      env_int("OPENCV_TAIL_CAMERA_HEIGHT", DetectionConfig::CAMERA_FRAME_HEIGHT));
  const float sx = camera_width / 1920.0f;
  const float sy = camera_height / 1080.0f;
  for (auto &p : roi_pts) {
    p.x *= sx;
    p.y *= sy;
  }
  if (DetectionConfig::ROTATE_ROI_180 &&
      DetectionConfig::ROTATE_ROI_USING_SRC_REMAP) {
    for (auto &p : roi_pts) {
      p.x = (float)(DetectionConfig::CAMERA_FRAME_WIDTH - 1) - p.x;
      p.y = (float)(DetectionConfig::CAMERA_FRAME_HEIGHT - 1) - p.y;
    }
  }
  std::vector<cv::Point2f> dst_pts = {{0, 0}, {1080, 0}, {1080, 804}, {0, 804}};
  M_ = cv::getPerspectiveTransform(roi_pts, dst_pts);

  // Half-res: 540×402
  std::vector<cv::Point2f> dst_half;
  for (auto &p : dst_pts)
    dst_half.push_back(p * 0.5f);
  M_half_ = cv::getPerspectiveTransform(roi_pts, dst_half);

#if USE_CUDA_ACCEL
  if (cv::cuda::getCudaEnabledDeviceCount() > 0) {
    try {
      cv::cuda::setDevice(0);
      gpu_gaussian_ =
          cv::cuda::createGaussianFilter(CV_8UC1, CV_8UC1, {11, 11}, 2.5);
      gpu_morph_close_large_ = cv::cuda::createMorphologyFilter(
          cv::MORPH_CLOSE, CV_8UC1, morph_kernel_large_, cv::Point(-1, -1), 2);
      gpu_morph_open_xl_ = cv::cuda::createMorphologyFilter(
          cv::MORPH_OPEN, CV_8UC1, morph_kernel_xl_, cv::Point(-1, -1), 2);
      use_cuda_ = true;
      cv::cuda::DeviceInfo info(0);
      log_msg(LOG_WARNING, "CUDA ENABLED — GPU: %s (%d cores, %luMB)",
              info.name(), (int)info.multiProcessorCount(),
              (unsigned long)(info.totalMemory() / (1024 * 1024)));
    } catch (const cv::Exception &e) {
      log_msg(LOG_WARNING, "CUDA init failed: %s — CPU fallback", e.what());
      use_cuda_ = false;
    }
  } else {
    log_msg(LOG_WARNING, "No CUDA device — using CPU");
  }
#endif
}

cv::Mat ImageProcessor::sharpen(const cv::Mat &img) {
  cv::Mat result;
  cv::filter2D(img, result, -1, sharpen_kernel_);
  return result;
}

std::pair<cv::Mat, cv::Mat> ImageProcessor::preprocess(const cv::Mat &roi) {
  int t = std::max(0, std::min(250, params_.threshold));

#if USE_CUDA_ACCEL
  if (use_cuda_ && !DetectionConfig::EDGE_WHITE_ON_BLACK_MODE) {
    if (gpu_roi_cache_valid_) {
      gpu_roi_cache_valid_ = false;
    } else {
      gpu_roi_.upload(roi);
    }
    cv::cuda::cvtColor(gpu_roi_, gpu_gray_, cv::COLOR_BGR2GRAY);
    gpu_gaussian_->apply(gpu_gray_, gpu_blurred_);
    cv::cuda::threshold(gpu_blurred_, gpu_thresh_, t, 255, cv::THRESH_BINARY);
    gpu_morph_close_large_->apply(gpu_thresh_, gpu_thresh_);
    gpu_morph_open_xl_->apply(gpu_thresh_, gpu_thresh_);
    cv::Mat gray, thresh;
    gpu_gray_.download(gray);
    gpu_thresh_.download(thresh);
    return {gray, thresh};
  }
#endif

  cv::Mat gray, blurred, thresh;
  cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);
  cv::GaussianBlur(gray, blurred, {11, 11}, 2.5);

  if (DetectionConfig::EDGE_WHITE_ON_BLACK_MODE) {
    cv::Mat bin, edges, merged, filled;
    cv::threshold(blurred, bin, t, 255, cv::THRESH_BINARY);
    double fg_ratio = (double)cv::countNonZero(bin) / (double)bin.total();
    if (fg_ratio < 0.002 || fg_ratio > 0.95)
      cv::threshold(blurred, bin, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    int canny_low = std::max(15, t / 3);
    int canny_high = std::max(canny_low + 15, t);
    cv::Canny(blurred, edges, canny_low, canny_high);
    cv::dilate(edges, edges, morph_kernel_, cv::Point(-1, -1), 1);
    cv::bitwise_or(bin, edges, merged);
    cv::morphologyEx(merged, merged, cv::MORPH_CLOSE, morph_kernel_large_,
                     cv::Point(-1, -1), 2);
    filled = cv::Mat::zeros(merged.size(), CV_8UC1);
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(merged.clone(), contours, cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);
    for (const auto &c : contours)
      if (cv::contourArea(c) >= 150.0)
        cv::drawContours(filled, std::vector<std::vector<cv::Point>>{c}, -1,
                         cv::Scalar(255), cv::FILLED);
    cv::morphologyEx(filled, thresh, cv::MORPH_CLOSE, morph_kernel_,
                     cv::Point(-1, -1), 1);
  } else {
    cv::threshold(blurred, thresh, t, 255, cv::THRESH_BINARY);
    cv::morphologyEx(thresh, thresh, cv::MORPH_CLOSE, morph_kernel_large_,
                     cv::Point(-1, -1), 2);
    cv::morphologyEx(thresh, thresh, cv::MORPH_OPEN, morph_kernel_xl_,
                     cv::Point(-1, -1), 2);
  }
  return {gray, thresh};
}

cv::Mat ImageProcessor::warp_roi(const cv::Mat &frame) {
#if USE_CUDA_ACCEL
  if (use_cuda_) {
    gpu_frame_.upload(frame);
    cv::cuda::warpPerspective(gpu_frame_, gpu_roi_, M_, {1080, 804});
    if (DetectionConfig::ROTATE_ROI_180 &&
        !DetectionConfig::ROTATE_ROI_USING_SRC_REMAP)
      cv::cuda::flip(gpu_roi_, gpu_roi_, -1);
    gpu_roi_cache_valid_ = true;
    cv::Mat result;
    gpu_roi_.download(result);
    return result;
  }
#endif
  cv::Mat result;
  cv::warpPerspective(frame, result, M_, {1080, 804});
  if (DetectionConfig::ROTATE_ROI_180 &&
      !DetectionConfig::ROTATE_ROI_USING_SRC_REMAP)
    cv::rotate(result, result, cv::ROTATE_180);
  return result;
}

cv::Mat ImageProcessor::warp_roi_fast(const cv::Mat &frame) {
#if USE_CUDA_ACCEL
  if (use_cuda_) {
    gpu_frame_.upload(frame);
    cv::cuda::warpPerspective(gpu_frame_, gpu_roi_, M_half_, {540, 402});
    if (DetectionConfig::ROTATE_ROI_180 &&
        !DetectionConfig::ROTATE_ROI_USING_SRC_REMAP)
      cv::cuda::flip(gpu_roi_, gpu_roi_, -1);
    gpu_roi_cache_valid_ = true;
    cv::Mat result;
    gpu_roi_.download(result);
    return result;
  }
#endif
  cv::Mat result;
  cv::warpPerspective(frame, result, M_half_, {540, 402});
  if (DetectionConfig::ROTATE_ROI_180 &&
      !DetectionConfig::ROTATE_ROI_USING_SRC_REMAP)
    cv::rotate(result, result, cv::ROTATE_180);
  return result;
}

// ── Static helpers ────────────────────────────────────
std::tuple<double, int, int>
ImageProcessor::measure_spike(const std::vector<cv::Point> &contour,
                              const cv::Mat &thresh) {
  cv::Rect br = cv::boundingRect(contour);
  if (br.height < 30 || thresh.empty())
    return {0.0, 0, br.height};

  int y0 = std::max(0, br.y), y1 = std::min(thresh.rows, br.y + br.height);
  int x0 = std::max(0, br.x), x1 = std::min(thresh.cols, br.x + br.width);
  cv::Mat region = thresh(cv::Range(y0, y1), cv::Range(x0, x1));
  if (region.empty())
    return {0.0, 0, br.height};

  std::vector<int> row_widths;
  for (int r = 0; r < region.rows; r++) {
    int first = -1, last = -1;
    const uchar *row = region.ptr<uchar>(r);
    for (int c = 0; c < region.cols; c++) {
      if (row[c] > 0) {
        if (first < 0)
          first = c;
        last = c;
      }
    }
    row_widths.push_back((first >= 0 && last > first) ? last - first : 0);
  }
  if (row_widths.size() < 4)
    return {0.0, 0, br.height};

  int top_40 = std::max(4, (int)(row_widths.size() * 0.4));
  int neck_w = 0;
  for (int r = 0; r < top_40; r++)
    neck_w = std::max(neck_w, row_widths[r]);
  if (neck_w <= 0)
    return {0.0, 0, br.height};

  int spike_len = 0;
  for (int r = 0; r < (int)row_widths.size(); r++) {
    if (row_widths[r] < neck_w * DetectionConfig::SPIKE_CAP_ZONE)
      spike_len++;
    else
      break;
  }
  double ratio = std::round((double)spike_len / br.height * 1000.0) / 1000.0;
  return {ratio, spike_len, br.height};
}

ShapeMetrics
ImageProcessor::calculate_metrics(const std::vector<cv::Point> &contour, int x,
                                  int y, int w, int h, const cv::Mat &thresh) {
  ShapeMetrics m;
  m.area = cv::contourArea(contour);
  m.perimeter = cv::arcLength(contour, true);
  std::vector<cv::Point> hull;
  cv::convexHull(contour, hull);
  double hull_area = cv::contourArea(hull);
  m.solidity = hull_area > 0 ? m.area / hull_area : 0;
  m.width = w;
  m.height = h;

  int min_y = contour[0].y;
  for (auto &p : contour)
    min_y = std::min(min_y, p.y);

  int top_min_x = INT_MAX, top_max_x = INT_MIN, count = 0;
  for (auto &p : contour) {
    if (p.y <= min_y + 10) {
      top_min_x = std::min(top_min_x, p.x);
      top_max_x = std::max(top_max_x, p.x);
      count++;
    }
  }
  m.top_points = count;
  m.top_width = (count > 0) ? (top_max_x - top_min_x) : 0;
  m.top_width_ratio = (w > 0) ? (double)m.top_width / w : 0;

  auto [sr, smin, smax] = measure_spike(contour, thresh);
  m.spike_ratio = sr;
  m.spike_min_w = smin;
  m.spike_max_w = smax;
  return m;
}
