cmake_minimum_required(VERSION 3.10)
project(jetson_inspect_v2 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Find dependencies ─────────────────────────────────
find_package(OpenCV REQUIRED)

# ── Include directories ───────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
)

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
