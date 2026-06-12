# Huong Dan Chay Demo ThingsBoard Tren Jetson

File nay huong dan cach build va chay demo ket noi Jetson voi ThingsBoard Cloud bang MQTT.

Demo mac dinh dang cau hinh la **device 1**:

```text
MQTT host  : mqtt.thingsboard.cloud
MQTT port  : 1883
Topic      : v1/devices/me/telemetry
Client ID  : jetson-1
Username   : jetson-1
Password   : jetson-1
Image port : 8090
```

## 1. Kien Truc Hoat Dong

```text
Jetson
  |-- MQTT telemetry/attributes/RPC --> ThingsBoard Cloud
  |-- HTTP snapshot/live stream ------> Browser dashboard
```

Jetson tu connect ra ThingsBoard. ThingsBoard khong can truy cap truc tiep vao Jetson de nhan telemetry.

Flow onboarding qua WiFi setup:

```text
Jetson phat WiFi AP JETSON-xxxxxx
  -> User/FE ket noi vao WiFi do
  -> Mo setup UI: http://<JETSON_SETUP_IP>:8090/setup
  -> FE lay MAC/device id chuan tu /device-info
  -> FE/setup UI goi local BE /tb/onboard de tao/map device ThingsBoard theo MAC
  -> User nhap WiFi nha xuong, vi du Phoenix CAFE144_5G
  -> Jetson tat setup AP va connect WiFi nha xuong
  -> Jetson chay MQTT len ThingsBoard
```

Browser web khong tu scan/ket noi WiFi duoc. User can ket noi WiFi Jetson truoc, hoac dung app native/mobile neu muon scan SSID tu dong.

Anh/live view khong gui qua MQTT. Jetson mo HTTP image server rieng:

```text
http://JETSON_IP:8090/snapshot.jpg
http://JETSON_IP:8090/stream.mjpg
http://JETSON_IP:8090/health
http://JETSON_IP:8090/device-info
http://JETSON_IP:8090/setup
http://JETSON_IP:8090/wifi/scan
http://JETSON_IP:8090/setup/status
http://JETSON_IP:8090/tb/onboard
```

ThingsBoard chi luu cac URL nay trong device attributes de dashboard hien thi anh.

## 2. Cai Dependency Tren Jetson

Chay tren Jetson:

```bash
sudo apt update
sudo apt install -y build-essential cmake libopencv-dev libmosquitto-dev mosquitto-clients curl
```

Kiem tra cac tool:

```bash
cmake --version
mosquitto_pub --help
```

Neu muon Jetson phat WiFi setup, can co NetworkManager/nmcli:

```bash
nmcli --version
```

Neu chua co:

```bash
sudo apt install -y network-manager
```

Luu y: neu Jetson chi co 1 WiFi card, khi card do phat AP thi co the khong dong thoi connect WiFi internet duoc. Khuyen nghi production:

```text
Ethernet = internet/MQTT len ThingsBoard
WiFi AP  = setup/local onboarding cho FE
```

## 3. Build Source

Vao thu muc source:

```bash
cd /path/to/jetson-inspect-v2
```

Build app chinh va demo ThingsBoard:

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TB_MQTT_DEMO=ON
make -j$(nproc)
```

Sau khi build xong se co 2 file quan trong:

```text
build/jetson_inspect_v2
build/thingsboard_mqtt_demo
```

Trong do:

```text
jetson_inspect_v2       = app inspection that voi camera/GPIO/HMI
thingsboard_mqtt_demo   = demo ket noi ThingsBoard, telemetry, health, image, RPC
```

Luu y: hien tai ThingsBoard moi nam trong `thingsboard_mqtt_demo`, chua cam truc tiep vao app inspection that.

## 4. Test MQTT Credential Truoc

Truoc khi chay demo C++, nen test MQTT bang `mosquitto_pub`.

### Test Device 1

```bash
mosquitto_pub \
  -h mqtt.thingsboard.cloud \
  -p 1883 \
  -i jetson-1 \
  -u jetson-1 \
  -P jetson-1 \
  -t v1/devices/me/telemetry \
  -m '{"mqtt_test":1,"message":"hello from jetson device 1"}'
