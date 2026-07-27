#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="$SCRIPT_DIR/run_jetson_hmi.sh"
SYSTEM_PINMUX="/usr/local/sbin/jetson-inspect-pinmux.sh"

find_devmem() {
  if command -v devmem >/dev/null 2>&1; then
    echo "devmem"
    return 0
  fi

  if command -v busybox >/dev/null 2>&1 && busybox --list | grep -qx devmem; then
    echo "busybox devmem"
    return 0
  fi

  return 1
}

write_devmem() {
  local address="$1"
  local value="$2"

  if [[ "$DEVMEM_CMD" == "busybox devmem" ]]; then
    sudo busybox devmem "$address" w "$value"
  else
    sudo "$DEVMEM_CMD" "$address" w "$value"
  fi
}

force_outputs_off() {
  sudo python3 - <<'PY'
import Jetson.GPIO as GPIO

GPIO.setwarnings(False)
GPIO.setmode(GPIO.BOARD)
for pin in (15, 13, 16):
    GPIO.setup(pin, GPIO.OUT, initial=GPIO.LOW)
    GPIO.output(pin, GPIO.LOW)
PY
}

if [[ ! -x "$RUNNER" ]]; then
  chmod +x "$RUNNER"
fi

if [[ "${JETSON_INSPECT_SKIP_PINMUX:-0}" != "1" ]]; then
  if [[ -x "$SYSTEM_PINMUX" ]]; then
    if sudo -n "$SYSTEM_PINMUX"; then
      echo "Pinmux ready. Starting HMI..."
      exec "$RUNNER" "$@"
    fi

    echo "ERROR: pinmux setup exists but sudo needs a password." >&2
    echo "Run ./install_camera_v2_desktop.sh once from a terminal, then click the desktop icon again." >&2
    exit 1
  fi

  if ! DEVMEM_CMD="$(find_devmem)"; then
    echo "ERROR: devmem not found. Install busybox-static and create devmem first:" >&2
    echo "  sudo apt install -y busybox-static" >&2
    echo "  sudo ln -sf \"\$(command -v busybox)\" /usr/local/sbin/devmem" >&2
    exit 1
  fi

  echo "Configuring Jetson Inspect GPIO pinmux..."
  sudo -v

  # OK: BOARD 15 -> output
  write_devmem 0x02440020 0x5

  # NG: BOARD 13 -> output
  write_devmem 0x0243D030 0x1005

  # BUSY: BOARD 16 -> output
  write_devmem 0x0243D020 0x5

  force_outputs_off

  echo "Pinmux ready. Starting HMI..."
fi

exec "$RUNNER" "$@"
