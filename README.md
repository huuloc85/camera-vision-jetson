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
