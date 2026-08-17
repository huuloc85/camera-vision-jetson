#!/usr/bin/env bash
set -euo pipefail

# Persistent NetworkManager profile for the dedicated Hikrobot GigE link.
# The Jetson USB management link (192.168.55.1) is not changed.
INTERFACE="${MVS_GIGE_INTERFACE:-eth0}"
CONNECTION="${MVS_GIGE_CONNECTION:-mvs-camera-${INTERFACE}}"
ADDRESS="${MVS_GIGE_ADDRESS:-192.168.1.100/24}"

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "ERROR: run this script on the Jetson."
    exit 1
fi
if ! command -v nmcli >/dev/null 2>&1; then
    echo "ERROR: NetworkManager/nmcli is required."
    exit 1
fi
if [[ ! -d "/sys/class/net/${INTERFACE}" ]]; then
    echo "ERROR: network interface ${INTERFACE} does not exist."
    ip -br link
    exit 1
fi

if nmcli -t -f NAME connection show | grep -Fxq "${CONNECTION}"; then
    sudo nmcli connection modify "${CONNECTION}" \
        connection.interface-name "${INTERFACE}" \
        connection.autoconnect yes \
        connection.autoconnect-priority 100 \
        ipv4.method manual \
        ipv4.addresses "${ADDRESS}" \
        ipv4.never-default yes \
        ipv6.method ignore
else
    sudo nmcli connection add type ethernet \
        ifname "${INTERFACE}" \
        con-name "${CONNECTION}" \
        connection.autoconnect yes \
        connection.autoconnect-priority 100 \
        ipv4.method manual \
        ipv4.addresses "${ADDRESS}" \
        ipv4.never-default yes \
        ipv6.method ignore
fi

sudo nmcli connection up "${CONNECTION}"

echo "MVS GigE network ready:"
ip -br link show dev "${INTERFACE}"
ip -br -4 addr show dev "${INTERFACE}"
echo "Persistent profile: ${CONNECTION} (${ADDRESS})"
