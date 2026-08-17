#include "vision/image_processor.h"

#if CV_VERSION_MAJOR >= 5
#include <opencv2/geometry/2d.hpp>
#endif

#include <cmath>
#include <iostream>

namespace
{
  cv::Mat bottle_mask(bool attached_branch)
  {
    cv::Mat mask = cv::Mat::zeros(402, 540, CV_8UC1);
    cv::rectangle(mask, cv::Rect(145, 190, 250, 190), cv::Scalar(255), cv::FILLED);
    cv::rectangle(mask, cv::Rect(220, 55, 100, 155), cv::Scalar(255), cv::FILLED);
    cv::ellipse(mask, cv::Point(270, 195), cv::Size(125, 45), 0, 180, 360,
                cv::Scalar(255), cv::FILLED);
    if (attached_branch)
    {
      const std::vector<cv::Point> branch = {
          {220, 70}, {175, 52}, {125, 72}, {70, 68}, {42, 100}, {112, 112}, {175, 100}, {220, 95}};
      cv::fillConvexPoly(mask, branch, cv::Scalar(255));
    }
    return mask;
  }

  bool check_mask(bool attached_branch)
  {
    const cv::Mat mask = bottle_mask(attached_branch);
    std::vector<cv::Point> contour;
    cv::Mat product_mask;
    if (!ImageProcessor::select_product_contour(mask, contour, product_mask))
      return false;
    const cv::Rect bounds = cv::boundingRect(contour);
    const bool kept_body = bounds.x <= 145 && bounds.x + bounds.width >= 395 &&
                           bounds.y <= 55 && bounds.y + bounds.height >= 380;
    const bool unchanged = attached_branch ||
                           cv::countNonZero(mask != product_mask) == 0;
    const bool removed_branch = !attached_branch ||
                                (bounds.x >= 140 && product_mask.at<uchar>(80, 205) == 0);
    return kept_body && unchanged && removed_branch;
  }

  bool check_attached_neck_blob()
  {
    cv::Mat mask = bottle_mask(false);
    cv::circle(mask, cv::Point(205, 78), 20, cv::Scalar(255), cv::FILLED);
    std::vector<cv::Point> contour;
    cv::Mat product_mask;
    if (!ImageProcessor::select_product_contour(mask, contour, product_mask))
      return false;
    return product_mask.at<uchar>(78, 190) == 0 &&
           product_mask.at<uchar>(78, 225) != 0;
  }

  bool check_large_attached_upper_obstruction()
  {
    cv::Mat mask = bottle_mask(false);
    // A hand or cloth can form a tall bright component beside the neck. Its
    // full bounding box is wider than it is tall, even though the lower product
    // body still has the bottle geometry used to locate the real neck.
    cv::rectangle(mask, cv::Rect(40, 40, 180, 140), cv::Scalar(255), cv::FILLED);
    std::vector<cv::Point> contour;
    cv::Mat product_mask;
    if (!ImageProcessor::select_product_contour(mask, contour, product_mask))
      return false;
    const cv::Rect bounds = cv::boundingRect(contour);
    return product_mask.at<uchar>(100, 100) == 0 &&
           product_mask.at<uchar>(100, 270) != 0 && bounds.x >= 140;
  }

  bool check_lit_conveyor_rejected()
  {
    cv::Mat mask = cv::Mat::zeros(402, 540, CV_8UC1);
    cv::rectangle(mask, cv::Rect(0, 0, 540, 100), cv::Scalar(255), cv::FILLED);
    cv::rectangle(mask, cv::Rect(170, 230, 200, 160), cv::Scalar(255), cv::FILLED);
    cv::rectangle(mask, cv::Rect(230, 110, 80, 140), cv::Scalar(255), cv::FILLED);
    cv::ellipse(mask, cv::Point(270, 230), cv::Size(100, 35), 0, 180, 360,
                cv::Scalar(255), cv::FILLED);
    std::vector<cv::Point> contour;
    cv::Mat product_mask;
    if (!ImageProcessor::select_product_contour(mask, contour, product_mask))
      return false;
    return product_mask.at<uchar>(300, 270) != 0 &&
           product_mask.at<uchar>(20, 20) == 0;
  }

