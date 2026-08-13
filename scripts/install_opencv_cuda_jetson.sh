#!/usr/bin/env bash
set -euo pipefail

# Build an isolated CUDA-enabled OpenCV for Jetson Orin Nano.  The system
# OpenCV and the current production binary are left untouched.
OPENCV_VERSION="${OPENCV_VERSION:-4.10.0}"
INSTALL_PREFIX="${OPENCV_CUDA_PREFIX:-/opt/opencv-cuda}"
BUILD_JOBS="${OPENCV_BUILD_JOBS:-2}"
CUDA_ARCH_BIN="${JETSON_CUDA_ARCH:-8.7}"

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "ERROR: run this script on the Jetson (aarch64), not on the Mac."
    exit 1
fi

NVCC_BIN="$(command -v nvcc || true)"
if [[ -z "${NVCC_BIN}" && -x /usr/local/cuda/bin/nvcc ]]; then
    NVCC_BIN=/usr/local/cuda/bin/nvcc
fi

if [[ -z "${NVCC_BIN}" ]]; then
    echo "ERROR: CUDA toolkit (nvcc) is missing."
    echo "Install the JetPack development packages first, then rerun this script:"
    echo "  sudo apt update && sudo apt install nvidia-jetpack"
    exit 1
fi

echo "CUDA compiler: $("${NVCC_BIN}" --version | tail -n 1)"
echo "OpenCV source: ${OPENCV_VERSION}"
echo "Install prefix: ${INSTALL_PREFIX}"
echo "CUDA architecture: ${CUDA_ARCH_BIN} (Orin)"

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake git pkg-config ninja-build \
    libgtk-3-dev libtbb-dev libeigen3-dev \
    libjpeg-dev libpng-dev libtiff-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
    gstreamer1.0-tools gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
    gstreamer1.0-libav \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libv4l-dev

SOURCE_ROOT="$(mktemp -d /tmp/jetson-opencv-cuda.XXXXXX)"
echo "Temporary build directory: ${SOURCE_ROOT}"
echo "It is intentionally retained after the script for build diagnostics."

git clone --depth 1 --branch "${OPENCV_VERSION}" \
    https://github.com/opencv/opencv.git "${SOURCE_ROOT}/opencv"
git clone --depth 1 --branch "${OPENCV_VERSION}" \
    https://github.com/opencv/opencv_contrib.git "${SOURCE_ROOT}/opencv_contrib"

cmake -S "${SOURCE_ROOT}/opencv" -B "${SOURCE_ROOT}/build" -G Ninja \
    -D CMAKE_BUILD_TYPE=Release \
    -D CMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -D OPENCV_EXTRA_MODULES_PATH="${SOURCE_ROOT}/opencv_contrib/modules" \
    -D BUILD_LIST=core,imgproc,imgcodecs,videoio,highgui,cudev,cudaarithm,cudaimgproc,cudafilters,cudawarping \
    -D WITH_CUDA=ON \
    -D CUDA_ARCH_BIN="${CUDA_ARCH_BIN}" \
    -D CUDA_ARCH_PTX= \
    -D WITH_CUDNN=OFF \
    -D OPENCV_DNN_CUDA=OFF \
    -D WITH_GSTREAMER=ON \
    -D WITH_V4L=ON \
    -D WITH_GTK=ON \
    -D WITH_OPENGL=OFF \
    -D BUILD_SHARED_LIBS=ON \
    -D BUILD_TESTS=OFF \
    -D BUILD_PERF_TESTS=OFF \
    -D BUILD_EXAMPLES=OFF \
    -D BUILD_opencv_apps=ON \
    -D BUILD_opencv_python2=OFF \
    -D BUILD_opencv_python3=OFF \
    -D BUILD_JAVA=OFF

cmake --build "${SOURCE_ROOT}/build" --parallel "${BUILD_JOBS}"
sudo cmake --install "${SOURCE_ROOT}/build"

CUDA_VERSION_OUTPUT="$(LD_LIBRARY_PATH="${INSTALL_PREFIX}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    "${INSTALL_PREFIX}/bin/opencv_version" --verbose)"

if ! grep -Eq 'NVIDIA CUDA:[[:space:]]+YES' <<<"${CUDA_VERSION_OUTPUT}"; then
    echo "ERROR: OpenCV was installed but CUDA is not enabled."
    exit 1
fi

for module in cudaarithm cudaimgproc cudafilters cudawarping; do
    if ! find "${INSTALL_PREFIX}/lib" -maxdepth 1 -name "libopencv_${module}.so*" -print -quit | grep -q .; then
        echo "ERROR: missing CUDA module: ${module}"
        exit 1
    fi
done

echo "OpenCV CUDA installation verified:"
grep -E 'OpenCV modules:|NVIDIA CUDA:|GStreamer:|v4l/v4l2:' <<<"${CUDA_VERSION_OUTPUT}" || true
echo "Next: ./scripts/build_jetson_cuda.sh"
