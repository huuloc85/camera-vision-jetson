// -*- coding: utf-8 -*-
// vision/image_processor.h — ROI warp, preprocess, metrics
#pragma once

#include "core/types.h"
#include "core/config.h"
#include <opencv2/opencv.hpp>

#if USE_CUDA_ACCEL
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudafilters.hpp>
#include <opencv2/cudawarping.hpp>
#include <opencv2/cudaarithm.hpp>
#endif

// ══════════════════════════════════════════════════════
// ImageProcessor — runtime ROI + preprocessing
// ══════════════════════════════════════════════════════
class ImageProcessor {
public:
    // Accepts runtime-tunable detection and ROI state by const reference.
    explicit ImageProcessor(const DetectionParams& params, const RoiParams& roi);

    cv::Mat sharpen(const cv::Mat& img);
    std::pair<cv::Mat, cv::Mat> preprocess(const cv::Mat& roi);
    cv::Mat warp_roi(const cv::Mat& frame);        // Full-res 1080×804
    cv::Mat warp_roi_fast(const cv::Mat& frame);   // Half-res 540×402

    // Rebuild both transforms after ROI edits. If frame_size is empty, the
    // next warp call rebuilds for the captured frame dimensions.
    void refresh_roi(const cv::Size& frame_size = cv::Size());
    std::array<cv::Point2f, 4> roi_points_for_frame(const cv::Size& frame_size) const;
    cv::Point2f reference_point_from_frame(const cv::Point2f& point,
                                           const cv::Size& frame_size) const;
    static std::tuple<double, int, int> measure_spike(
        const std::vector<cv::Point>& contour, const cv::Mat& thresh);

    static ShapeMetrics calculate_metrics(
        const std::vector<cv::Point>& contour,
        int x, int y, int w, int h, const cv::Mat& thresh);

private:
    const DetectionParams& params_;
    const RoiParams& roi_;

    cv::Mat morph_kernel_;
    cv::Mat morph_kernel_large_;
    cv::Mat morph_kernel_xl_;
    cv::Mat sharpen_kernel_;
    cv::Mat M_;       // Full-res perspective
    cv::Mat M_half_;  // Half-res perspective
    cv::Size transform_frame_size_;

    void rebuild_roi_transforms(const cv::Size& frame_size);
    void ensure_roi_transforms(const cv::Size& frame_size);

    bool use_cuda_ = false;

#if USE_CUDA_ACCEL
    mutable cv::cuda::GpuMat gpu_frame_, gpu_warp_, gpu_roi_;
    mutable cv::cuda::GpuMat gpu_gray_, gpu_blurred_, gpu_thresh_, gpu_morph_;
    cv::Ptr<cv::cuda::Filter> gpu_gaussian_;
    cv::Ptr<cv::cuda::Filter> gpu_morph_close_large_;
    cv::Ptr<cv::cuda::Filter> gpu_morph_open_xl_;
    mutable bool gpu_roi_cache_valid_ = false;
#endif
};
