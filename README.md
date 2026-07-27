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

## Run HMI on Jetson screen

Run the local screen HMI with the minimal camera/display variables the current code actually uses:

```bash
DISPLAY=:0 XAUTHORITY=/home/vvp/.Xauthority \
HMI_FULLSCREEN=0 \
HMI_WINDOW_NAME=HMI-CAM-0 \
HMI_WINDOW_X=0 HMI_WINDOW_Y=0 HMI_WINDOW_W=1024 HMI_WINDOW_H=600 \
OPENCV_TAIL_CAMERA_BACKEND=v4l2 \
OPENCV_TAIL_CAMERA_INDEX=0 \
OPENCV_TAIL_CAMERA_WIDTH=1280 \
OPENCV_TAIL_CAMERA_HEIGHT=720 \
OPENCV_TAIL_CAMERA_FPS=60 \
./scripts/run_jetson_hmi.sh
```

`HMI_FOCUS_*` variables are not used by the current codebase.

## Capture ROI for calibration

To tune ROI directly on the Jetson screen, stop the app and open the ROI editor:

```bash
cd /home/vvp/jetson-inspect-v2
sudo pkill -f jetson_inspect_v2 || true
DISPLAY=:0 XAUTHORITY=/home/vvp/.Xauthority \
python3 demos/roi_editor_tool.py --width 1280 --height 720 --fps 60
```

Drag `P1/P2/P3/P4` on the full camera frame. Press `S` to save to
`config/roi_points.txt`, then restart the main app. The app reads this file on
startup.

Use this tool to grab the current camera frame and ROI cutout so you can send the images back for ROI tuning:

```bash
cd /home/vvp/jetson-inspect-v2
sudo pkill -f jetson_inspect_v2 || true
python3 demos/capture_roi_tool.py --out debug/roi_captures --width 1280 --height 720 --fps 60
```

It saves:

- `01_full_frame.jpg`
- `02_roi_overlay.jpg`
- `03_roi_warp.jpg`
- `04_roi_warp_fast.jpg`
- `05_threshold.jpg`
- `metadata.txt`

If you want to capture on PLC trigger instead of immediately, add `--wait-trigger`.

## Demo PLC input PNP opto pin 22

App chính đang đọc PLC trigger bằng Jetson.GPIO `BOARD` numbering ở pin 22,
active-HIGH rising edge. Với flow hiện tại, PLC/opto ON là HIGH, app bật
`BUSY`, xử lý camera, bật `OK` hoặc `NG`, tắt `BUSY`, rồi giữ `OK/NG` cho tới
khi trigger nhả về LOW. Không đưa 24V PLC trực tiếp vào GPIO Jetson.

Chạy demo độc lập để test tín hiệu input trước khi mở camera/HMI:

```bash
cd jetson-inspect-v2
python3 demos/plc_input_pin22_demo.py --show-state --active-high
```

cấu hình production hiện tại là active-HIGH, nên khi PLC/opto ON demo cần in
`state=HIGH` và trigger được nhận ở cạnh LOW -> HIGH. Nếu
cần test trên máy dev không có Jetson.GPIO:

```bash
python3 demos/plc_input_pin22_demo.py --mock --duration 1 --show-state --active-high
```

## Demo PLC output OK / NG / BUSY

App chính xuất tín hiệu PLC bằng Jetson.GPIO `BOARD` numbering:

| Signal | BOARD pin | Logic |
|---|---:|---|
| OK | 15 | active-HIGH |
| NG | 13 | active-HIGH |
| BUSY | 16 | active-HIGH |

Tắt app chính trước khi test output để tránh tranh chấp GPIO:

```bash
sudo pkill -f jetson_inspect_v2
sudo python3 demos/plc_output_3signals_demo.py
```

Demo sẽ bật `BUSY` lên HIGH để ON, trả LOW để OFF, rồi pulse `OK` và `NG`
cùng kiểu active-HIGH. Nếu muốn lặp liên tục để đo bằng đồng hồ hoặc xem PLC
input:

```bash
sudo python3 demos/plc_output_3signals_demo.py --loop
```

## Demo Trigger -> OK / NG

Tool này đọc `BOARD 22` rồi trả `OK` hoặc `NG` theo tuỳ chọn, để bạn tách riêng
kiểm tra opto / trigger input khỏi camera và vision:

```bash
cd /home/vvp/jetson-inspect-v2
sudo python3 demos/plc_trigger_response_tool.py --show-state --trigger-active-high --result toggle
```

Một vài chế độ hay dùng:

```bash
sudo python3 demos/plc_trigger_response_tool.py --trigger-active-high --result ok
sudo python3 demos/plc_trigger_response_tool.py --trigger-active-high --result ng
sudo python3 demos/plc_trigger_response_tool.py --trigger-active-high --result toggle --toggle-start ok
```

Với cấu hình production hiện tại, chạy tool bằng `--trigger-active-high`. Output
OK/NG/BUSY là active-HIGH. Nếu muốn test handshake giống app chính, thêm
`--hold-result-until-release` để giữ OK/NG cho tới khi trigger về LOW.

