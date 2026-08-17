#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=jetson_platform.sh
source "${SCRIPT_DIR}/jetson_platform.sh"

errors=0
warnings=0

pass() { echo "PASS: $*"; }
warn() { echo "WARN: $*"; warnings=$((warnings + 1)); }
fail() { echo "FAIL: $*"; errors=$((errors + 1)); }

MODEL="$(jetson_model)"
if [[ "$(uname -m)" == "aarch64" ]]; then
    pass "architecture aarch64"
else
    fail "expected aarch64, got $(uname -m)"
fi

if [[ "$(jetson_platform)" == "nano" ]]; then
    pass "classic Jetson Nano detected: ${MODEL}"
else
    fail "this preflight is for Jetson Nano B01; detected: ${MODEL}"
fi

if [[ -r /etc/nv_tegra_release ]]; then
    L4T_RELEASE="$(sed -n 's/^# R\([0-9][0-9]*\).*/\1/p' /etc/nv_tegra_release | head -n 1)"
    if [[ "${L4T_RELEASE}" == "32" ]]; then
        pass "JetPack 4 / L4T 32 detected"
    else
        fail "Nano B01 requires the JetPack 4 / L4T 32 software line"
    fi
else
    fail "/etc/nv_tegra_release is missing"
fi

if command -v cmake >/dev/null 2>&1; then
    pass "cmake found"
else
    fail "cmake is missing"
fi

if command -v g++ >/dev/null 2>&1; then
    pass "C++ compiler found: $(g++ --version | head -n 1)"
else
    fail "g++ is missing"
fi

MEMORY_MB="$(awk '/MemTotal/ {print int($2 / 1024)}' /proc/meminfo 2>/dev/null)"
SWAP_MB="$(awk '/SwapTotal/ {print int($2 / 1024)}' /proc/meminfo 2>/dev/null)"
if [[ -n "${MEMORY_MB}" ]]; then
    pass "RAM detected: ${MEMORY_MB} MB"
fi
if [[ -n "${SWAP_MB}" ]] && (( SWAP_MB >= 4096 )); then
    pass "swap available: ${SWAP_MB} MB"
else
    warn "less than 4096 MB swap; the application build may be killed by the OOM killer"
fi

if [[ -f /opt/MVS/include/MvCameraControl.h ]]; then
    pass "MVS headers found in /opt/MVS/include"
else
    fail "MVS header /opt/MVS/include/MvCameraControl.h is missing"
fi

if find /opt/MVS/lib /opt/MVS/lib/aarch64 /opt/MVS/bin -maxdepth 2 -name 'libMvCameraControl.so*' \
    -print -quit 2>/dev/null | grep -q .; then
    pass "MVS runtime library found"
else
    fail "MVS runtime libMvCameraControl.so is missing"
fi

if [[ -r /sys/class/net/eth0/carrier ]] && [[ "$(< /sys/class/net/eth0/carrier)" == "1" ]]; then
    pass "GigE camera interface eth0 has link"
else
    warn "eth0 link is down; check the Hikrobot camera cable and power"
fi

if command -v ip >/dev/null 2>&1; then
    ETH0_ADDR="$(ip -4 -o addr show dev eth0 2>/dev/null | awk '{print $4}' | head -n 1)"
    if [[ -n "${ETH0_ADDR}" ]]; then
        pass "eth0 IPv4 address: ${ETH0_ADDR}"
    else
        warn "eth0 has no IPv4 address; set a static address in the camera subnet"
    fi
fi

if [[ -e /dev/ttyTHS1 && -r /dev/ttyTHS1 && -w /dev/ttyTHS1 ]]; then
    pass "UART /dev/ttyTHS1 is accessible"
else
    warn "UART /dev/ttyTHS1 is absent or lacks read/write permission"
fi

echo "Preflight summary: errors=${errors} warnings=${warnings}"
(( errors == 0 ))
