#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=jetson_platform.sh
source "${SCRIPT_DIR}/jetson_platform.sh"

BUILD_DIR="${ROOT_DIR}/build-nano-cpu"
BUILD_JOBS="${APP_BUILD_JOBS:-$(jetson_app_build_jobs)}"
APP="${BUILD_DIR}/jetson_inspect_v2"

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "ERROR: run this script on the Jetson Nano, not on the Mac."
    exit 1
fi

if [[ "$(jetson_platform)" != "nano" ]]; then
    echo "ERROR: this CPU-only build script is for Jetson Nano."
    echo "Detected: $(jetson_model)"
    exit 1
fi

if [[ ! -f /opt/MVS/include/MvCameraControl.h ]]; then
    echo "ERROR: Hikrobot MVS SDK is not installed: /opt/MVS/include/MvCameraControl.h"
    echo "Install MVS-3.0.1_aarch64_20251113.deb before rebuilding."
    exit 1
fi

if ! find /opt/MVS/lib /opt/MVS/lib/64 /opt/MVS/lib/aarch64 /opt/MVS/bin -maxdepth 2 \
    -name 'libMvCameraControl.so*' -print -quit 2>/dev/null | grep -q .; then
    echo "ERROR: Hikrobot MVS runtime library is missing under /opt/MVS."
    exit 1
fi

# Never leave an obsolete pre-MVS executable runnable after a failed rebuild.
rm -f "${APP}"

mkdir -p "${BUILD_DIR}"
(
    cd "${BUILD_DIR}"
    cmake \
        -D CMAKE_BUILD_TYPE=Release \
        -D JETSON_INSPECT_FORCE_CPU=ON \
        "${ROOT_DIR}"
)

cmake --build "${BUILD_DIR}" -- -j"${BUILD_JOBS}"

if ! grep -q '^JETSON_INSPECT_FORCE_CPU:BOOL=ON$' "${BUILD_DIR}/CMakeCache.txt"; then
    echo "ERROR: CPU-only build flag was not applied."
    exit 1
fi

CPU_STATE="${BUILD_DIR}/.counter_state.json"
if [[ ! -f "${CPU_STATE}" ]]; then
    for previous_state in \
        "${ROOT_DIR}/build/.counter_state.json"; do
        if [[ -f "${previous_state}" ]]; then
            cp -p "${previous_state}" "${CPU_STATE}"
            echo "Copied existing counter/ROI state to build-nano-cpu."
            break
        fi
    done
fi

echo "Nano CPU build ready: ${BUILD_DIR}/jetson_inspect_v2"
echo "Run: ./scripts/run_jetson_inspect_nano_cpu.sh"
