// -*- coding: utf-8 -*-
// core/time_utils.h — shared monotonic time helpers
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace core {

inline double now_sec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

inline void sleep_sec(double s) {
    if (s <= 0) return;
    std::this_thread::sleep_for(
        std::chrono::microseconds(static_cast<int64_t>(s * 1e6)));
}

}  // namespace core
