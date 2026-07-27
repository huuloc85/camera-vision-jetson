#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="$PROJECT_DIR/scripts/run_jetson_hmi_with_pinmux.sh"
TARGET_USER="${SUDO_USER:-$USER}"
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
DESKTOP_DIR="${XDG_DESKTOP_DIR:-$TARGET_HOME/Desktop}"
DESKTOP_FILE="$DESKTOP_DIR/jetson-inspect-v2.desktop"
PINMUX_SCRIPT="/usr/local/sbin/jetson-inspect-pinmux.sh"
PINMUX_SERVICE="/etc/systemd/system/jetson-inspect-pinmux.service"
SUDOERS_FILE="/etc/sudoers.d/jetson-inspect-pinmux"

mkdir -p "$DESKTOP_DIR"

if [[ ! -x "$RUNNER" ]]; then
  chmod +x "$RUNNER"
fi

echo "Installing root-owned GPIO pinmux helper..."
sudo tee "$PINMUX_SCRIPT" >/dev/null <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

run_devmem() {
  local address="$1"
  local value="$2"

  if command -v devmem >/dev/null 2>&1; then
    devmem "$address" w "$value"
    return
  fi

  if command -v busybox >/dev/null 2>&1 && busybox --list | grep -qx devmem; then
    busybox devmem "$address" w "$value"
    return
  fi

  echo "ERROR: devmem not found. Install busybox-static first." >&2
  exit 1
}

# OK: BOARD 15 -> output
run_devmem 0x02440020 0x5

# NG: BOARD 13 -> output
run_devmem 0x0243D030 0x1005

# BUSY: BOARD 16 -> output
run_devmem 0x0243D020 0x5

# Force all PLC outputs OFF after pinmux. devmem selects GPIO mode/direction,
# but the output data latch can keep a previous/default level across boot.
python3 - <<'PY'
import Jetson.GPIO as GPIO

GPIO.setwarnings(False)
GPIO.setmode(GPIO.BOARD)
for pin in (15, 13, 16):
    GPIO.setup(pin, GPIO.OUT, initial=GPIO.LOW)
    GPIO.output(pin, GPIO.LOW)
PY
EOF
sudo chown root:root "$PINMUX_SCRIPT"
sudo chmod 0755 "$PINMUX_SCRIPT"

echo "Allowing $TARGET_USER to run only the pinmux helper without a sudo password..."
sudo tee "$SUDOERS_FILE" >/dev/null <<EOF
$TARGET_USER ALL=(root) NOPASSWD: $PINMUX_SCRIPT
EOF
sudo chmod 0440 "$SUDOERS_FILE"
sudo visudo -cf "$SUDOERS_FILE" >/dev/null

echo "Installing boot-time pinmux service..."
sudo tee "$PINMUX_SERVICE" >/dev/null <<EOF
[Unit]
Description=Configure Jetson Inspect GPIO pinmux
After=multi-user.target

[Service]
Type=oneshot
ExecStart=$PINMUX_SCRIPT
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now jetson-inspect-pinmux.service

echo "Configuring GPIO user permissions..."
sudo groupadd -f -r gpio
sudo usermod -a -G gpio "$TARGET_USER"
if [[ -f /opt/nvidia/jetson-gpio/etc/99-gpio.rules ]]; then
  sudo cp /opt/nvidia/jetson-gpio/etc/99-gpio.rules /etc/udev/rules.d/
fi
sudo udevadm control --reload-rules
sudo udevadm trigger

sudo chown -R "$TARGET_USER:$TARGET_USER" "$PROJECT_DIR/build" 2>/dev/null || true
sudo chown "$TARGET_USER:$TARGET_USER" "$PROJECT_DIR/run.log" 2>/dev/null || true

cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=Jetson Inspect V2
Comment=Run Jetson Inspect HMI
Exec=$RUNNER
Icon=utilities-terminal
Terminal=true
Categories=Utility;
StartupNotify=false
EOF

chmod +x "$DESKTOP_FILE"
chown "$TARGET_USER:$TARGET_USER" "$DESKTOP_FILE" 2>/dev/null || true

if command -v gio >/dev/null 2>&1; then
  gio set "$DESKTOP_FILE" metadata::trusted true >/dev/null 2>&1 || true
fi

echo "Created desktop launcher: $DESKTOP_FILE"
echo "Double-click Jetson Inspect V2 on the Desktop to run the HMI."
echo "Runner: $RUNNER"
echo "If GPIO still fails without sudo, log out/in or reboot once so the gpio group applies."