```

Vao ThingsBoard:

```text
Devices -> jetson-1 -> Latest telemetry
```

Neu thay `mqtt_test = 1` thi MQTT credential dung.

### Test Device 2

```bash
mosquitto_pub \
  -h mqtt.thingsboard.cloud \
  -p 1883 \
  -i test-device-2 \
  -u test-device-2 \
  -P test-device-2 \
  -t v1/devices/me/telemetry \
  -m '{"mqtt_test":2,"message":"hello from jetson device 2"}'
```

Vao ThingsBoard:

```text
Devices -> test-device-2 -> Latest telemetry
```

## 5. Chay Demo Device 1

### 5.1 Bat WiFi setup AP tren Jetson

Kiem tra interface WiFi:

```bash
nmcli dev status
```

Neu interface la `wlan0`, chay:

```bash
cd /path/to/jetson-inspect-v2
sudo chmod +x scripts/setup_jetson_wifi_ap.sh
JETSON_WIFI_IFACE=wlan0 ./scripts/setup_jetson_wifi_ap.sh
```

Script se tao SSID theo 6 ky tu cuoi MAC, vi du:

```text
SSID       : JETSON-A1B2C3
Security   : wpa-psk
Password   : Jetson@123456
Gateway IP : <JETSON_SETUP_IP>
Device API : http://<JETSON_SETUP_IP>:8090/device-info
```

Neu muon WiFi setup **khong can mat khau** de dien thoai connect nhanh:

```bash
JETSON_SETUP_OPEN=1 JETSON_WIFI_IFACE=wlan0 ./scripts/setup_jetson_wifi_ap.sh
```

Output se co:

```text
Security   : open
Password   : none
```

Captive portal tu dong:

```text
Script setup AP se cau hinh DNS captive va redirect HTTP port 80 ve port 8090.
Dien thoai co the tu popup setup page sau khi connect WiFi JETSON-xxxxxx.
```

Dieu kien de tu popup:

```text
1. Demo HTTP server phai dang chay tren Jetson
2. Dien thoai connect/reconnect vao WiFi JETSON-xxxxxx sau khi server da chay
3. Trinh duyet/he dieu hanh cho phep captive portal popup
```

Neu dien thoai khong popup, mo thu cong:

```text
http://<JETSON_SETUP_IP>:8090/setup
```

Hoac mo mot trang HTTP bat ky nhu:

```text
http://neverssl.com
```

Neu danh sach WiFi trong setup page rong, nhap SSID thu cong. Mot so WiFi card khong scan on dinh khi dang phat AP bang chinh card do.

Luu y: open WiFi chi nen dung trong thoi gian onboarding/setup. Khi vao production, nen gioi han thoi gian bat AP hoac doi sang WPA password neu moi truong yeu cau bao mat.

Setup UI local:

```text
http://<JETSON_SETUP_IP>:8090/setup
```

API provisioning:

```text
GET  /device-info
GET  /wifi/scan
POST /wifi/connect       body: {"ssid":"Phoenix CAFE144_5G","password":"..."}
POST /tb/onboard         body: {"tbUrl":"https://thingsboard.cloud","jwt":"<TENANT_ADMIN_JWT>"}
GET  /setup/status
```

`/tb/onboard` la local BE nam trong `thingsboard_mqtt_demo`. Endpoint nay:

```text
1. Tao hoac tim ThingsBoard device theo ten mac dinh jetson-<6 ky tu cuoi MAC>
2. Set MQTT ACCESS_TOKEN cho device do
3. Ghi server attributes: mac_address, device_name, jetson_onboard
4. Luu cau hinh MQTT vao ~/.config/jetson-inspect-v2/thingsboard.env
```

Vi du goi onboarding bang curl:

```bash
curl -X POST http://<JETSON_SETUP_IP>:8090/tb/onboard \
  -H 'Content-Type: application/json' \
  -d '{
    "tbUrl":"https://thingsboard.cloud",
    "jwt":"YOUR_TENANT_ADMIN_JWT"
  }'
