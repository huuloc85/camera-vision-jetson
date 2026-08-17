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
│   │   ├── camera.h        — MvsCamera (Hikrobot GigE only)
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
    │     ├── MvsCamera
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

## Hikrobot GigE / MVS camera

Camera production lấy ảnh qua Hikrobot MVS SDK 3.0.1 trong `/opt/MVS`:

```text
MVS GigE → MV_CC_GetImageBuffer → native-size copy/convert → OpenCV
```

MVS là camera backend duy nhất và CMake bắt buộc phải tìm thấy
`MvCameraControl`. Ứng dụng không ghi `Width`, `Height`, `AcquisitionFrameRate`,
`PixelFormat`, exposure hoặc gain; chất lượng native đang cấu hình trên camera
được giữ nguyên. Flow trigger, realtime latest-frame, Vision/ROI, UART và HMI
của `VisionService` không đổi.

Frame Mono8 được đưa sang BGR chỉ để vẽ overlay màu. Frame RGB/BGR giữ nguyên
màu; Bayer/YUV được MVS chuyển sang BGR ở đúng kích thước camera, không resize,
crop, sharpen, blur hay ép grayscale.

HMI cũng có live preview liên tục khi chờ trigger: camera worker giữ acquisition
native và cập nhật một mailbox chỉ chứa bản preview vừa vùng hiển thị (tối đa
30 FPS). Bản preview này chỉ phục vụ hiển thị; frame dùng cho trigger/Vision
vẫn là frame native đầy đủ của MVS.

```bash
sudo dpkg -i MVS-3.0.1_aarch64_20251113.deb
export LD_LIBRARY_PATH=/opt/MVS/lib:/opt/MVS/lib/64:/opt/MVS/lib/aarch64:/opt/MVS/bin:$LD_LIBRARY_PATH
./scripts/preflight_jetson_nano.sh
./scripts/build_jetson_nano_cpu.sh
./scripts/run_jetson_inspect_nano_cpu.sh
```

Khi đúng, build/runtime phải có `Hikrobot MVS: REQUIRED`,
`MVS GigE devices found: 1`, `Camera started via Hikrobot MVS GigE` và
`MVS real frame: <width>x<height> pixel=... bytes=... frame=...`.

---

## Jetson Nano B01: MVS + CPU-only

Đây là flow build duy nhất của branch này. Nano dùng OpenCV hệ thống và ép toàn
bộ Vision/ROI sang CPU. Binary được tạo trong `build-nano-cpu`; flow
trigger/UART/HMI không thay đổi:

```bash
./scripts/build_jetson_nano_cpu.sh
./scripts/run_jetson_inspect_nano_cpu.sh
```

```bash
cd ~/jetson-inspect-v2
chmod +x scripts/*.sh
./scripts/preflight_jetson_nano.sh
```

Nếu preflight không có `FAIL`, chạy build và runner CPU-only ở trên.

Nano lấy camera Hikrobot GigE trực tiếp bằng MVS SDK. Project không
còn code CSI, USB, Argus, libcamera, V4L2 hoặc GStreamer camera fallback.

Nano tự giới hạn OpenCV CPU ở 2 thread để dành CPU cho camera/HMI/UART. Chỉ
đổi khi chẩn đoán bằng biến `JETSON_OPENCV_THREADS`; production nên để mặc định.

Khi benchmark Nano, có thể khóa chế độ hiệu năng tối đa (đổi lại máy nóng và
tiêu thụ điện cao hơn):

```bash
sudo nvpmodel -m 0
sudo jetson_clocks
```

Phải dùng nguồn DC 5V/4A qua jack barrel và quạt chủ động trước khi bật chế độ
này. Các lệnh chỉ đổi clock/power, không đổi flow Vision hoặc UART.

Mỗi trigger publish `HMI freeze: published 1080x804` bằng đúng perspective ROI
cận sản phẩm giống Calibration mode; frame nền bị bỏ và không đưa lên HMI. Log
`Trigger timing` tách `critical` (đến GPIO), `hmi_frame` và `total`; log
`Trigger displayed` đo cả bước đưa ảnh lên cửa sổ. Chỉ coi mục tiêu dưới 50 ms
là đạt sau khi đo liên tục ít nhất 20 sản phẩm trên Jetson.

HMI dùng cùng template `IndustrialHmiApp` của `opencv-detect`: canvas 1024x600,
top bar 52 px, bottom touch bar 96 px, panel kết quả phải 190 px và khung camera
có lề 18 px. Phần giao diện này không thay đổi Vision ROI hay ESP32/UART.

Mọi thiết lập chất lượng camera được chỉnh và lưu bằng MVS Client. Ứng dụng chỉ
tắt hardware trigger của camera để giữ continuous acquisition theo flow trigger
PLC hiện tại; không ghi đè các thông số hình ảnh.

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
binary CPU-only `build-nano-cpu/jetson_inspect_v2`, tự đặt `DISPLAY=:0`, dùng
`~/.Xauthority`, và không cho mở hai HMI cùng lúc. Log khi mở từ icon nằm tại
`logs/desktop-launch.log`.

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
