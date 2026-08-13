#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
APP="${ROOT_DIR}/build/jetson_inspect_v2"
LOG_DIR="${ROOT_DIR}/logs"
LOG_FILE="${LOG_DIR}/jetson_inspect.log"

export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-${HOME}/.Xauthority}"
export JETSON_CAM_V4L2_INDEX="${JETSON_CAM_V4L2_INDEX:-0}"
export JETSON_CAM_WIDTH="${JETSON_CAM_WIDTH:-1920}"
export JETSON_CAM_HEIGHT="${JETSON_CAM_HEIGHT:-1080}"
export JETSON_CAM_FPS="${JETSON_CAM_FPS:-60}"
export JETSON_CAM_FOURCC="${JETSON_CAM_FOURCC:-MJPG}"
export JETSON_CAM_HW_MJPEG="${JETSON_CAM_HW_MJPEG:-1}"

mkdir -p "${LOG_DIR}"
cd "${ROOT_DIR}"

if [[ ! -x "${APP}" ]]; then
    echo "Executable not found: ${APP}" >> "${LOG_FILE}"
    echo "Build first: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)" >> "${LOG_FILE}"
    exit 1
fi

exec "${APP}" >> "${LOG_FILE}" 2>&1
