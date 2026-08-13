# jetson-inspect-v2

Refactored từ `jetson-inspect` (v1) theo kiến trúc module hóa.  
**Folder gốc `jetson-inspect` không bị thay đổi.**

---

## Cấu trúc thư mục

```
jetson-inspect-v2/
├── include/
│   ├── core/
│   │   ├── types.h         — Shared types: AppState, ProductResult, ShapeMetrics, InspectionResult
│   │   ├── config.h        — Tất cả config hằng số (UART, camera, detection, display)
│   │   └── logger.h        — Simple stderr logger
│   ├── gpio/
│   │   └── gpio_controller.h  — Interface UART GPIO qua ESP32
│   ├── vision/
│   │   ├── camera.h        — LibcameraCapture (Argus/libcamera/V4L2)
│   │   ├── image_processor.h  — ROI warp, preprocess, metrics
│   │   ├── classifier.h    — ProductClassifier (OK/NG logic)
│   │   ├── detection_state.h  — Counters, params, state machine, persistence
│   │   └── vision_service.h   — Core vision orchestrator (no UI)
│   └── hmi/
│       ├── theme.h         — Color palette + Draw primitives
│       └── touch_hmi.h     — TouchHMI (render) + TouchApp (main loop)
├── src/
│   ├── core/types.cpp
│   ├── gpio/gpio_controller.cpp
│   ├── vision/
│   │   ├── detection_state.cpp
│   │   ├── camera.cpp
│   │   ├── image_processor.cpp
│   │   ├── classifier.cpp
│   │   └── vision_service.cpp
│   └── hmi/
│       ├── theme.cpp
│       └── touch_hmi.cpp   — Entry point (main)
├── CMakeLists.txt
└── README.md
```

---

## Kiến trúc

```
TouchApp (main loop)
    │
    ├── VisionService        ← Camera, GPIO, pipeline, state machine
    │     ├── LibcameraCapture
    │     ├── ImageProcessor
    │     ├── ProductClassifier
    │     ├── DetectionState
    │     └── GPIOController
    │
    └── TouchHMI             ← Render UI (OpenCV window)
          └── Theme / Draw
```

**Nguyên tắc:**
- `VisionService` không biết OpenCV window, không gọi `imshow`.
- `TouchHMI` không biết camera, GPIO, không tự chạy thuật toán.
- Giao tiếp qua `InspectionResult` struct (zero-copy trong single process).
- State machine `AppState` được chia sẻ qua `DetectionState`.

---

## Build

```bash
cd jetson-inspect-v2
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./jetson_inspect_v2
```

## Jetson Orin: build CUDA riêng (không ghi đè bản đang chạy)

Các script dưới đây phải chạy trên Jetson (`aarch64`), không chạy trên Mac.
OpenCV CUDA được cài riêng vào `/opt/opencv-cuda`; binary CPU trong `build/` và
file căn chỉnh `.counter_state.json` không bị thay thế.

```bash
cd ~/jetson-inspect-v2
chmod +x scripts/*cuda*.sh
./scripts/install_opencv_cuda_jetson.sh
./scripts/build_jetson_cuda.sh

DISPLAY=:0 XAUTHORITY=/home/vvp/.Xauthority \
./scripts/run_jetson_inspect_cuda.sh
```

Nếu `nvcc` chưa tồn tại, cài JetPack development trước:

```bash
sudo apt update
sudo apt install nvidia-jetpack
```

Build CUDA dùng kiến trúc Orin `8.7`, mặc định chỉ build 2 job để tránh hết RAM.
Có thể đổi bằng `OPENCV_BUILD_JOBS=1` hoặc `2`. Khi khởi động đúng, log phải có
`Build: CUDA acceleration ENABLED`, `Camera realtime capture worker ON` và
`OpenCV CUDA` trong output `opencv_version --verbose`.

Mỗi trigger publish `HMI freeze: published 1080x804` bằng đúng perspective ROI
cận sản phẩm giống Calibration mode; frame nền bị bỏ và không đưa lên HMI. Log
`Trigger timing` tách `critical` (đến GPIO), `hmi_frame` và `total`; log
`Trigger displayed` đo cả bước đưa ảnh lên cửa sổ. Chỉ coi mục tiêu dưới 50 ms
là đạt sau khi đo liên tục ít nhất 20 sản phẩm trên Jetson.

