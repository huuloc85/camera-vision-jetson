#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCV_PREFIX="${OPENCV_CUDA_PREFIX:-/opt/opencv-cuda}"
BUILD_DIR="${ROOT_DIR}/build-cuda"
BUILD_JOBS="${APP_BUILD_JOBS:-4}"
OPENCV_CMAKE_DIR="${OPENCV_PREFIX}/lib/cmake/opencv4"

if [[ ! -f "${OPENCV_CMAKE_DIR}/OpenCVConfig.cmake" ]]; then
    echo "ERROR: CUDA OpenCV not found at ${OPENCV_PREFIX}."
    echo "Run first: ./scripts/install_opencv_cuda_jetson.sh"
    exit 1
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -D CMAKE_BUILD_TYPE=Release \
    -D OpenCV_DIR="${OPENCV_CMAKE_DIR}" \
    -D CMAKE_BUILD_RPATH="${OPENCV_PREFIX}/lib"

cmake --build "${BUILD_DIR}" --parallel "${BUILD_JOBS}"

LDD_OUTPUT="$(ldd "${BUILD_DIR}/jetson_inspect_v2")"
if ! grep -q "${OPENCV_PREFIX}" <<<"${LDD_OUTPUT}"; then
    echo "ERROR: executable is not linked to ${OPENCV_PREFIX}."
    grep opencv <<<"${LDD_OUTPUT}" || true
    exit 1
fi

# Detection, camera and ROI calibration are stored next to the executable.
# Preserve the production settings when build-cuda is created for the first
# time; never overwrite a state file already tuned specifically for CUDA.
OLD_STATE="${ROOT_DIR}/build/.counter_state.json"
CUDA_STATE="${BUILD_DIR}/.counter_state.json"
if [[ -f "${OLD_STATE}" && ! -f "${CUDA_STATE}" ]]; then
    cp -p "${OLD_STATE}" "${CUDA_STATE}"
    echo "Copied existing ROI/camera state to build-cuda."
fi

echo "CUDA build ready: ${BUILD_DIR}/jetson_inspect_v2"
echo "Run: ./scripts/run_jetson_inspect_cuda.sh"
