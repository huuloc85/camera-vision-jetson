#!/usr/bin/env bash

# Shared Jetson model detection for build and diagnostic scripts.
# JETSON_MODEL_OVERRIDE is only intended for CI/static validation.
jetson_model() {
    if [[ -n "${JETSON_MODEL_OVERRIDE:-}" ]]; then
        printf '%s\n' "${JETSON_MODEL_OVERRIDE}"
        return
    fi
    if [[ -r /proc/device-tree/model ]]; then
        tr -d '\0' < /proc/device-tree/model
        return
    fi
    printf '%s\n' "unknown"
}

jetson_platform() {
    local model
    model="$(jetson_model)"
    case "${model}" in
        *"Jetson Nano"*) printf '%s\n' "nano" ;;
        *)                printf '%s\n' "unknown" ;;
    esac
}

jetson_app_build_jobs() {
    case "$(jetson_platform)" in
        nano) printf '%s\n' "2" ;;
        *)    printf '%s\n' "1" ;;
    esac
}