## Demo ThingsBoard MQTT + Live Image + RPC

Demo này mô phỏng một Jetson inspection gateway hoàn chỉnh:

- MQTT telemetry dạng structured JSON string cho kết quả inspect: key `jetson_inspection`.
- MQTT health telemetry dạng structured JSON string cho Jetson: key `jetson_health`.
- MQTT attributes dạng structured JSON string cho project/image endpoints: key `jetson_project`, `jetson_image`.
- HTTP image server trên Jetson: `/snapshot.jpg`, `/stream.mjpg`, `/health`.
- RPC từ ThingsBoard xuống Jetson: bật/tắt đèn demo, calibration mode, reset counter, snapshot, reboot/shutdown Jetson, ping.

Không gửi ảnh JPEG/base64 qua MQTT telemetry. ThingsBoard chỉ giữ URL ảnh, còn browser dashboard lấy ảnh trực tiếp từ Jetson. Cách này nhẹ hơn và phù hợp khi có nhiều thiết bị.

### Cài dependency trên Jetson/Ubuntu

```bash
sudo apt update
sudo apt install -y libmosquitto-dev
```

### Build demo

```bash
cd jetson-inspect-v2
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TB_MQTT_DEMO=ON
make -j$(nproc) thingsboard_mqtt_demo
```

### Chạy demo với ThingsBoard

Mỗi thiết bị trong ThingsBoard cần một Device credential riêng. Nếu dùng access token thì token là MQTT username và password để trống; nếu dùng MQTT Basic credentials thì dùng đúng username/password của device.

Demo mặc định là Device 1:

```text
host      = mqtt.thingsboard.cloud
port      = 1883
topic     = v1/devices/me/telemetry
client id = jetson-1
username  = jetson-1
password  = jetson-1
```

Vì vậy sau khi build, để test nhanh Device 1 chỉ cần set URL ảnh đúng IP Jetson rồi chạy:

```bash
export TB_IMAGE_PUBLIC_BASE_URL="http://JETSON_IP:8090"
./thingsboard_mqtt_demo
```

```bash
export TB_MQTT_HOST="thingsboard.example.com"
export TB_MQTT_PORT=1883
export TB_MQTT_ACCESS_TOKEN="YOUR_DEVICE_ACCESS_TOKEN"
export TB_MQTT_CLIENT_ID="jetson-inspect-line-01"
export TB_DEVICE_ID="jetson-inspect-line-01"

# URL mà máy mở dashboard ThingsBoard truy cập được.
# Đừng để 0.0.0.0 ở đây; dùng IP/hostname thật của Jetson trong LAN/VPN.
export TB_IMAGE_PUBLIC_BASE_URL="http://192.168.1.10:8090"

./thingsboard_mqtt_demo
```

Với MQTT Basic credentials, dùng username/password thay cho access token:

```bash
export TB_MQTT_HOST="mqtt.thingsboard.cloud"
export TB_MQTT_PORT=1883
export TB_MQTT_TOPIC="v1/devices/me/telemetry"

# Device 1
export TB_MQTT_CLIENT_ID="jetson-1"
export TB_MQTT_USERNAME="jetson-1"
export TB_MQTT_PASSWORD="jetson-1"
export TB_DEVICE_ID="jetson-1"
export TB_IMAGE_PUBLIC_BASE_URL="http://JETSON_IP:8090"

./thingsboard_mqtt_demo
```

```bash
export TB_MQTT_HOST="mqtt.thingsboard.cloud"
export TB_MQTT_PORT=1883
export TB_MQTT_TOPIC="v1/devices/me/telemetry"

# Device 2
export TB_MQTT_CLIENT_ID="test-device-2"
export TB_MQTT_USERNAME="test-device-2"
export TB_MQTT_PASSWORD="test-device-2"
export TB_DEVICE_ID="test-device-2"
export TB_IMAGE_PUBLIC_BASE_URL="http://JETSON_IP:8090"

./thingsboard_mqtt_demo
```

Biến môi trường hữu ích:

| Biến | Mặc định | Ý nghĩa |
|---|---:|---|
| `TB_MQTT_HOST` / `TB_HOST` | `localhost` | ThingsBoard MQTT host |
| `TB_MQTT_PORT` | `1883` | MQTT port, dùng `8883` nếu TLS |
| `TB_MQTT_ACCESS_TOKEN` / `TB_ACCESS_TOKEN` | rỗng | Device access token, dùng khi không set username/password |
| `TB_MQTT_USERNAME` / `TB_USERNAME` | access token | MQTT username nếu dùng MQTT Basic credentials |
| `TB_MQTT_PASSWORD` / `TB_PASSWORD` | rỗng | MQTT password nếu dùng MQTT Basic credentials |
| `TB_MQTT_CLIENT_ID` | hostname | MQTT client id, nên unique theo máy |
| `TB_MQTT_TOPIC` | `v1/devices/me/telemetry` | Telemetry topic ThingsBoard |
| `TB_MQTT_QOS` | `1` | QoS 0 hoặc 1 |
| `TB_MQTT_TLS` | `0` | Bật TLS |
| `TB_MQTT_CA_FILE` | rỗng | CA file khi dùng TLS |
| `TB_DEVICE_ID` | client id | ID hiển thị trong telemetry health |
| `TB_PROJECT_VERSION` | `demo` | Version app/project gửi lên attributes |
| `TB_LINE_ID` | `line-01` | Mã line sản xuất |
| `TB_STATION_ID` | `station-01` | Mã station/máy |
| `TB_IMAGE_BIND_HOST` | `0.0.0.0` | Interface HTTP image server bind |
| `TB_IMAGE_PORT` | `8090` | Port HTTP image server |
| `TB_IMAGE_PUBLIC_BASE_URL` | bind host + port | URL public cho ThingsBoard dashboard |
| `TB_IMAGE_JPEG_QUALITY` | `75` | JPEG quality snapshot/stream |
| `TB_IMAGE_STREAM_FPS` | `3` | FPS MJPEG stream |
| `TB_DEMO_INTERVAL_MS` | `1000` | Khoảng cách giữa 2 cycle demo |
| `TB_DEMO_CYCLES` | `0` | Số cycle demo, `0` là chạy liên tục |
| `TB_HEALTH_INTERVAL_CYCLES` | `5` | Gửi health sau mỗi N cycle |

Sau khi chạy, vào ThingsBoard → Device → Latest telemetry để xem các key gọn:

```text
jetson_inspection
jetson_health
```

Vào Device → Attributes để xem:

```text
jetson_project
jetson_image
```

Ben trong moi key la JSON string co structure day du, vi du `jetson_inspection`:

```json
{
  "schema": "jetson.inspect.v1",
  "device": {"id": "jetson-1"},
  "product": {"id": 1, "result": "OK", "is_ok": true, "is_ng": false},
  "counter": {"ok": 1, "ng": 0, "total": 1, "cycle_rate": 1.8},
  "process": {"app_state": "RESULT_SHOWN", "cycle_ms": 32.0, "info": "OK shape stable"},
  "metrics": {"available": true, "area": 5230.0, "spike_ratio": 0.021}
}
```

FE chi can nhap device ID, vi du `jetson-1`, roi query ThingsBoard device do va doc 4 key chinh: `jetson_inspection`, `jetson_health`, `jetson_project`, `jetson_image`. Moi key deu co `schema` va `device.id` de FE parse dung version ma khong can device gui nhieu key roi rac.

### Structure dữ liệu đề xuất

| Nhóm | Kênh | Tần suất | Nội dung |
|---|---|---:|---|
| Project/device info | Attributes | Khi app start / khi đổi config | `jetson_project` JSON string |
| Image endpoints | Attributes | Khi app start / khi IP đổi | `jetson_image` JSON string |
| Inspection result | Telemetry | Mỗi trigger | `jetson_inspection` JSON string |
| Jetson health | Telemetry | 5-30 giây/lần | `jetson_health` JSON string |
| Live image | HTTP từ Jetson | Dashboard pull | `/snapshot.jpg` hoặc `/stream.mjpg` |
| Control | Server-side RPC | Khi operator bấm nút | `setLight`, `setCalibrationMode`, `resetCounters`, `captureSnapshot`, `resetJetson`, `shutdownJetson`, `restartApp`, `ping` |

### Hiển thị ảnh trên ThingsBoard

1. Đảm bảo máy mở dashboard ThingsBoard truy cập được URL trong `TB_IMAGE_PUBLIC_BASE_URL`.
2. Test từ trình duyệt: `http://JETSON_IP:8090/snapshot.jpg` và `http://JETSON_IP:8090/stream.mjpg`.
3. Trên dashboard ThingsBoard, dùng widget HTML/iframe/image để trỏ tới `stream_url` hoặc `snapshot_url` nam trong attribute `jetson_image`.
4. Nếu ThingsBoard Cloud và Jetson nằm trong LAN riêng, cần VPN/reverse proxy/Tailscale/ZeroTier hoặc gateway để browser truy cập được Jetson.

### RPC control demo

Server-side RPC methods demo:

```json
{"method":"ping","params":{}}
{"method":"setLight","params":true}
{"method":"setCalibrationMode","params":true}
{"method":"resetCounters","params":{}}
{"method":"captureSnapshot","params":{}}
{"method":"resetJetson","params":{}}
{"method":"shutdownJetson","params":{}}
{"method":"restartApp","params":{}}
```

`resetJetson`/`shutdownJetson` chi thuc thi khi Jetson set `TB_ALLOW_POWER_RPC=1`. Khi tích hợp vào app thật, map RPC vào `VisionService`/`GPIOController`: `setLight` → `gpio.toggle_light()` hoặc command ON/OFF, `resetCounters` → `state.reset_counters()`, `setCalibrationMode` → `state.calibration_mode`, `restartApp` → `request_auto_restart()`.

Với hệ thống nhiều thiết bị, tạo nhiều Device trên ThingsBoard và cấp mỗi Jetson một token riêng. Không dùng chung token cho nhiều máy nếu cần tracking, alarm và dashboard độc lập.

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
