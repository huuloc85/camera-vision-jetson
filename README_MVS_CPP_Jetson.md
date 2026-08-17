# MVS SDK – Lấy dữ liệu ảnh từ camera Hikrobot GigE bằng C++

Tài liệu này hướng dẫn lấy frame ảnh từ camera Hikrobot GigE trên **Jetson Nano B01 / Ubuntu ARM64** bằng **Hikrobot MVS SDK 3.0.1** để dùng cho dự án Computer Vision C++.

> MVS = Machine Vision Software của Hikrobot. Trong project, dùng SDK/API C++ của MVS để lấy frame từ camera; sau đó có thể đưa buffer sang OpenCV hoặc pipeline xử lý vision.

---

## 1. Kiến trúc hệ thống

```text
Hikrobot GigE Camera
        │
        │ Ethernet / GigE
        ▼
Jetson Nano B01
        │
        ├── MVS SDK
        │     /opt/MVS
        │
        └── C++ Vision Application
                 │
                 ├── OpenCV
                 ├── Image Processing
                 ├── AI / Deep Learning
                 └── Save / Stream / Inspect
```

Không cần Internet khi camera và Jetson kết nối trực tiếp qua Ethernet.

Ví dụ cấu hình:

```text
Jetson eth0 : 192.168.0.100/24
Camera      : 192.168.0.x/24
```

---

## 2. MVS SDK đã cài ở đâu?

Sau khi cài package ARM64:

```bash
sudo dpkg -i MVS-3.0.1_aarch64_20251113.deb
```

SDK nằm tại:

```text
/opt/MVS
```

Các thư mục quan trọng:

```text
/opt/MVS/include    # Header C/C++
/opt/MVS/lib        # Library
/opt/MVS/bin        # Executable và runtime library
/opt/MVS/Samples    # Sample code
/opt/MVS/driver     # Driver
```

Các header chính:

```text
/opt/MVS/include/MvCameraControl.h
/opt/MVS/include/CameraParams.h
/opt/MVS/include/PixelType.h
/opt/MVS/include/MvErrorDefine.h
```

Kiểm tra:

```bash
ls /opt/MVS/include
ls /opt/MVS/lib
ls /opt/MVS/Samples
```

---

## 3. Environment

Trong terminal:

```bash
export LD_LIBRARY_PATH=/opt/MVS/lib:/opt/MVS/bin:$LD_LIBRARY_PATH
```

Có thể thêm vào `~/.bashrc`:

```bash
echo 'export LD_LIBRARY_PATH=/opt/MVS/lib:/opt/MVS/bin:$LD_LIBRARY_PATH' >> ~/.bashrc
source ~/.bashrc
```

Kiểm tra:

```bash
echo $LD_LIBRARY_PATH
```

---

## 4. Luồng xử lý chuẩn của MVS SDK

Một chương trình vision cơ bản sẽ có flow:

```text
Enumerate devices
      ↓
Create camera handle
      ↓
Open camera
      ↓
Configure camera
      ↓
Start grabbing
      ↓
Get image buffer
      ↓
Process / convert image
      ↓
Release image buffer
      ↓
Stop grabbing
      ↓
Close camera
      ↓
Destroy handle
```

Các API MVS thường dùng:

```cpp
MV_CC_EnumDevices()
MV_CC_CreateHandle()
MV_CC_OpenDevice()
MV_CC_StartGrabbing()
MV_CC_GetImageBuffer()
MV_CC_FreeImageBuffer()
MV_CC_StopGrabbing()
MV_CC_CloseDevice()
MV_CC_DestroyHandle()
```

---

# 5. Ví dụ C++ tối thiểu: lấy frame từ camera

Tạo:

```bash
mkdir -p ~/vision_mvs/src
cd ~/vision_mvs
nano src/main.cpp
```

Nội dung:

```cpp
#include <iostream>
#include <cstring>

#include "MvCameraControl.h"

int main()
{
    int nRet = MV_OK;

    // ------------------------------------------------------------
    // 1. Enumerate camera
    // ------------------------------------------------------------
    MV_CC_DEVICE_INFO_LIST deviceList;
    std::memset(&deviceList, 0, sizeof(deviceList));

    nRet = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &deviceList);

    if (nRet != MV_OK)
    {
        std::cerr << "MV_CC_EnumDevices failed: 0x"
                  << std::hex << nRet << std::endl;
        return -1;
    }

    std::cout << "Found " << deviceList.nDeviceNum
              << " device(s)." << std::endl;

    if (deviceList.nDeviceNum == 0)
    {
        std::cerr << "No camera found." << std::endl;
        return -1;
    }

    // ------------------------------------------------------------
    // 2. Create handle
    // ------------------------------------------------------------
    void* handle = nullptr;

    nRet = MV_CC_CreateHandle(
        &handle,
        deviceList.pDeviceInfo[0]
    );

    if (nRet != MV_OK)
    {
        std::cerr << "MV_CC_CreateHandle failed: 0x"
                  << std::hex << nRet << std::endl;
        return -1;
    }

    // ------------------------------------------------------------
    // 3. Open camera
    // ------------------------------------------------------------
    nRet = MV_CC_OpenDevice(handle);

    if (nRet != MV_OK)
    {
        std::cerr << "MV_CC_OpenDevice failed: 0x"
                  << std::hex << nRet << std::endl;

        MV_CC_DestroyHandle(handle);
        return -1;
    }

    std::cout << "Camera opened." << std::endl;

    // ------------------------------------------------------------
    // 4. Start grabbing
    // ------------------------------------------------------------
    nRet = MV_CC_StartGrabbing(handle);

    if (nRet != MV_OK)
    {
        std::cerr << "MV_CC_StartGrabbing failed: 0x"
                  << std::hex << nRet << std::endl;

        MV_CC_CloseDevice(handle);
        MV_CC_DestroyHandle(handle);
        return -1;
    }

    std::cout << "Start grabbing." << std::endl;

    // ------------------------------------------------------------
    // 5. Get one frame
    // ------------------------------------------------------------
    MV_FRAME_OUT frame;
    std::memset(&frame, 0, sizeof(frame));

    nRet = MV_CC_GetImageBuffer(handle, &frame, 1000);

    if (nRet == MV_OK)
    {
        std::cout << "Frame received!" << std::endl;

        std::cout << "Width      : "
                  << frame.stFrameInfo.nWidth << std::endl;

        std::cout << "Height     : "
                  << frame.stFrameInfo.nHeight << std::endl;

        std::cout << "PixelType  : 0x"
                  << std::hex
                  << frame.stFrameInfo.enPixelType
                  << std::dec << std::endl;

        std::cout << "FrameNum   : "
                  << frame.stFrameInfo.nFrameNum << std::endl;

        std::cout << "ImageSize  : "
                  << frame.stFrameInfo.nFrameLen << " bytes"
                  << std::endl;

        // --------------------------------------------------------
        // TODO:
        // Process frame.pBufAddr here.
        //
        // frame.pBufAddr:
        //     pointer tới image buffer
        //
        // frame.stFrameInfo:
        //     metadata của frame
        // --------------------------------------------------------

        nRet = MV_CC_FreeImageBuffer(handle, &frame);

        if (nRet != MV_OK)
        {
            std::cerr << "MV_CC_FreeImageBuffer failed: 0x"
                      << std::hex << nRet << std::endl;
        }
    }
    else
    {
        std::cerr << "MV_CC_GetImageBuffer failed: 0x"
                  << std::hex << nRet << std::endl;
    }

    // ------------------------------------------------------------
    // 6. Stop grabbing
    // ------------------------------------------------------------
    MV_CC_StopGrabbing(handle);

    // ------------------------------------------------------------
    // 7. Close camera
    // ------------------------------------------------------------
    MV_CC_CloseDevice(handle);

    // ------------------------------------------------------------
    // 8. Destroy handle
    // ------------------------------------------------------------
    MV_CC_DestroyHandle(handle);

    std::cout << "Camera closed." << std::endl;

    return 0;
}
```

> Lưu ý quan trọng: sau khi dùng `MV_CC_GetImageBuffer()`, phải gọi `MV_CC_FreeImageBuffer()` để trả buffer về SDK.

---

# 6. CMakeLists.txt

Tạo:

```bash
nano CMakeLists.txt
```

Nội dung:

```cmake
cmake_minimum_required(VERSION 3.10)

project(vision_mvs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(vision_mvs
    src/main.cpp
)

target_include_directories(vision_mvs PRIVATE
    /opt/MVS/include
)

target_link_directories(vision_mvs PRIVATE
    /opt/MVS/lib
    /opt/MVS/bin
)

target_link_libraries(vision_mvs
    MvCameraControl
)

set_target_properties(vision_mvs PROPERTIES
    BUILD_RPATH "/opt/MVS/lib;/opt/MVS/bin"
)
```

Build:

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

Chạy:

```bash
./vision_mvs
```

Nếu runtime không tìm thấy library:

```bash
export LD_LIBRARY_PATH=/opt/MVS/lib:/opt/MVS/bin:$LD_LIBRARY_PATH
./vision_mvs
```

---

# 7. Kiểm tra camera trước khi chạy C++