HMI dùng cùng template `IndustrialHmiApp` của `opencv-detect`: canvas 1024x600,
top bar 52 px, bottom touch bar 96 px, panel kết quả phải 190 px và khung camera
có lề 18 px. Phần giao diện này không thay đổi Vision ROI hay ESP32/UART.

Camera USB lấy cấu hình từ biến môi trường của launcher, tương tự luồng CLI/env
của `opencv-detect`. Giá trị mặc định production vẫn là `/dev/video0`, MJPG,
1920x1080 @ 60 FPS và NVIDIA HW-MJPEG; do đó flow trigger/vision/UART không đổi:

```bash
JETSON_CAM_V4L2_INDEX=0 \
JETSON_CAM_WIDTH=1920 JETSON_CAM_HEIGHT=1080 JETSON_CAM_FPS=60 \
JETSON_CAM_FOURCC=MJPG JETSON_CAM_HW_MJPEG=1 \
./scripts/run_jetson_inspect_cuda.sh
```

Focus được đọc trực tiếp từ `config/app_config.json` khi camera khởi động:

```json
"focus_automatic_continuous": true,
"focus_absolute": 5
```

Khi autofocus là `true`, chương trình không gửi `focus_absolute`. Khi đổi sang
`false`, hai control được gửi tuần tự: tắt autofocus trước rồi mới đặt focus,
tránh lỗi I/O của camera khi gửi chung một lệnh. Nút `Mat Khau: BAT/TAT` trên
HMI điều khiển bảo vệ phần Căn Chỉnh; nhập đúng chữ số cuối sẽ tự xác nhận,
không cần nhấn `OK`.

Camera sẽ log mode thực tế sau khi driver negotiate. Để đo riêng tốc độ detect
giống benchmark của `opencv-detect`, đồng thời kiểm tra latency hiển thị đầy đủ:

```bash
python3 scripts/analyze_vision_timing.py logs/desktop-launch.log
```

`Vision equivalent throughput` chỉ là `1000 / detect_avg`; tiêu chí production
vẫn là `Trigger displayed: max < 50ms` với tối thiểu 20 mẫu.

## Tạo icon chạy HMI trên Desktop Jetson

Chạy bằng user đang đăng nhập màn hình Jetson, không dùng `sudo`:

```bash
cd ~/jetson-inspect-v2
chmod +x scripts/*.sh
./scripts/install_jetson_desktop_icon.sh
```

Sau đó double-click icon **CAP-INS7A-SPIKE** trên Desktop. Launcher luôn chạy
binary CUDA `build-cuda/jetson_inspect_v2`, tự đặt `DISPLAY=:0`, dùng
`~/.Xauthority`, và không cho mở hai HMI cùng lúc. Log khi mở từ icon nằm tại
`logs/desktop-launch.log`.

## GitNexus MCP

Project đã được index bằng GitNexus với alias `jetson-inspect-v2`.
Codex MCP project-local nằm ở `.codex/config.toml`:

```toml
[mcp_servers.gitnexus]
command = "npx"
args = ["-y", "gitnexus@latest", "mcp"]
```

Sau khi sửa code đáng kể, cập nhật graph:

```bash
npx -y gitnexus@latest analyze . --skip-git --skip-agents-md --name jetson-inspect-v2
```

Vì folder này hiện không có `.git`, GitNexus cần `--skip-git` và commit tracking sẽ là `unknown`.

---

## So sánh v1 vs v2

| | v1 (jetson-inspect) | v2 (jetson-inspect-v2) |
|---|---|---|
| Files | 2 files (full_calb + hmi) | 10+ files theo module |
| UI tách biệt | ❌ Vision render UI trực tiếp | ✅ TouchHMI độc lập |
| State Machine | ❌ Nhiều biến bool rải rác | ✅ `AppState` enum rõ ràng |
| Params | ❌ Nhúng trong DetectionState | ✅ `DetectionParams` struct riêng |
| Mở rộng | Khó — phải sửa file lớn | Dễ — thêm module mới |
| GPIO | Nhúng trong full_calb.cpp | ✅ Module riêng `gpio_controller` |
| Config | Rải rác nhiều struct | ✅ Gom vào `core/config.h` |
