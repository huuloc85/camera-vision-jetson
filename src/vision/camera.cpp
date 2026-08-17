// -*- coding: utf-8 -*-
// vision/camera.cpp — Hikrobot GigE capture through MVS SDK only
#include "vision/camera.h"
#include "core/logger.h"

#include "MvCameraControl.h"

#include <opencv2/imgproc.hpp>

#include <cstddef>
#include <exception>
#include <cstdio>
#include <vector>

namespace {

void log_mvs_error(const char *operation, int ret) {
  log_msg(LOG_ERROR, "MVS %s failed: 0x%08x", operation,
          static_cast<unsigned int>(ret));
}

const char *mvs_error_name(int ret) {
  switch (static_cast<unsigned int>(ret)) {
    case 0x80000203:
      return "MV_E_ACCESS_DENIED (camera is occupied or access is unavailable)";
    case 0x80000204:
      return "MV_E_BUSY (camera busy or network disconnected)";
    case 0x80000206:
      return "MV_E_NETER (network error)";
    default:
      return "unknown MVS error";
  }
}

void log_device_info(const MV_CC_DEVICE_INFO *device) {
  if (!device)
    return;
  if (device->nTLayerType == MV_GIGE_DEVICE) {
    const auto &gige = device->SpecialInfo.stGigEInfo;
    const unsigned int ip = gige.nCurrentIp;
    log_msg(LOG_WARNING,
            "MVS camera: model=%s user=%s ip=%u.%u.%u.%u",
            reinterpret_cast<const char *>(gige.chModelName),
            reinterpret_cast<const char *>(gige.chUserDefinedName),
            (ip >> 24) & 0xff, (ip >> 16) & 0xff,
            (ip >> 8) & 0xff, ip & 0xff);
  }
}

size_t packed_step(unsigned int width, unsigned int height,
                   unsigned int frame_length, size_t bytes_per_pixel) {
  const size_t minimum = static_cast<size_t>(width) * bytes_per_pixel;
  if (height > 0 && frame_length % height == 0) {
    const size_t reported = frame_length / height;
    if (reported >= minimum)
      return reported;
  }
  return minimum;
}

// The MVS buffer belongs to the SDK. Every returned Mat owns a full-resolution
// copy before MV_CC_FreeImageBuffer() is called. No resize, crop, enhancement,
// blur or sharpening is applied. Native Mono8 remains one channel so the Nano
// does not move three copies of the same luminance data on every frame.
cv::Mat copy_mvs_frame(void *handle, const MV_FRAME_OUT &frame) {
  const auto &info = frame.stFrameInfo;
  if (!frame.pBufAddr || info.nWidth == 0 || info.nHeight == 0)
    return {};

  if (info.enPixelType == PixelType_Gvsp_BGR8_Packed) {
    cv::Mat bgr(static_cast<int>(info.nHeight), static_cast<int>(info.nWidth),
                CV_8UC3, frame.pBufAddr,
                packed_step(info.nWidth, info.nHeight, info.nFrameLen, 3));
    return bgr.clone();
  }

  if (info.enPixelType == PixelType_Gvsp_RGB8_Packed) {
    cv::Mat rgb(static_cast<int>(info.nHeight), static_cast<int>(info.nWidth),
                CV_8UC3, frame.pBufAddr,
                packed_step(info.nWidth, info.nHeight, info.nFrameLen, 3));
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    return bgr;
  }

  if (info.enPixelType == PixelType_Gvsp_Mono8) {
    cv::Mat mono(static_cast<int>(info.nHeight), static_cast<int>(info.nWidth),
                 CV_8UC1, frame.pBufAddr,
                 packed_step(info.nWidth, info.nHeight, info.nFrameLen, 1));
    return mono.clone();
  }

  // Bayer, YUV and packed formats cannot be described safely as a cv::Mat.
  // MVS performs only the required pixel-format conversion at native size.
  const size_t output_size =
      static_cast<size_t>(info.nWidth) * info.nHeight * 3;
  std::vector<unsigned char> output(output_size);
  MV_CC_PIXEL_CONVERT_PARAM conversion{};
  conversion.nWidth = info.nWidth;
  conversion.nHeight = info.nHeight;
  conversion.pSrcData = frame.pBufAddr;
  conversion.nSrcDataLen = info.nFrameLen;
  conversion.enSrcPixelType = info.enPixelType;
  conversion.enDstPixelType = PixelType_Gvsp_BGR8_Packed;
  conversion.pDstBuffer = output.data();
  conversion.nDstBufferSize = static_cast<unsigned int>(output.size());

  const int ret = MV_CC_ConvertPixelType(handle, &conversion);
  if (ret != MV_OK) {
    log_mvs_error("ConvertPixelType", ret);
    return {};
  }
  if (conversion.nDstLen != output_size) {
    log_msg(LOG_ERROR,
            "MVS converted frame has unexpected size: actual=%u expected=%zu",
            conversion.nDstLen, output_size);
    return {};
  }

  cv::Mat bgr(static_cast<int>(info.nHeight), static_cast<int>(info.nWidth),
              CV_8UC3, output.data(), info.nWidth * 3);
  return bgr.clone();
}

}  // namespace

MvsCamera::MvsCamera() = default;
MvsCamera::~MvsCamera() { close(); }

