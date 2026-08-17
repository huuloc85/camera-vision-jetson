#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
APP_RUNNER="${SCRIPT_DIR}/run_jetson_inspect_nano_cpu.sh"
LOG_DIR="${ROOT_DIR}/logs"
LOG_FILE="${LOG_DIR}/desktop-launch.log"

mkdir -p "${LOG_DIR}"

{
    printf '\n[%s] Desktop launcher requested\n' "$(date '+%Y-%m-%d %H:%M:%S')"

    # eth0 is the dedicated Hikrobot link. NetworkManager may finish bringing
    # it up after the desktop autostart entry has already been launched.
    if [[ -d /sys/class/net/eth0 ]]; then
        network_deadline=$((SECONDS + 30))
        while (( SECONDS < network_deadline )); do
            carrier="$(cat /sys/class/net/eth0/carrier 2>/dev/null || true)"
            eth0_ipv4="$(ip -4 -o addr show dev eth0 2>/dev/null || true)"
            if [[ "${carrier}" == "1" && -n "${eth0_ipv4}" ]]; then
                break
            fi
            sleep 1
        done
        echo "MVS eth0 link: carrier=$(cat /sys/class/net/eth0/carrier 2>/dev/null || echo unknown)"
        ip -br -4 addr show dev eth0 2>/dev/null || true
    fi

    if [[ ! -x "${APP_RUNNER}" ]]; then
        echo "ERROR: application runner is not executable: ${APP_RUNNER}"
        echo "Run: chmod +x ${ROOT_DIR}/scripts/*.sh"
        exit 1
    fi

    while true; do
        echo "Desktop application startup: ${APP_RUNNER}"
        "${APP_RUNNER}"
        status=$?

        if [[ ${status} -eq 0 ]]; then
            echo "HMI exited normally."
            exit 0
        fi
        if [[ ${status} -eq 75 ]]; then
            echo "HMI is already running; ignored duplicate launch."
            exit 0
        fi

        echo "HMI exited with status ${status}; restarting in 3 seconds."
        sleep 3
    done
} >> "${LOG_FILE}" 2>&1
