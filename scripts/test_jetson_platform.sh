#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=jetson_platform.sh
source "${SCRIPT_DIR}/jetson_platform.sh"

assert_equals() {
    local expected="$1"
    local actual="$2"
    local label="$3"
    if [[ "${actual}" != "${expected}" ]]; then
        echo "FAIL: ${label}: expected ${expected}, got ${actual}"
        exit 1
    fi
    echo "PASS: ${label}=${actual}"
}

JETSON_MODEL_OVERRIDE="NVIDIA Jetson Nano Developer Kit"
assert_equals nano "$(jetson_platform)" "Nano platform"
assert_equals 2 "$(jetson_app_build_jobs)" "Nano app jobs"

JETSON_MODEL_OVERRIDE="NVIDIA Jetson Orin Nano Engineering Reference Developer Kit"
assert_equals unknown "$(jetson_platform)" "Non-Nano platform"
assert_equals 1 "$(jetson_app_build_jobs)" "Unknown platform safe jobs"

JETSON_MODEL_OVERRIDE="unknown board"
assert_equals unknown "$(jetson_platform)" "Unknown platform"
echo "PASS: unknown platform fails closed"
