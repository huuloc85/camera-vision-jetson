cmake_minimum_required(VERSION 3.10)
project(jetson_inspect_v2 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(JETSON_INSPECT_FORCE_CPU "Disable OpenCV CUDA acceleration" OFF)

# ── Find dependencies ─────────────────────────────────
if(JETSON_INSPECT_FORCE_CPU)
    find_package(OpenCV REQUIRED COMPONENTS
        core imgproc imgcodecs highgui
    )
else()
    find_package(OpenCV REQUIRED)
endif()

# ── Include directories ───────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
)

find_path(MVS_INCLUDE_DIR MvCameraControl.h
    PATHS /opt/MVS/include
    NO_DEFAULT_PATH
)
find_library(MVS_LIBRARY MvCameraControl
    PATHS /opt/MVS/lib /opt/MVS/lib/64 /opt/MVS/lib/aarch64 /opt/MVS/bin
    NO_DEFAULT_PATH
)
if(NOT MVS_INCLUDE_DIR OR NOT MVS_LIBRARY)
    message(FATAL_ERROR
        "Hikrobot MVS SDK is required: expected headers/libs under /opt/MVS")
endif()
include_directories(${MVS_INCLUDE_DIR})
message(STATUS "Hikrobot MVS: REQUIRED (${MVS_LIBRARY})")

# ── Source files ─────────────────────────────────────
set(CORE_SRCS
    src/core/types.cpp
)

set(GPIO_SRCS
    src/gpio/gpio_controller.cpp
)

set(VISION_SRCS
    src/vision/detection_state.cpp
    src/vision/camera.cpp
    src/vision/image_processor.cpp
    src/vision/classifier.cpp
    src/vision/vision_service.cpp
)

set(HMI_SRCS
    src/hmi/theme.cpp
    src/hmi/touch_hmi.cpp
)

# ── Main executable ───────────────────────────────────
add_executable(jetson_inspect_v2
    src/main.cpp
    ${CORE_SRCS}
    ${GPIO_SRCS}
    ${VISION_SRCS}
    ${HMI_SRCS}
)

target_link_libraries(jetson_inspect_v2
    PRIVATE
    ${OpenCV_LIBS}
    pthread
)

target_link_libraries(jetson_inspect_v2 PRIVATE ${MVS_LIBRARY})
set_target_properties(jetson_inspect_v2 PROPERTIES
    BUILD_RPATH "/opt/MVS/lib;/opt/MVS/lib/64;/opt/MVS/lib/aarch64;/opt/MVS/bin"
)

# Enable the CUDA code path only when this exact OpenCV installation exports
# every module used by ImageProcessor.  CUDA toolkit availability alone is not
# enough; Ubuntu's stock OpenCV packages are commonly CPU-only on Jetson.
set(OPENCV_CUDA_MODULES
    opencv_cudaarithm
    opencv_cudaimgproc
    opencv_cudafilters
    opencv_cudawarping
)
set(OPENCV_CUDA_READY FALSE)
if(NOT JETSON_INSPECT_FORCE_CPU)
    set(OPENCV_CUDA_READY TRUE)
    foreach(module IN LISTS OPENCV_CUDA_MODULES)
        if(NOT TARGET ${module})
            set(OPENCV_CUDA_READY FALSE)
        endif()
    endforeach()
endif()

if(OPENCV_CUDA_READY)
    target_compile_definitions(jetson_inspect_v2 PRIVATE JETSON_INSPECT_OPENCV_CUDA=1)
    target_link_libraries(jetson_inspect_v2 PRIVATE ${OPENCV_CUDA_MODULES})
    message(STATUS "OpenCV CUDA: ENABLED")
else()
    target_compile_definitions(jetson_inspect_v2 PRIVATE JETSON_INSPECT_OPENCV_CUDA=0)
    if(JETSON_INSPECT_FORCE_CPU)
        message(STATUS "OpenCV CUDA: DISABLED (CPU-only build requested)")
    else()
        message(STATUS "OpenCV CUDA: DISABLED (required CUDA modules not found)")
    endif()
endif()

# ── Install rules ─────────────────────────────────────
install(TARGETS jetson_inspect_v2 DESTINATION bin)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/
    DESTINATION share/jetson_inspect_v2/config
    FILES_MATCHING PATTERN "*.json"
)

# GCC < 9: explicit filesystem link
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(jetson_inspect_v2 PRIVATE stdc++fs)
endif()

# ── Build type default ────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "OpenCV      : ${OpenCV_VERSION}")
message(STATUS "Compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
