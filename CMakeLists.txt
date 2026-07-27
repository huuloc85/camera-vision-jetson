cmake_minimum_required(VERSION 3.10)
project(jetson_inspect_v2 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TB_MQTT_DEMO "Build ThingsBoard MQTT telemetry demo" OFF)

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

set(TELEMETRY_SRCS
    src/telemetry/image_http_server.cpp
    src/telemetry/thingsboard_mqtt_client.cpp
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

if(BUILD_TB_MQTT_DEMO)
    find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h)
    find_library(MOSQUITTO_LIBRARY mosquitto)
    if(NOT MOSQUITTO_INCLUDE_DIR OR NOT MOSQUITTO_LIBRARY)
        message(FATAL_ERROR
            "BUILD_TB_MQTT_DEMO=ON requires libmosquitto. "
            "Install libmosquitto-dev on Jetson/Ubuntu, then rerun CMake.")
    endif()

    add_executable(thingsboard_mqtt_demo
        demos/thingsboard_mqtt_demo.cpp
        src/vision/camera.cpp
        ${TELEMETRY_SRCS}
    )

    target_include_directories(thingsboard_mqtt_demo PRIVATE
        ${MOSQUITTO_INCLUDE_DIR}
    )

    target_link_libraries(thingsboard_mqtt_demo
        PRIVATE
        ${OpenCV_LIBS}
        ${MOSQUITTO_LIBRARY}
        pthread
    )
endif()

# ── Install rules ─────────────────────────────────────
install(TARGETS jetson_inspect_v2 DESTINATION bin)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/
    DESTINATION share/jetson_inspect_v2/config
    FILES_MATCHING PATTERN "*.json" PATTERN "*.txt"
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
