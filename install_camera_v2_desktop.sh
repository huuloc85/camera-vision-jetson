#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
BIN="$BUILD_DIR/jetson_inspect_v2"
RUNNER="$PROJECT_DIR/run_camera_v2.sh"
DESKTOP_DIR="${XDG_DESKTOP_DIR:-$HOME/Desktop}"
DESKTOP_FILE="$DESKTOP_DIR/camera-v2.desktop"

mkdir -p "$DESKTOP_DIR"

cat > "$RUNNER" <<EOF
#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$PROJECT_DIR"
BUILD_DIR="\$PROJECT_DIR/build"
BIN="\$BUILD_DIR/jetson_inspect_v2"
LOG_FILE="\$HOME/camera-v2.log"
LOCK_FILE="/tmp/camera-v2.lock"

export DISPLAY="\${DISPLAY:-:0}"
export HMI_CALIB_PASSWORD="\${HMI_CALIB_PASSWORD:-1234}"

mkdir -p "\$BUILD_DIR"
cd "\$PROJECT_DIR"

if [[ ! -x "\$BIN" ]]; then
  cmake -S "\$PROJECT_DIR" -B "\$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  cmake --build "\$BUILD_DIR" -- -j"\$(nproc)"
fi

cd "\$BUILD_DIR"
exec flock -n "\$LOCK_FILE" "\$BIN" >> "\$LOG_FILE" 2>&1
EOF

chmod +x "$RUNNER"

cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=camera-v2
Comment=Run Jetson Inspect HMI
Exec=$RUNNER
Icon=camera-video
Terminal=false
Categories=Utility;
StartupNotify=false
EOF

chmod +x "$DESKTOP_FILE"

if command -v gio >/dev/null 2>&1; then
  gio set "$DESKTOP_FILE" metadata::trusted true >/dev/null 2>&1 || true
fi

echo "Created desktop launcher: $DESKTOP_FILE"
echo "Double-click camera-v2 on the Desktop to run the HMI."
echo "Runtime log: $HOME/camera-v2.log"
