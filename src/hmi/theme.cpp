// -*- coding: utf-8 -*-
// hmi/theme.cpp — Drawing primitive implementations
#include "hmi/theme.h"

void Draw::rrect(cv::Mat& img, int x, int y, int w, int h,
                 cv::Scalar color, int r) {
    r = std::min(r, std::min(w, h) / 2);
    cv::rectangle(img, {x+r, y},   {x+w-r, y+h}, color, -1);
    cv::rectangle(img, {x,   y+r}, {x+w,   y+h-r}, color, -1);
    cv::circle(img, {x+r,   y+r},   r, color, -1, cv::LINE_AA);
    cv::circle(img, {x+w-r, y+r},   r, color, -1, cv::LINE_AA);
    cv::circle(img, {x+r,   y+h-r}, r, color, -1, cv::LINE_AA);
    cv::circle(img, {x+w-r, y+h-r}, r, color, -1, cv::LINE_AA);
}
