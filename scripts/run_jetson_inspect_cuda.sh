#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCV_PREFIX="${OPENCV_CUDA_PREFIX:-/opt/opencv-cuda}"
APP="${ROOT_DIR}/build-cuda/jetson_inspect_v2"
LOCK_FILE="${XDG_RUNTIME_DIR:-/tmp}/jetson-inspect-v2-${UID}.lock"

if [[ ! -x "${APP}" ]]; then
    echo "ERROR: executable not found: ${APP}"
    echo "Run first: ./scripts/build_jetson_cuda.sh"
    exit 1
fi

export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-${HOME}/.Xauthority}"
export LD_LIBRARY_PATH="${OPENCV_PREFIX}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export JETSON_CAM_V4L2_INDEX="${JETSON_CAM_V4L2_INDEX:-0}"
export JETSON_CAM_WIDTH="${JETSON_CAM_WIDTH:-1920}"
export JETSON_CAM_HEIGHT="${JETSON_CAM_HEIGHT:-1080}"
export JETSON_CAM_FPS="${JETSON_CAM_FPS:-60}"
export JETSON_CAM_FOURCC="${JETSON_CAM_FOURCC:-MJPG}"
# Direct SSH runs must favor reliable startup. The Desktop launcher explicitly
# opts into HW-MJPEG and supervises that attempt with a bounded V4L2 fallback.
export JETSON_CAM_HW_MJPEG="${JETSON_CAM_HW_MJPEG:-0}"

cd "${ROOT_DIR}"

# Keep UART, camera, and fullscreen HMI single-owner even if the launcher is
# clicked twice or the runner is also invoked from SSH.
exec 9>"${LOCK_FILE}"
if ! flock --nonblock 9; then
    echo "ERROR: jetson-inspect-v2 is already running."
    exit 75
fi

exec "${APP}"