  bool check_connected_light_band_removed()
  {
    cv::Mat mask = bottle_mask(false);
    // Simulate a bright conveyor band touching both sides of the product body.
    cv::rectangle(mask, cv::Rect(0, 245, 540, 17), cv::Scalar(255), cv::FILLED);
    std::vector<cv::Point> contour;
    cv::Mat product_mask;
    if (!ImageProcessor::select_product_contour(mask, contour, product_mask))
      return false;
    const cv::Rect bounds = cv::boundingRect(contour);
    return product_mask.at<uchar>(252, 20) == 0 &&
           product_mask.at<uchar>(252, 520) == 0 &&
           product_mask.at<uchar>(252, 270) != 0 &&
           bounds.x >= 140 && bounds.x + bounds.width <= 400;
  }

  bool check_lower_side_glare_removed()
  {
    cv::Mat mask = bottle_mask(false);
    // Simulate a small reflection attached to the lower-left product edge.
    cv::rectangle(mask, cv::Rect(125, 330, 25, 17), cv::Scalar(255), cv::FILLED);
    std::vector<cv::Point> contour;
    cv::Mat product_mask;
    if (!ImageProcessor::select_product_contour(mask, contour, product_mask))
      return false;
    return product_mask.at<uchar>(338, 130) == 0 &&
           product_mask.at<uchar>(338, 170) != 0;
  }

  bool check_wide_cap_like_product_not_pruned()
  {
    cv::Mat mask = cv::Mat::zeros(402, 540, CV_8UC1);
    cv::ellipse(mask, cv::Point(270, 210), cv::Size(150, 90), 0, 0, 360,
                cv::Scalar(255), cv::FILLED);
    cv::rectangle(mask, cv::Rect(150, 150, 240, 120), cv::Scalar(255),
                  cv::FILLED);
    std::vector<cv::Point> contour;
    cv::Mat product_mask;
    if (!ImageProcessor::select_product_contour(mask, contour, product_mask))
      return false;
    const cv::Rect bounds = cv::boundingRect(contour);
    return bounds.x <= 150 && bounds.x + bounds.width >= 390 &&
           bounds.y <= 150 && bounds.y + bounds.height >= 270;
  }

  bool check_full_bright_roi_rejected()
  {
    cv::Mat mask(402, 540, CV_8UC1, cv::Scalar(255));
    std::vector<cv::Point> contour;
    cv::Mat product_mask;
    return !ImageProcessor::select_product_contour(mask, contour, product_mask);
  }

  bool check_contour_simplification()
  {
    const cv::Mat mask = bottle_mask(true);
    std::vector<std::vector<cv::Point>> dense, simple;
    cv::findContours(mask.clone(), dense, cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_NONE);
    cv::findContours(mask.clone(), simple, cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);
    if (dense.size() != 1 || simple.size() != 1)
      return false;
    return cv::boundingRect(dense[0]) == cv::boundingRect(simple[0]) &&
           std::abs(cv::contourArea(dense[0]) - cv::contourArea(simple[0])) < 0.5 &&
           simple[0].size() < dense[0].size();
  }
}

int main()
{
  if (!check_mask(false))
  {
    std::cerr << "normal bottle contour changed\n";
    return 1;
  }
  if (!check_mask(true))
  {
    std::cerr << "attached side branch was not removed\n";
    return 1;
  }
  if (!check_attached_neck_blob())
  {
    std::cerr << "attached neck blob was not removed\n";
    return 1;
  }
  if (!check_large_attached_upper_obstruction())
  {
    std::cerr << "large attached upper obstruction was not removed\n";
    return 1;
  }
  if (!check_lit_conveyor_rejected())
  {
    std::cerr << "lit conveyor patch was selected as product\n";
    return 1;
  }
  if (!check_connected_light_band_removed())
  {
    std::cerr << "connected conveyor light band was not removed\n";
    return 1;
  }
  if (!check_lower_side_glare_removed())
  {
    std::cerr << "lower side glare was not removed\n";
    return 1;
  }
  if (!check_wide_cap_like_product_not_pruned())
  {
    std::cerr << "wide product contour was pruned as a bottle neck\n";
    return 1;
  }
  if (!check_full_bright_roi_rejected())
  {
    std::cerr << "full bright ROI was selected as product\n";
    return 1;
  }
  if (!check_contour_simplification())
  {
    std::cerr << "simplified contour changed product geometry\n";
    return 1;
  }
  std::cout << "PRODUCT_CONTOUR_SELECTION=PASS\n";
  return 0;
}
