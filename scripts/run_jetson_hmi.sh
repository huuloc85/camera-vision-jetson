#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$APP_DIR/build"
BIN="$BUILD_DIR/jetson_inspect_v2"
LOG="$APP_DIR/run.log"

export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-/home/vvp/.Xauthority}"
export HMI_FULLSCREEN="${HMI_FULLSCREEN:-0}"
export HMI_WINDOW_NAME="${HMI_WINDOW_NAME:-HMI}"
export HMI_WINDOW_X="${HMI_WINDOW_X:-0}"
export HMI_WINDOW_Y="${HMI_WINDOW_Y:-0}"
export HMI_WINDOW_W="${HMI_WINDOW_W:-1024}"
export HMI_WINDOW_H="${HMI_WINDOW_H:-600}"

export JETSON_CAMERA_BACKEND="${JETSON_CAMERA_BACKEND:-${OPENCV_TAIL_CAMERA_BACKEND:-auto}}"
export JETSON_CAM_SENSOR_ID="${JETSON_CAM_SENSOR_ID:-0}"
export JETSON_CAM_DEVICE="${JETSON_CAM_DEVICE:-${OPENCV_TAIL_CAMERA_INDEX:-0}}"
export JETSON_CAM_WIDTH="${JETSON_CAM_WIDTH:-${OPENCV_TAIL_CAMERA_WIDTH:-1280}}"
export JETSON_CAM_HEIGHT="${JETSON_CAM_HEIGHT:-${OPENCV_TAIL_CAMERA_HEIGHT:-720}}"
export JETSON_CAM_FPS="${JETSON_CAM_FPS:-${OPENCV_TAIL_CAMERA_FPS:-120}}"
export JETSON_CAM_MIN_EXPOSURE_US="${JETSON_CAM_MIN_EXPOSURE_US:-${OPENCV_TAIL_CAMERA_MIN_EXPOSURE_US:-}}"
export JETSON_CAM_MAX_EXPOSURE_US="${JETSON_CAM_MAX_EXPOSURE_US:-${OPENCV_TAIL_CAMERA_MAX_EXPOSURE_US:-}}"
export JETSON_CAM_MAX_GAIN="${JETSON_CAM_MAX_GAIN:-${OPENCV_TAIL_CAMERA_MAX_GAIN:-}}"

mkdir -p "$BUILD_DIR"

if [[ ! -x "$BIN" ]] || [[ -n "$(find "$APP_DIR/src" "$APP_DIR/include" "$APP_DIR/config" "$APP_DIR/CMakeLists.txt" -newer "$BIN" -print -quit 2>/dev/null)" ]]; then
  cmake -S "$APP_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD_DIR" -- -j"$(nproc)"
fi

pkill -f jetson_inspect_v2 2>/dev/null || true
sleep 0.5

cd "$BUILD_DIR"

if [[ "$(id -u)" -ne 0 && "${JETSON_INSPECT_RUN_AS_ROOT:-0}" == "1" ]]; then
  if command -v xhost >/dev/null 2>&1; then
    xhost +SI:localuser:root >/dev/null 2>&1 || xhost +local:root >/dev/null 2>&1 || true
  fi

  sudo env \
    DISPLAY="$DISPLAY" \
    XAUTHORITY="$XAUTHORITY" \
    HMI_FULLSCREEN="$HMI_FULLSCREEN" \
    HMI_WINDOW_NAME="$HMI_WINDOW_NAME" \
    HMI_WINDOW_X="$HMI_WINDOW_X" \
    HMI_WINDOW_Y="$HMI_WINDOW_Y" \
    HMI_WINDOW_W="$HMI_WINDOW_W" \
    HMI_WINDOW_H="$HMI_WINDOW_H" \
    JETSON_CAMERA_BACKEND="$JETSON_CAMERA_BACKEND" \
    JETSON_CAM_SENSOR_ID="$JETSON_CAM_SENSOR_ID" \
    JETSON_CAM_DEVICE="$JETSON_CAM_DEVICE" \
    JETSON_CAM_WIDTH="$JETSON_CAM_WIDTH" \
    JETSON_CAM_HEIGHT="$JETSON_CAM_HEIGHT" \
    JETSON_CAM_FPS="$JETSON_CAM_FPS" \
    JETSON_CAM_MIN_EXPOSURE_US="$JETSON_CAM_MIN_EXPOSURE_US" \
    JETSON_CAM_MAX_EXPOSURE_US="$JETSON_CAM_MAX_EXPOSURE_US" \
    JETSON_CAM_MAX_GAIN="$JETSON_CAM_MAX_GAIN" \
    "$BIN" 2>&1 | tee "$LOG"
  exit ${PIPESTATUS[0]}
fi

"$BIN" 2>&1 | tee "$LOG"
