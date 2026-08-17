#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=jetson_platform.sh
source "${SCRIPT_DIR}/jetson_platform.sh"

APP="${ROOT_DIR}/build-nano-cpu/jetson_inspect_v2"
LOCK_FILE="${XDG_RUNTIME_DIR:-/tmp}/jetson-inspect-v2-${UID}.lock"

if [[ "$(jetson_platform)" != "nano" ]]; then
    echo "ERROR: this CPU-only runner is for Jetson Nano."
    echo "Detected: $(jetson_model)"
    exit 1
fi

if [[ ! -x "${APP}" ]]; then
    echo "ERROR: executable not found: ${APP}"
    echo "Run first: ./scripts/build_jetson_nano_cpu.sh"
    exit 1
fi

if ! grep -aFq 'MVS real frame:' "${APP}"; then
    echo "ERROR: stale camera binary detected: ${APP}"
    echo "This executable still predates the MVS-only camera flow."
    echo "Install /opt/MVS, then rebuild with: APP_BUILD_JOBS=2 ./scripts/build_jetson_nano_cpu.sh"
    exit 1
fi

export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-${HOME}/.Xauthority}"
export MVCAM_SDK_PATH="/opt/MVS"
export MVCAM_COMMON_RUNENV="/opt/MVS/lib"
export MVCAM_SOFTWARE_LIBENV="/opt/MVS/lib"
export MVCAM_GENICAM_CLPROTOCOL="/opt/MVS/lib/CLProtocol"
export LD_LIBRARY_PATH="/opt/MVS/lib/aarch64:/opt/MVS/lib:/opt/MVS/lib/64:/opt/MVS/bin${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

MVS_LDD="$(ldd "${APP}")"
if ! grep -Fq 'libMvCameraControl.so =>' <<<"${MVS_LDD}"; then
    echo "ERROR: executable is not resolving libMvCameraControl.so"
    grep -E 'MvCamera|not found' <<<"${MVS_LDD}" || true
    exit 1
fi

cd "${ROOT_DIR}"

exec 9>"${LOCK_FILE}"
if ! flock --nonblock 9; then
    echo "ERROR: jetson-inspect-v2 is already running."
    exit 75
fi

exec "${APP}"