```

Response thanh cong:

```json
{
  "ok": true,
  "device": {
    "id": "THINGSBOARD_DEVICE_UUID",
    "name": "jetson-BE15F9",
    "mac": "58:02:05:be:15:f9"
  },
  "mqtt": {
    "host": "mqtt.thingsboard.cloud",
    "port": 1883,
    "client_id": "jetson-BE15F9",
    "username": "jetson-BE15F9",
    "access_token": "jetson-BE15F9"
  },
  "restart_required": true
}
```

Sau khi onboard, restart demo de no doc cau hinh MQTT moi tu file env:

```bash
pkill -f thingsboard_mqtt_demo || true
cd ~/jetson-inspect-v2/build
nohup ./thingsboard_mqtt_demo > ~/thingsboard_mqtt_demo.log 2>&1 &
```

Khong hardcode Tenant Admin JWT trong source. JWT chi nen duoc FE/app truyen vao luc setup, hoac backend that cua he thong goi thay cho FE neu chay production.

`/wifi/connect` tra response truoc, sau do Jetson doi mang trong background. Dien thoai se mat ket noi voi WiFi `JETSON-xxxxxx`, roi user connect lai WiFi nha xuong.

Neu `/wifi/connect` khong doi duoc WiFi do `nmcli` can sudo password, tao sudoers rieng:

```bash
printf '%s\n' \
'vvp ALL=(root) NOPASSWD: /usr/bin/nmcli' \
'vvp ALL=(root) NOPASSWD: /bin/nmcli' | sudo tee /etc/sudoers.d/jetson-inspect-network >/dev/null

sudo chmod 440 /etc/sudoers.d/jetson-inspect-network
sudo visudo -cf /etc/sudoers.d/jetson-inspect-network
```

Co the doi password/SSID:

```bash
export JETSON_SETUP_PASSWORD="YourStrongPassword123"
export JETSON_SETUP_SSID="JETSON-LINE01-01"
./scripts/setup_jetson_wifi_ap.sh
```

Tat AP khi can:

```bash
sudo nmcli con down jetson-setup-ap
```

Bat lai:

```bash
sudo nmcli con up jetson-setup-ap
```

### 5.2 Chay demo ThingsBoard

Lay IP LAN cua Jetson neu can public URL cho browser cung mang:

```bash
hostname -I
```

Vi du Jetson co IP LAN:

```text
<JETSON_LAN_IP>
```

Chay demo:

```bash
cd /path/to/jetson-inspect-v2/build

export TB_IMAGE_PUBLIC_BASE_URL="http://<JETSON_LAN_IP>:8090"

./thingsboard_mqtt_demo
```

Neu khong set `TB_IMAGE_PUBLIC_BASE_URL`, demo se tu detect IPv4 active tren Jetson. Neu Jetson co nhieu interface va detect sai, chi dinh ro interface:

```bash
export TB_IMAGE_INTERFACE="eth0"      # URL anh/live cho browser cung mang LAN
export JETSON_SETUP_IFACE="wlan0"     # WiFi AP/interface setup local
```

Hoac chi dinh host/base URL ro rang:

```bash
export TB_IMAGE_PUBLIC_HOST="<JETSON_LAN_IP>"
export JETSON_SETUP_API_BASE_URL="http://<JETSON_SETUP_IP>:8090"
```

Vi demo da mac dinh la device 1 nen khong can export MQTT credential nua.

Neu muon khai bao ro rang:

```bash
export TB_MQTT_HOST="mqtt.thingsboard.cloud"
export TB_MQTT_PORT=1883
export TB_MQTT_TOPIC="v1/devices/me/telemetry"
export TB_MQTT_CLIENT_ID="jetson-1"
export TB_MQTT_USERNAME="jetson-1"
export TB_MQTT_PASSWORD="jetson-1"
export TB_DEVICE_ID="jetson-1"
export TB_IMAGE_PUBLIC_BASE_URL="http://<JETSON_LAN_IP>:8090"

