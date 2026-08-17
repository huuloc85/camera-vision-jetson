#pragma once

#include <cstdlib>
#include <fstream>
#include <string>

namespace core {

inline std::string jetson_model() {
    if (const char* override_model = std::getenv("JETSON_MODEL_OVERRIDE"))
        return override_model;

    std::ifstream input("/proc/device-tree/model", std::ios::binary);
    std::string model;
    std::getline(input, model, '\0');
    return model;
}

inline bool is_jetson_nano() {
    const std::string model = jetson_model();
    return model.find("Jetson Nano") != std::string::npos &&
           model.find("Orin") == std::string::npos;
}

inline int recommended_opencv_threads() {
    if (const char* value = std::getenv("JETSON_OPENCV_THREADS")) {
        const int parsed = std::atoi(value);
        if (parsed > 0 && parsed <= 16)
            return parsed;
    }
    return is_jetson_nano() ? 2 : 4;
}

}  // namespace core
