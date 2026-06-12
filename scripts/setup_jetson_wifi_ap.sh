#!/usr/bin/env bash
set -euo pipefail

CON_NAME="${JETSON_SETUP_CON_NAME:-jetson-setup-ap}"
IFACE="${JETSON_WIFI_IFACE:-wlan0}"
SETUP_IP="${JETSON_SETUP_IP:-}"
PASSWORD="${JETSON_SETUP_PASSWORD:-Jetson@123456}"
OPEN_WIFI="${JETSON_SETUP_OPEN:-0}"
CAPTIVE_PORTAL="${JETSON_CAPTIVE_PORTAL:-1}"

if [[ -z "${SETUP_IP}" && ("${CAPTIVE_PORTAL}" == "1" || "${CAPTIVE_PORTAL}" == "true" || "${CAPTIVE_PORTAL}" == "TRUE" || "${CAPTIVE_PORTAL}" == "yes") ]]; then
  SETUP_IP="10.42.0.1"
fi

if ! command -v nmcli >/dev/null 2>&1; then
  echo "nmcli not found. Install NetworkManager first." >&2
  exit 1
fi

if [[ ! -d "/sys/class/net/${IFACE}" ]]; then
  echo "WiFi interface not found: ${IFACE}" >&2
  echo "Run: nmcli dev status" >&2
  exit 1
fi

MAC="$(tr -d ':-' < "/sys/class/net/${IFACE}/address" | tr '[:lower:]' '[:upper:]')"
SUFFIX="${MAC: -6}"
SSID="${JETSON_SETUP_SSID:-JETSON-${SUFFIX}}"
SCAN_CACHE="${JETSON_WIFI_SCAN_CACHE:-/tmp/jetson_wifi_scan_cache.txt}"

SCAN_TMP="$(mktemp /tmp/jetson_wifi_scan.XXXXXX)"
nmcli -t --escape yes -f SSID,SIGNAL,SECURITY dev wifi list ifname "${IFACE}" --rescan yes > "${SCAN_TMP}" 2>/dev/null || true
if grep -vE '^(|JETSON-[^:]*):' "${SCAN_TMP}" | grep -q ':'; then
  grep -vE '^(|JETSON-[^:]*):' "${SCAN_TMP}" > "${SCAN_CACHE}"
elif [[ ! -s "${SCAN_CACHE}" ]]; then
  cp "${SCAN_TMP}" "${SCAN_CACHE}"
fi
rm -f "${SCAN_TMP}"

if nmcli -t -f NAME con show | grep -Fxq "${CON_NAME}"; then
  sudo nmcli con delete "${CON_NAME}" >/dev/null
fi

sudo nmcli con add type wifi ifname "${IFACE}" con-name "${CON_NAME}" autoconnect yes ssid "${SSID}"
sudo nmcli con modify "${CON_NAME}" 802-11-wireless.mode ap
sudo nmcli con modify "${CON_NAME}" 802-11-wireless.band bg
sudo nmcli con modify "${CON_NAME}" ipv4.method shared
if [[ -n "${SETUP_IP}" ]]; then
  sudo nmcli con modify "${CON_NAME}" ipv4.addresses "${SETUP_IP}/24"
fi
if [[ "${OPEN_WIFI}" == "1" || "${OPEN_WIFI}" == "true" || "${OPEN_WIFI}" == "TRUE" || "${OPEN_WIFI}" == "yes" ]]; then
  SECURITY="open"
else
  SECURITY="wpa-psk"
  sudo nmcli con modify "${CON_NAME}" wifi-sec.key-mgmt wpa-psk
  sudo nmcli con modify "${CON_NAME}" wifi-sec.psk "${PASSWORD}"
fi
sudo nmcli con up "${CON_NAME}"

ACTUAL_IP="$(ip -4 -o addr show dev "${IFACE}" scope global | awk '{split($4,a,"/"); print a[1]; exit}')"
if [[ -z "${ACTUAL_IP}" ]]; then
  ACTUAL_IP="${SETUP_IP:-unknown}"
fi

if [[ "${CAPTIVE_PORTAL}" == "1" || "${CAPTIVE_PORTAL}" == "true" || "${CAPTIVE_PORTAL}" == "TRUE" || "${CAPTIVE_PORTAL}" == "yes" ]]; then
  sudo mkdir -p /etc/NetworkManager/dnsmasq-shared.d
  printf '%s\n' \
    "address=/#/${ACTUAL_IP}" \
    "dhcp-option=option:router,${ACTUAL_IP}" \
    "dhcp-option=option:dns-server,${ACTUAL_IP}" | sudo tee /etc/NetworkManager/dnsmasq-shared.d/jetson-captive.conf >/dev/null
  sudo nmcli con down "${CON_NAME}" >/dev/null || true
  sudo nmcli con up "${CON_NAME}"
  ACTUAL_IP="$(ip -4 -o addr show dev "${IFACE}" scope global | awk '{split($4,a,"/"); print a[1]; exit}')"
  if [[ -z "${ACTUAL_IP}" ]]; then
    ACTUAL_IP="${SETUP_IP:-unknown}"
  fi
  if command -v iptables >/dev/null 2>&1; then
    sudo iptables -t nat -D PREROUTING -i "${IFACE}" -p tcp --dport 80 -j REDIRECT --to-ports 8090 2>/dev/null || true
    sudo iptables -t nat -C PREROUTING -i "${IFACE}" -p tcp --dport 80 -j REDIRECT --to-ports 8090 2>/dev/null \
      || sudo iptables -t nat -A PREROUTING -i "${IFACE}" -p tcp --dport 80 -j REDIRECT --to-ports 8090
  fi
fi

cat <<EOF
Jetson setup WiFi is active.
SSID       : ${SSID}
Security   : ${SECURITY}
Password   : $([[ "${SECURITY}" == "open" ]] && echo "none" || echo "${PASSWORD}")
Captive    : ${CAPTIVE_PORTAL}
Gateway IP : ${ACTUAL_IP}
Device API : http://${ACTUAL_IP}:8090/device-info
Setup UI   : http://${ACTUAL_IP}:8090/setup
EOF
