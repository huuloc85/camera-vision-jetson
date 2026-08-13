#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CUDA_RUNNER="${SCRIPT_DIR}/run_jetson_inspect_cuda.sh"
LOG_DIR="${ROOT_DIR}/logs"
LOG_FILE="${LOG_DIR}/desktop-launch.log"
CAMERA_START_TIMEOUT_SEC=12

mkdir -p "${LOG_DIR}"

{
    printf '\n[%s] Desktop launcher requested\n' "$(date '+%Y-%m-%d %H:%M:%S')"

    if [[ ! -x "${CUDA_RUNNER}" ]]; then
        echo "ERROR: CUDA runner is not executable: ${CUDA_RUNNER}"
        echo "Run: chmod +x ${ROOT_DIR}/scripts/*.sh"
        exit 1
    fi

    echo "Desktop camera startup: HW-MJPEG attempt"
    first_new_log_byte=$(( $(wc -c < "${LOG_FILE}") + 1 ))
    JETSON_CAM_HW_MJPEG=1 "${CUDA_RUNNER}" &
    app_pid=$!
    camera_ready=0

    deadline=$((SECONDS + CAMERA_START_TIMEOUT_SEC))
    while (( SECONDS < deadline )); do
        new_startup_log="$(tail -c "+${first_new_log_byte}" "${LOG_FILE}")"
        if [[ "${new_startup_log}" == *"Camera started"* ]]; then
            camera_ready=1
            break
        fi

        if ! kill -0 "${app_pid}" 2>/dev/null; then
            wait "${app_pid}"
            status=$?
            if [[ ${status} -eq 75 ]]; then
                echo "HMI is already running; ignored duplicate click."
                exit 0
            fi
            echo "HMI exited during camera startup with status ${status}."
            exit "${status}"
        fi

        sleep 0.25
    done

    if [[ ${camera_ready} -eq 1 ]]; then
        wait "${app_pid}"
        status=$?
    else
        echo "HW-MJPEG startup timeout; restarting with V4L2"
        kill -TERM "${app_pid}" 2>/dev/null || true

        for _ in {1..10}; do
            if ! kill -0 "${app_pid}" 2>/dev/null; then
                break
            fi
            sleep 0.1
        done

        if kill -0 "${app_pid}" 2>/dev/null; then
            kill -KILL "${app_pid}" 2>/dev/null || true
        fi
        wait "${app_pid}" 2>/dev/null || true

        sleep 1
        echo "Desktop camera startup: V4L2 fallback"
        JETSON_CAM_HW_MJPEG=0 "${CUDA_RUNNER}"
        status=$?
    fi

    if [[ ${status} -eq 75 ]]; then
        echo "HMI is already running; ignored duplicate click."
        exit 0
    fi

    echo "HMI exited with status ${status}."
    exit "${status}"
} >> "${LOG_FILE}" 2>&1