# Khong bat buoc set 2 bien nay. Neu khong set, demo se doc IP thuc te cua WiFi AP interface.
# Chi set khi ban da co subnet setup co dinh.
export JETSON_SETUP_IFACE="wlan0"
# export JETSON_SETUP_IP="<JETSON_SETUP_IP>"
# export JETSON_SETUP_API_BASE_URL="http://<JETSON_SETUP_IP>:8090"

./thingsboard_mqtt_demo
```

Khi chay thanh cong, terminal se co log dang:

```text
Image server started: http://<JETSON_REACHABLE_IP>:8090
ThingsBoard MQTT connected: mqtt.thingsboard.cloud:1883 topic=v1/devices/me/telemetry
Jetson setup identity: ssid=JETSON-A1B2C3 device_info=http://<JETSON_SETUP_IP>:8090/device-info mac=48:b0:2d:a1:b2:c3
Demo queued product=1 result=OK ok=1 ng=0 dropped=0
```

Test local API sau khi laptop/FE da connect WiFi Jetson:

```bash
curl http://<JETSON_SETUP_IP>:8090/device-info
curl http://<JETSON_SETUP_IP>:8090/wifi/scan
```

FE se lay MAC that tu response `device.mac`, khong can user nhap MAC bang tay.

Mo setup page tren dien thoai/laptop:

```text
http://<JETSON_SETUP_IP>:8090/setup
```

## 6. Chay Demo Device 2

Neu muon chay device 2:

```bash
cd /path/to/jetson-inspect-v2/build

export TB_MQTT_HOST="mqtt.thingsboard.cloud"
export TB_MQTT_PORT=1883
export TB_MQTT_TOPIC="v1/devices/me/telemetry"
export TB_MQTT_CLIENT_ID="test-device-2"
export TB_MQTT_USERNAME="test-device-2"
export TB_MQTT_PASSWORD="test-device-2"
export TB_DEVICE_ID="test-device-2"
export TB_IMAGE_PUBLIC_BASE_URL="http://<JETSON_LAN_IP>:8090"

