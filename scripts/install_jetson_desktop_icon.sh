#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
LAUNCHER="${SCRIPT_DIR}/run_jetson_inspect_desktop.sh"
ICON="${SCRIPT_DIR}/cap-ins7a-spike.svg"
DESKTOP_FILE="${HOME}/.local/share/applications/cap-ins7a-spike.desktop"

DESKTOP_DIR=""
if command -v xdg-user-dir >/dev/null 2>&1; then
    DESKTOP_DIR="$(xdg-user-dir DESKTOP 2>/dev/null || true)"
fi
if [[ -z "${DESKTOP_DIR}" || "${DESKTOP_DIR}" == "${HOME}" ]]; then
    DESKTOP_DIR="${HOME}/Desktop"
fi
DESKTOP_COPY="${DESKTOP_DIR}/cap-ins7a-spike.desktop"

mkdir -p "${HOME}/.local/share/applications" "${DESKTOP_DIR}"
chmod +x "${LAUNCHER}"

cat > "${DESKTOP_FILE}" <<EOF
[Desktop Entry]
Type=Application
Name=CAP-INS7A-SPIKE
Comment=Open CAP-INS7A-SPIKE industrial inspection HMI
Exec=${LAUNCHER}
Icon=${ICON}
Terminal=false
Categories=Utility;Engineering;
StartupNotify=false
X-GNOME-Autostart-enabled=false
EOF

cp "${DESKTOP_FILE}" "${DESKTOP_COPY}"
chmod +x "${DESKTOP_FILE}" "${DESKTOP_COPY}"

if command -v gio >/dev/null 2>&1; then
    DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$(id -u)/bus}" \
        gio set "${DESKTOP_COPY}" metadata::trusted true >/dev/null 2>&1 || true
fi

echo "Installed desktop icon: ${DESKTOP_COPY}"
echo "Double-click CAP-INS7A-SPIKE to open the CUDA HMI."
echo "Runtime log: ${ROOT_DIR}/logs/desktop-launch.log"