Có thể dùng MVS Client:

```bash
/opt/MVS/bin/MVS.sh
```

Nếu chạy GUI từ SSH nhưng muốn hiển thị trên màn hình Jetson:

```bash
export DISPLAY=:0
export LD_LIBRARY_PATH=/opt/MVS/lib:/opt/MVS/bin:$LD_LIBRARY_PATH

/opt/MVS/bin/MVS.sh
```

Camera GigE phải xuất hiện trong danh sách device.

---

# 8. Lấy frame liên tục

Trong dự án vision thực tế, không nên chỉ lấy một frame.

Flow sẽ là:

```text
Camera
  │
  ▼
MV_CC_GetImageBuffer()
  │
  ▼
Image Buffer
  │
  ├── Convert pixel format
  │
  ├── OpenCV cv::Mat
  │
  ├── AI inference
  │
  └── Save / display
  │
  ▼
MV_CC_FreeImageBuffer()
  │
  └── lấy frame tiếp theo
```

Ví dụ loop:

```cpp
while (running)
{
    MV_FRAME_OUT frame;
    std::memset(&frame, 0, sizeof(frame));

    int ret = MV_CC_GetImageBuffer(handle, &frame, 1000);

    if (ret == MV_OK)
    {
        // Process image
        processImage(
            frame.pBufAddr,
            frame.stFrameInfo
        );

        MV_CC_FreeImageBuffer(handle, &frame);
    }
}
```

---

# 9. Quan trọng: không giữ buffer quá lâu

`frame.pBufAddr` thuộc buffer do MVS quản lý.

Không nên:

```cpp
MV_CC_GetImageBuffer(...);

savePointerSomewhere(frame.pBufAddr);

// tiếp tục dùng pointer sau FreeImageBuffer()
MV_CC_FreeImageBuffer(...);
```

Sau:

```cpp
MV_CC_FreeImageBuffer()
```

buffer có thể được SDK tái sử dụng cho frame tiếp theo.

Nếu cần giữ ảnh lâu hơn, hãy copy dữ liệu sang buffer riêng:

```cpp
std::vector<unsigned char> image(
    frame.stFrameInfo.nFrameLen
);

std::memcpy(
    image.data(),
    frame.pBufAddr,
    frame.stFrameInfo.nFrameLen
);
```

---

# 10. Đưa ảnh vào OpenCV

MVS có thể trả nhiều pixel format khác nhau.

Không nên mặc định mọi camera đều là:

```cpp
CV_8UC1
```

hoặc:

```cpp
CV_8UC3
```

Phải kiểm tra:

```cpp
frame.stFrameInfo.enPixelType
```

Ví dụ với ảnh Mono8:

```cpp
cv::Mat image(
    frame.stFrameInfo.nHeight,
    frame.stFrameInfo.nWidth,
    CV_8UC1,
    frame.pBufAddr,
    frame.stFrameInfo.nWidth
);
```

Nếu cần giữ ảnh sau khi release buffer:

```cpp
cv::Mat imageCopy = image.clone();

MV_CC_FreeImageBuffer(handle, &frame);
```

`imageCopy` lúc này sở hữu vùng nhớ riêng.

---

# 11. Nếu camera trả Bayer / RGB / YUV

Đây là phần cần chú ý khi làm Computer Vision.

Có thể gặp:

```text
Mono8
Mono10
Mono12
BayerRG8
BayerGB8
BayerGR8
BayerBG8
RGB8
BGR8
YUV
```

Không nên tự đoán format.

Kiểm tra:

```cpp
frame.stFrameInfo.enPixelType
```

Nếu cần chuyển pixel format, xem API trong:

```text
/opt/MVS/include/MvCameraControl.h
/opt/MVS/include/PixelType.h
```

và các sample tương ứng trong:

```text
/opt/MVS/Samples
```

Mục tiêu nên là:

```text
MVS raw frame
      ↓
Pixel conversion
      ↓
OpenCV cv::Mat
      ↓
Vision algorithm
```

---

# 12. Kiến trúc project nên dùng

Khi project bắt đầu lớn, không nên nhét toàn bộ MVS code vào `main.cpp`.

Có thể tổ chức:

```text
vision_project/
├── CMakeLists.txt
├── README.md
│
├── include/
│   ├── MvsCamera.hpp
│   ├── Frame.hpp
│   └── VisionPipeline.hpp
│
├── src/
│   ├── main.cpp
│   ├── MvsCamera.cpp
│   └── VisionPipeline.cpp
│
├── config/
│   └── camera.yaml
│
├── samples/
│
└── build/
```

Tách camera layer:

```text
MvsCamera
   │
   ├── connect()
   ├── start()
   ├── grab()
   ├── stop()
   └── disconnect()
```

Vision layer:

```text
VisionPipeline
   │
   ├── preprocess()
   ├── detect()
   ├── inspect()
   └── postprocess()
```

Như vậy sau này có thể thay camera hoặc thay thuật toán mà không phải sửa toàn bộ project.

---

# 13. Cách kiểm tra lỗi MVS

Nếu log có dạng:

```text
MVS GigE devices found: 1
MVS OpenDevice failed: 0x80000203
```

thì MVS đã enumerate được camera nhưng camera đang từ chối quyền exclusive
(`MV_E_ACCESS_DENIED`). Đóng hoàn toàn MVS Client/Ip Configurator và dừng các
tiến trình ứng dụng camera trước khi chạy production:

```bash
pkill -TERM -x jetson_inspect_v2 2>/dev/null || true
pkill -TERM -x MVS 2>/dev/null || true
pkill -TERM -x Ip_Configurator 2>/dev/null || true
pgrep -af 'jetson_inspect|MVS|Ip_Configurator|MvCamera' || true
```

Nếu vẫn bị từ chối, thoát MVS Client bằng giao diện hoặc power-cycle camera để
giải phóng quyền điều khiển GigE. Không chạy nhiều chương trình cùng mở camera.

Chỉ xác nhận camera hoạt động khi log có đủ:

```text
Camera started via Hikrobot MVS GigE (native camera settings)
MVS real frame: <width>x<height> pixel=... bytes=... frame=...
```

Các API MVS trả về mã lỗi:

```cpp
int ret = MV_CC_StartGrabbing(handle);

if (ret != MV_OK)
{
    std::cerr
        << "MVS error: 0x"
        << std::hex
        << ret
        << std::endl;
}
```

Không nên chỉ kiểm tra:

```cpp
if (ret)
```

nên dùng:

```cpp
if (ret != MV_OK)
```

và in mã lỗi hexadecimal.

---

# 14. Kiểm tra camera GigE trên Jetson

Kiểm tra interface:

```bash
ip addr show eth0
```

Ví dụ:

```text
eth0
inet 192.168.0.100/24
```

Kiểm tra link:

```bash
sudo ethtool eth0
```

Cần:

```text
Speed: 1000Mb/s
Duplex: Full
Link detected: yes
```

Camera và Jetson nên cùng subnet:

```text
Jetson:
192.168.0.100/24

Camera:
192.168.0.xxx/24
```

Không cần Internet.

---

# 15. Sample code là nguồn tham khảo quan trọng nhất

MVS SDK đã cài sample tại:

```bash
find /opt/MVS/Samples -maxdepth 3 -type f | head -100
```

Tìm sample liên quan camera:

```bash
find /opt/MVS/Samples -iname '*Grab*' -o -iname '*Image*' -o -iname '*Camera*'
```

Đọc:

```bash
grep -R "MV_CC_GetImageBuffer" /opt/MVS/Samples 2>/dev/null
```

Đây là cách tốt nhất để đối chiếu API chính xác với **version MVS 3.0.1_aarch64** đang cài trên Jetson.

---

# 16. Checklist trước khi phát triển Vision

- [x] Jetson Nano B01 ARM64
- [x] MVS ARM64 đã cài
- [x] `/opt/MVS` tồn tại
- [x] Camera GigE được kết nối
- [x] `eth0` hoạt động
- [x] Ethernet 1 Gbps Full Duplex
- [x] Camera xuất hiện trong MVS/Ip Configurator
- [ ] C++ enumerate được camera
- [ ] C++ open được camera
- [ ] C++ nhận được frame
- [ ] Xác định pixel format
- [ ] Convert sang OpenCV nếu cần
- [ ] Xây dựng Vision Pipeline
- [ ] Tối ưu FPS / latency / memory

---

# 17. Mục tiêu cuối cùng

Pipeline production nên hướng tới:

```text
Hikrobot GigE
     │
     │ image frame
     ▼
MVS SDK
     │
     ▼
MvsCamera
     │
     ▼
OpenCV cv::Mat
     │
     ▼
Pre-processing
     │
     ▼
Vision / AI
     │
     ├── Detection
     ├── Classification
     ├── OCR
     ├── Measurement
     └── Inspection
     │
     ▼
Result
```

MVS chịu trách nhiệm **giao tiếp và lấy ảnh từ camera**.

OpenCV / CUDA / TensorRT / model AI chịu trách nhiệm **xử lý ảnh và vision**.

Đây là cách nên thiết kế để dự án có thể mở rộng về sau.