bool MvsCamera::start() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  if (running_)
    return true;

  MV_CC_DEVICE_INFO_LIST devices{};
  int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE, &devices);
  if (ret != MV_OK) {
    log_mvs_error("EnumDevices", ret);
    return false;
  }
  log_msg(LOG_DEBUG, "MVS GigE devices found: %u", devices.nDeviceNum);
  if (devices.nDeviceNum == 0 || !devices.pDeviceInfo[0]) {
    log_msg(LOG_ERROR, "MVS GigE camera not found");
    return false;
  }
  log_device_info(devices.pDeviceInfo[0]);
  if (!MV_CC_IsDeviceAccessible(devices.pDeviceInfo[0], MV_ACCESS_Exclusive)) {
    log_msg(LOG_ERROR,
            "MVS camera is not accessible with exclusive control; verify the "
            "Jetson Ethernet IP is in the camera subnet, then close MVS "
            "Client/Ip Configurator and stop other camera processes");
    return false;
  }

  ret = MV_CC_CreateHandle(&mvs_handle_, devices.pDeviceInfo[0]);
  if (ret != MV_OK) {
    mvs_handle_ = nullptr;
    log_mvs_error("CreateHandle", ret);
    return false;
  }

  ret = MV_CC_OpenDevice(mvs_handle_);
  if (ret != MV_OK) {
    log_mvs_error("OpenDevice", ret);
    log_msg(LOG_ERROR, "MVS OpenDevice code detail: %s", mvs_error_name(ret));
    MV_CC_DestroyHandle(mvs_handle_);
    mvs_handle_ = nullptr;
    return false;
  }

  // Preserve the camera's native Width, Height, PixelFormat, exposure, gain
  // and frame rate. Only acquisition transport/flow settings are changed.
  const int packet_size = MV_CC_GetOptimalPacketSize(mvs_handle_);
  if (packet_size > 0) {
    const int packet_ret = MV_CC_SetIntValue(
        mvs_handle_, "GevSCPSPacketSize", packet_size);
    if (packet_ret != MV_OK)
      log_mvs_error("Set GevSCPSPacketSize", packet_ret);
  } else {
    log_mvs_error("GetOptimalPacketSize", packet_size);
  }

  ret = MV_CC_SetEnumValue(mvs_handle_, "TriggerMode", 0);
  if (ret != MV_OK) {
    log_mvs_error("Set TriggerMode=Off", ret);
    MV_CC_CloseDevice(mvs_handle_);
    MV_CC_DestroyHandle(mvs_handle_);
    mvs_handle_ = nullptr;
    return false;
  }

  ret = MV_CC_StartGrabbing(mvs_handle_);
  if (ret != MV_OK) {
    log_mvs_error("StartGrabbing", ret);
    MV_CC_CloseDevice(mvs_handle_);
    MV_CC_DestroyHandle(mvs_handle_);
    mvs_handle_ = nullptr;
    return false;
  }

  running_ = true;
  log_msg(LOG_WARNING,
          "Camera started via Hikrobot MVS GigE (native camera settings)");
  return true;
}

void MvsCamera::stop() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  if (!mvs_handle_) {
    running_ = false;
    return;
  }

  if (running_) {
    const int ret = MV_CC_StopGrabbing(mvs_handle_);
    if (ret != MV_OK)
      log_mvs_error("StopGrabbing", ret);
  }
  const int close_ret = MV_CC_CloseDevice(mvs_handle_);
  if (close_ret != MV_OK)
    log_mvs_error("CloseDevice", close_ret);
  const int destroy_ret = MV_CC_DestroyHandle(mvs_handle_);
  if (destroy_ret != MV_OK)
    log_mvs_error("DestroyHandle", destroy_ret);

  mvs_handle_ = nullptr;
  running_ = false;
}

void MvsCamera::close() { stop(); }

cv::Mat MvsCamera::capture(std::uint32_t* frame_number) {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  if (!running_ || !mvs_handle_)
    return {};

  MV_FRAME_OUT frame{};
  const int ret = MV_CC_GetImageBuffer(mvs_handle_, &frame, 100);
  if (ret != MV_OK)
    return {};

  if (frame_number)
    *frame_number = frame.stFrameInfo.nFrameNum;

  static bool logged_frame_format = false;
  if (!logged_frame_format) {
    log_msg(LOG_WARNING,
            "MVS real frame: %ux%u pixel=0x%08x bytes=%u frame=%u",
            frame.stFrameInfo.nWidth, frame.stFrameInfo.nHeight,
            static_cast<unsigned int>(frame.stFrameInfo.enPixelType),
            frame.stFrameInfo.nFrameLen, frame.stFrameInfo.nFrameNum);
    logged_frame_format = true;
  }

  cv::Mat image;
  try {
    image = copy_mvs_frame(mvs_handle_, frame);
  } catch (...) {
    MV_CC_FreeImageBuffer(mvs_handle_, &frame);
    throw;
  }

  const int free_ret = MV_CC_FreeImageBuffer(mvs_handle_, &frame);
  if (free_ret != MV_OK) {
    log_mvs_error("FreeImageBuffer", free_ret);
    return {};
  }
  return image;
}

int MvsCamera::discard_frames(int count) {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  if (!running_ || !mvs_handle_ || count <= 0)
    return 0;

  int discarded = 0;
  while (discarded < count) {
    MV_FRAME_OUT frame{};
    if (MV_CC_GetImageBuffer(mvs_handle_, &frame, 100) != MV_OK)
      break;
    if (MV_CC_FreeImageBuffer(mvs_handle_, &frame) != MV_OK)
      break;
    ++discarded;
  }
  return discarded;
}