./thingsboard_mqtt_demo
```

Neu chay device 1 va device 2 cung luc tren cung mot Jetson, phai doi port image cho device 2:

```bash
export TB_IMAGE_PORT=8091
export TB_IMAGE_PUBLIC_BASE_URL="http://<JETSON_LAN_IP>:8091"
```

## 7. Kiem Tra Data Tren ThingsBoard

Sau khi demo dang chay, vao ThingsBoard:

```text
Devices -> jetson-1 -> Latest telemetry
```

Ban se thay cac key gon:

```text
jetson_inspection
jetson_health
```

Neu truoc do da chay schema cu, ThingsBoard co the van hien cac key cu nhu `product_id`, `total_ok`, `area`, `cpu_temp_c`. Do la latest/history cu. Schema moi chi update cac key `jetson_inspection` va `jetson_health`.

Schema moi duoc thiet ke de FE chi can nhap device ID, vi du `jetson-1`, roi FE query ThingsBoard theo ID do va doc cac key chinh:

```text
Latest telemetry: jetson_inspection, jetson_health
Attributes      : jetson_project, jetson_image, jetson_identity
```

Ben trong moi JSON string deu co:

```json
{
  "schema": "jetson.inspect.v1",
  "device": {"id": "jetson-1", "mac": "48:b0:2d:xx:xx:xx"}
}
```

Nhu vay device khong can gui nhieu key roi rac len ThingsBoard, FE van biet data thuoc device nao, MAC nao va parse theo schema version nao.

Demo tu doc MAC tu `/sys/class/net/*/address`. Neu muon ep MAC dung interface cu the, set:

```bash
export TB_DEVICE_MAC="48:b0:2d:xx:xx:xx"
```

Gia tri cua moi key la JSON string co structure ben trong. Vi du `jetson_inspection`:

```json
{
  "product": {"id": 1, "result": "OK", "is_ok": true, "is_ng": false},
  "counter": {"ok": 1, "ng": 0, "total": 1, "cycle_rate": 1.8},
  "process": {"app_state": "RESULT_SHOWN", "cycle_ms": 32.0, "info": "OK shape stable"},
  "metrics": {"available": true, "area": 5230.0, "spike_ratio": 0.021}
}
```

Vi du `jetson_health`:

```json
{
  "device": {"id": "jetson-1", "hostname": "vvp-desktop"},
  "system": {"uptime_sec": 1234.5, "cpu_load_1m": 0.32, "mem_used_pct": 41.2},
  "services": {"camera_ok": true, "gpio_ok": true, "mqtt_connected": true}
}
```

Vao tab Attributes:

```text
Devices -> jetson-1 -> Attributes
```

Ban se thay cac key gon:

```text
jetson_project
jetson_image
jetson_identity
```

`jetson_image` chua cac endpoint:

```json
{
  "endpoints": {
    "snapshot_url": "http://<JETSON_REACHABLE_IP>:8090/snapshot.jpg",
    "stream_url": "http://<JETSON_REACHABLE_IP>:8090/stream.mjpg",
    "health_url": "http://<JETSON_REACHABLE_IP>:8090/health"
  },
  "settings": {"jpeg_quality": 75, "stream_fps": 3}
}
```

`jetson_identity` chua MAC va URL setup local:

```json
{
  "device": {"id": "jetson-1", "mac": "48:b0:2d:a1:b2:c3"},
  "setup": {
    "ssid": "JETSON-A1B2C3",
    "ip": "<JETSON_SETUP_IP>",
    "device_info_url": "http://<JETSON_SETUP_IP>:8090/device-info"
  }
}
```

## 8. Kiem Tra Anh Va Live Stream

Tu may dang mo dashboard, mo browser:

```text
http://<JETSON_REACHABLE_IP>:8090/snapshot.jpg
```

Live stream:

```text
http://<JETSON_REACHABLE_IP>:8090/stream.mjpg
```

Health HTTP:

```text
http://<JETSON_REACHABLE_IP>:8090/health
```

Device info/provisioning:

```text
http://<JETSON_REACHABLE_IP>:8090/device-info
```

Neu browser mo duoc cac URL nay thi ThingsBoard dashboard cung co the hien thi anh.

Quan trong:

```text
TB_IMAGE_PUBLIC_BASE_URL phai la URL ma may mo dashboard truy cap duoc.
Khong dung 0.0.0.0 trong URL hien thi anh.
```

Neu ThingsBoard Cloud o internet nhung Jetson nam trong LAN rieng, browser ben ngoai se khong mo duoc IP noi bo cua Jetson. Khi do can mot trong cac cach:

```text
VPN
Tailscale
ZeroTier
Reverse proxy
Port forwarding co bao mat
```

## 9. RPC Dieu Khien Tu ThingsBoard

Demo da subscribe RPC topic:

```text
v1/devices/me/rpc/request/+
```

Cac method demo:

```json
{"method":"ping","params":{}}
{"method":"setLight","params":true}
{"method":"setCalibrationMode","params":true}
{"method":"resetCounters","params":{}}
{"method":"captureSnapshot","params":{}}
{"method":"resetJetson","params":{}}
{"method":"rebootJetson","params":{}}
{"method":"shutdownJetson","params":{}}
{"method":"restartApp","params":{}}
```

Trong demo:

```text
ping                 -> tra pong
setLight             -> doi trang thai LIGHT ON/OFF tren frame demo
setCalibrationMode   -> doi trang thai RUN/CALIB tren frame demo
resetCounters        -> reset OK/NG/product_id demo
captureSnapshot      -> tra ve snapshot_url
resetJetson          -> reboot Jetson, can TB_ALLOW_POWER_RPC=1
rebootJetson         -> reboot Jetson, can TB_ALLOW_POWER_RPC=1
shutdownJetson       -> shutdown Jetson, can TB_ALLOW_POWER_RPC=1
restartApp           -> chi tra response demo_mode, khong restart that
```

De cho phep reboot/shutdown that tu ThingsBoard, chay demo voi:

```bash
export TB_ALLOW_POWER_RPC=1
```

Tai khoan chay app can duoc phep sudo khong hoi password cho lenh shutdown. Vi du tren Jetson:

```bash
sudo visudo -f /etc/sudoers.d/jetson-inspect-power
```

Them dong:

```text
vvp ALL=(root) NOPASSWD: /sbin/shutdown
```

Neu khong set `TB_ALLOW_POWER_RPC=1`, RPC se tra ve `power_rpc_disabled` va khong tat/reboot may.

Khi tich hop vao app that, map RPC nhu sau:

```text
setLight             -> GPIOController / ESP32 LIGHT_ON, LIGHT_OFF
setCalibrationMode   -> DetectionState.calibration_mode
resetCounters        -> DetectionState.reset_counters()
restartApp           -> VisionService.request_auto_restart()
```

## 10. Chay App Inspection That

Chay app chinh:

```bash
cd /path/to/jetson-inspect-v2/build
./jetson_inspect_v2
```

App nay chay camera/GPIO/HMI theo source hien tai.

Luu y:

```text
jetson_inspect_v2 hien chua gui data that len ThingsBoard.
thingsboard_mqtt_demo la demo de test ThingsBoard truoc.
```

Buoc tich hop tiep theo la cam `ThingsBoardMqttClient` vao `VisionService::process_trigger()`:

```text
VisionService::process_trigger()
  -> detect OK/NG
  -> signal PLC
  -> update HMI local
  -> enqueue telemetry len ThingsBoard
```

## 11. Troubleshooting

### Khong thay telemetry tren ThingsBoard

Kiem tra MQTT bang `mosquitto_pub`:

```bash
mosquitto_pub \
  -h mqtt.thingsboard.cloud \
  -p 1883 \
  -i jetson-1 \
  -u jetson-1 \
  -P jetson-1 \
  -t v1/devices/me/telemetry \
  -m '{"debug":1}'
```

Neu len ThingsBoard thi credential dung. Neu khong len, kiem tra:

```text
host/port dung chua
device credential dung chua
device tren ThingsBoard co dung type MQTT Basic credentials khong
mang Jetson co ra internet duoc khong
firewall co chan port 1883 khong
```

### Demo bao MQTT config invalid

Can co mot trong hai cach:

```bash
# Cach 1: Basic credentials
export TB_MQTT_USERNAME="jetson-1"
export TB_MQTT_PASSWORD="jetson-1"

# Cach 2: Access token
export TB_MQTT_ACCESS_TOKEN="YOUR_ACCESS_TOKEN"
```

Voi demo device 1, cac gia tri nay da co default san.

### Browser khong mo duoc stream.mjpg

Kiem tra Jetson IP:

```bash
hostname -I
```

Kiem tra port image:

```bash
curl -I http://<JETSON_REACHABLE_IP>:8090/snapshot.jpg
```

Neu chay tren may khac ma khong vao duoc, kiem tra firewall/LAN/VPN.

### Port 8090 bi trung

Doi port:

```bash
export TB_IMAGE_PORT=8091
export TB_IMAGE_PUBLIC_BASE_URL="http://<JETSON_REACHABLE_IP>:8091"
./thingsboard_mqtt_demo
```

### ThingsBoard cham hon HMI

Day la binh thuong. HMI/PLC chay local real-time, ThingsBoard la monitoring async.

Nguyen tac:

```text
PLC/HMI/inspection khong duoc doi ThingsBoard.
MQTT chi enqueue va gui bang thread rieng.
```

## 12. Lenh Chay Nhanh Device 1

```bash
cd /path/to/jetson-inspect-v2/build
export TB_IMAGE_PUBLIC_BASE_URL="http://$(hostname -I | awk '{print $1}'):8090"
./thingsboard_mqtt_demo
```

Neu len ThingsBoard thay telemetry va browser mo duoc `snapshot.jpg`, demo da OK.
