# FE Guide: Lay Du Lieu ThingsBoard Cho Jetson Inspect

File nay danh cho FE/backend can lay du lieu hien tai cua project `jetson-inspect-v2` tu ThingsBoard.

## 1. Tong Quan Flow

```text
Jetson -> MQTT -> ThingsBoard
FE     -> REST API -> ThingsBoard
```

Jetson gui data len ThingsBoard bang MQTT. FE khong connect MQTT truc tiep den Jetson. FE goi REST API cua ThingsBoard de lay data.

User/FE co the nhap device ID/name hoac MAC address. Khuyen nghi dung MAC address de phan biet thiet bi vat ly.

Flow onboarding moi de tranh nhap sai MAC:

```text
1. Jetson phat WiFi setup, vi du SSID JETSON-A1B2C3
2. User ket noi laptop/tablet vao WiFi cua Jetson
3. FE mo setup UI/local API: http://<JETSON_SETUP_IP>:8090/setup
4. FE goi /device-info de lay device.mac va device.id
5. FE goi /tb/onboard de Jetson local BE tao/map ThingsBoard device theo MAC
6. FE goi /wifi/scan, user chon WiFi nha xuong
7. FE goi /wifi/connect voi ssid/password
8. Jetson tat setup AP, connect WiFi nha xuong, sau do chay MQTT ThingsBoard bang device moi
```

WiFi setup co the de dang open/no password trong luc onboarding:

```bash
JETSON_SETUP_OPEN=1 JETSON_WIFI_IFACE=<JETSON_WIFI_IFACE> ./scripts/setup_jetson_wifi_ap.sh
```

Luu y: browser web khong duoc quyen tu scan/ket noi WiFi. User phai ket noi WiFi Jetson truoc, hoac dung app native/mobile neu muon scan SSID tu dong.

Neu FE la website tren internet, khi dien thoai connect WiFi Jetson open AP thi co the mat internet de tai trang FE. Production nen dung mot trong cac cach:

```text
1. FE setup la PWA/offline page da cache san tren dien thoai
2. Dung app mobile/native de scan/connect WiFi va goi /device-info
3. Jetson serve local setup page, vi du http://<JETSON_SETUP_IP>:8090/device-info hoac /setup
4. Jetson co internet rieng bang Ethernet/4G/USB tethering de phone van vao duoc FE/backend qua route phu hop
```

Captive portal auto popup:

```text
Jetson AP script cau hinh DNS captive va redirect port 80 ve local setup server port 8090.
Neu OS popup khong xuat hien, FE/user mo thu cong http://<JETSON_SETUP_IP>:8090/setup.
Neu user da connect WiFi truoc khi setup server chay, can tat/bat lai WiFi hoac reconnect SSID JETSON-xxxxxx de trigger popup lai.
```

Danh sach WiFi co the rong khi Jetson chi co 1 WiFi card va dang phat AP. UI/FE phai cho phep user nhap SSID thu cong.

Vi du device name:

```text
jetson-1
```

Vi du MAC address:

```text
48:b0:2d:xx:xx:xx
```

Sau do FE:

```text
1. Tim ThingsBoard device co name = jetson-1
2. Lay DEVICE_UUID
3. Goi API latest telemetry
4. Goi API attributes
5. JSON.parse() cac value dang string
6. Render UI
```

## 2. Schema Hien Tai Tren ThingsBoard

Project nay gui data len ThingsBoard theo schema gon, khong gui hang chuc key roi rac.

### Latest telemetry

```text
jetson_inspection
jetson_health
```

### Attributes

```text
jetson_project
jetson_image
jetson_identity
```

Tat ca key tren co value la **JSON string**. FE can `JSON.parse(value)` de dung.

## 2.1 Local Provisioning API

Sau khi user ket noi vao WiFi Jetson, FE goi:

```http
GET http://<JETSON_SETUP_IP>:8090/device-info
```

Setup UI co san tren Jetson:

```http
GET http://<JETSON_SETUP_IP>:8090/setup
```

Scan WiFi xung quanh:

```http
GET http://<JETSON_SETUP_IP>:8090/wifi/scan
```

Response mau:

```json
{
  "ok": true,
  "interface": "wlP1p1s0",
  "networks": [
    {"ssid": "Phoenix CAFE144_5G", "signal": 80, "security": "WPA2"}
  ]
}
```

Gui WiFi nha xuong cho Jetson:

```http
POST http://<JETSON_SETUP_IP>:8090/wifi/connect
Content-Type: application/json

{"ssid":"Phoenix CAFE144_5G","password":"79797979"}
```

Sau request nay Jetson se roi WiFi setup `JETSON-xxxxxx` va connect WiFi muc tieu. Dien thoai/FE se mat ket noi local la binh thuong.

Check status khi con ket noi local:

```http
GET http://<JETSON_SETUP_IP>:8090/setup/status
```

Response mau:

```json
{
  "schema": "jetson.provision.v1",
  "device": {
    "id": "jetson-1",
    "name": "jetson-1",
    "mac": "48:b0:2d:a1:b2:c3",
    "ssid": "JETSON-A1B2C3",
    "suggested_tb_device_name": "jetson-A1B2C3"
  },
  "project": {
    "name": "jetson-inspect-v2",
    "version": "demo",
    "mode": "thingsboard-demo"
  },
  "network": {
    "setup_ip": "<JETSON_SETUP_IP>",
    "local_api_base_url": "http://<JETSON_SETUP_IP>:8090",
    "device_info_url": "http://<JETSON_SETUP_IP>:8090/device-info",
    "setup_page_url": "http://<JETSON_SETUP_IP>:8090/setup",
    "wifi_scan_url": "http://<JETSON_SETUP_IP>:8090/wifi/scan",
    "wifi_connect_url": "http://<JETSON_SETUP_IP>:8090/wifi/connect",
    "tb_onboard_url": "http://<JETSON_SETUP_IP>:8090/tb/onboard",
    "snapshot_url": "http://<JETSON_SETUP_IP>:8090/snapshot.jpg",
    "stream_url": "http://<JETSON_SETUP_IP>:8090/stream.mjpg",
    "health_url": "http://<JETSON_SETUP_IP>:8090/health"
  },
  "thingsboard": {
    "mqtt_host": "mqtt.thingsboard.cloud",
    "mqtt_port": 1883,
    "mqtt_client_id": "jetson-1",
    "device_name": "jetson-1"
  }
}
```

FE khong can cho user nhap MAC. MAC chuan nam o:

```js
const mac = deviceInfo.device.mac;
```

Khong hardcode `<JETSON_SETUP_IP>` tren FE. Gia tri nay phai lay tu:

```text
1. URL Device API ma Jetson/script setup in ra
2. Native/mobile network API neu app co quyen doc gateway IP
3. QR code/label/provisioning page do Jetson serve trong qua trinh setup
```

Sau khi goi `/device-info`, FE dung cac URL trong response `network.*` va `public_endpoints.*` thay vi tu ghep IP bang tay.

Neu FE production chay tren HTTPS va browser chan request den `http://<JETSON_SETUP_IP>`, nen dung mot trong cac cach sau:

```text
1. Mo trang setup local do Jetson serve trong cung origin
2. Dung app native/mobile de scan WiFi va goi local API
3. Cho user bam nut mo URL `device_info_url` ma script Jetson in ra trong tab setup rieng
```

Jetson da tra CORS header va `Access-Control-Allow-Private-Network`, nhung chinh sach browser van co the khac nhau theo version/trinh duyet.

Project demo nay da co local BE tren Jetson, nen FE co the goi truc tiep Jetson trong luc dang ket noi WiFi setup:

```http
POST http://<JETSON_SETUP_IP>:8090/tb/onboard
Content-Type: application/json

{
  "tbUrl": "https://thingsboard.cloud",
  "jwt": "<TENANT_ADMIN_JWT>",
  "deviceName": "jetson-BE15F9",
  "accessToken": "jetson-BE15F9"
}
```

Trong do `deviceName` va `accessToken` la optional. Neu bo trong, Jetson tu dung ten `jetson-<6 ky tu cuoi MAC>`.

Response mau:

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
  "saved_env": {
    "ok": true,
    "path": "/home/vvp/.config/jetson-inspect-v2/thingsboard.env"
  },
  "restart_required": true
}
```

Sau response nay, ThingsBoard da co device va server attributes:

```text
mac_address
device_name
jetson_onboard
```

Luu y bao mat: khong nen hardcode Tenant Admin JWT vao source FE public. Demo setup local co the nhap JWT tam thoi. Production nen de app native/backend lay token ngan han hoac backend that cua he thong goi `/tb/onboard` thay cho browser public.

## 3. Vi Du Data

### jetson_inspection

```json
{
  "schema": "jetson.inspect.v1",
  "device": {"id": "jetson-1", "mac": "48:b0:2d:xx:xx:xx"},
  "product": {
    "id": 1,
    "result": "OK",
    "is_ok": true,
    "is_ng": false
  },
  "counter": {
    "ok": 1,
    "ng": 0,
    "total": 1,
    "cycle_rate": 1.8
  },
  "process": {
    "app_state": "RESULT_SHOWN",
    "cycle_ms": 32.0,
    "info": "OK shape stable"
  },
  "metrics": {
    "available": true,
    "area": 5230.0,
    "perimeter": 388.0,
    "solidity": 0.93,
    "width": 180,
    "height": 240,
    "spike_ratio": 0.021
  }
}
```

### jetson_health

```json
{
  "schema": "jetson.inspect.v1",
  "device": {
    "id": "jetson-1",
    "mac": "48:b0:2d:xx:xx:xx",
    "hostname": "vvp-desktop"
  },
  "system": {
    "uptime_sec": 1234.5,
    "cpu_load_1m": 0.32,
    "mem_used_pct": 41.2,
    "disk_used_pct": 55.0,
    "cpu_temp_c": 48.0
  },
  "services": {
    "camera_ok": true,
    "gpio_ok": true,
    "mqtt_connected": true,
    "image_server_ok": true,
    "mqtt_dropped": 0
  }
}
```

### jetson_project

```json
{
  "schema": "jetson.inspect.v1",
  "device": {"id": "jetson-1", "mac": "48:b0:2d:xx:xx:xx"},
  "project": {
    "name": "jetson-inspect-v2",
    "version": "demo",
    "mode": "thingsboard-demo"
  },
  "production": {
    "line_id": "line-01",
    "station_id": "station-01",
    "device_role": "vision-inspection"
  }
}
```

### jetson_image

```json
{
  "schema": "jetson.inspect.v1",
  "device": {"id": "jetson-1", "mac": "48:b0:2d:xx:xx:xx"},
  "endpoints": {
    "snapshot_url": "http://<JETSON_REACHABLE_IP>:8090/snapshot.jpg",
    "stream_url": "http://<JETSON_REACHABLE_IP>:8090/stream.mjpg",
    "health_url": "http://<JETSON_REACHABLE_IP>:8090/health"
  },
  "settings": {
    "jpeg_quality": 75,
    "stream_fps": 3
  }
}
```

### jetson_identity

```json
{
  "schema": "jetson.identity.v1",
  "device": {"id": "jetson-1", "mac": "48:b0:2d:a1:b2:c3"},
  "setup": {
    "ssid": "JETSON-A1B2C3",
    "ip": "<JETSON_SETUP_IP>",
    "device_info_url": "http://<JETSON_SETUP_IP>:8090/device-info"
  },
  "thingsboard": {
    "mqtt_host": "mqtt.thingsboard.cloud",
    "mqtt_client_id": "jetson-1"
  }
}
```

Luu y: `snapshot_url` va `stream_url` phai la URL ma browser cua user truy cap duoc. Neu user khac mang Jetson, can VPN/Tailscale/Cloudflare Tunnel/reverse proxy.

## 4. Auth ThingsBoard REST API

ThingsBoard REST API can header:

```http
X-Authorization: Bearer JWT_TOKEN
```

Hoac neu dung API Key:

```http
X-Authorization: ApiKey YOUR_API_KEY
```

Khuyen nghi production:

```text
FE -> Backend cua minh -> ThingsBoard
```

Khong nen de Tenant Admin password hoac API Key truc tiep trong frontend public.

## 5. Login Lay JWT Token

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"username":"YOUR_EMAIL","password":"YOUR_PASSWORD"}' \
  "https://thingsboard.cloud/api/auth/login"
```

Response:

```json
{
  "token": "JWT_TOKEN",
  "refreshToken": "REFRESH_TOKEN"
}
```

Dung token:

```http
X-Authorization: Bearer JWT_TOKEN
```

## 6. Tim Device UUID Tu Device Name

ThingsBoard telemetry API can `DEVICE_UUID`, khong phai name `jetson-1`.

### Cach 1: List devices va filter name

```bash
export TB_URL="https://thingsboard.cloud"
export AUTH="Bearer JWT_TOKEN"

curl -H "X-Authorization: $AUTH" \
  "$TB_URL/api/tenant/devices?pageSize=100&page=0"
```

Response mau:

```json
{
  "data": [
    {
      "id": {
        "entityType": "DEVICE",
        "id": "DEVICE_UUID"
      },
      "name": "jetson-1"
    }
  ]
}
```

FE tim theo name:

```js
const device = page.data.find(d => d.name === "jetson-1");
const deviceId = device.id.id;
```

Neu FE nhap MAC address, co 2 cach:

```text
Cach A: Dat ThingsBoard device name = MAC address, hoac label = MAC address.
Cach B: List devices -> lay attributes/telemetry tung device -> tim item co device.mac trung MAC.
```

Khuyen nghi production: khi tao device tren ThingsBoard, luu MAC vao device name/label hoac server-side attribute `mac_address` de FE resolve nhanh hon.

### Cach 2: Get device by name

Mot so version ThingsBoard ho tro endpoint:

```http
GET /api/tenant/devices?deviceName=jetson-1
```

Kiem tra tren Swagger cua instance:

```text
https://thingsboard.cloud/swagger-ui.html
```

## 7. Lay Latest Telemetry

Endpoint:

```http
GET /api/plugins/telemetry/DEVICE/{DEVICE_UUID}/values/timeseries?keys=jetson_inspection,jetson_health
```

Vi du curl:

```bash
curl -H "X-Authorization: $AUTH" \
  "$TB_URL/api/plugins/telemetry/DEVICE/$DEVICE_UUID/values/timeseries?keys=jetson_inspection,jetson_health"
```

Response mau:

```json
{
  "jetson_inspection": [
    {
      "ts": 1781000000000,
      "value": "{\"schema\":\"jetson.inspect.v1\",\"device\":{\"id\":\"jetson-1\",\"mac\":\"48:b0:2d:xx:xx:xx\"},\"product\":{\"id\":10,\"result\":\"OK\"}}"
    }
  ],
  "jetson_health": [
    {
      "ts": 1781000000000,
      "value": "{\"schema\":\"jetson.inspect.v1\",\"device\":{\"id\":\"jetson-1\",\"mac\":\"48:b0:2d:xx:xx:xx\"},\"system\":{\"cpu_load_1m\":0.3}}"
    }
  ]
}
```

Parse:

```js
const inspectionRaw = telemetry.jetson_inspection?.[0]?.value;
const healthRaw = telemetry.jetson_health?.[0]?.value;

const inspection = inspectionRaw ? JSON.parse(inspectionRaw) : null;
const health = healthRaw ? JSON.parse(healthRaw) : null;
```

## 8. Lay Attributes

Jetson publish attributes len ThingsBoard bang device MQTT attributes topic. Scope can doc la `CLIENT_SCOPE`.

Endpoint:

```http
GET /api/plugins/telemetry/DEVICE/{DEVICE_UUID}/values/attributes/CLIENT_SCOPE?keys=jetson_project,jetson_image,jetson_identity
```

Vi du curl:

```bash
curl -H "X-Authorization: $AUTH" \
  "$TB_URL/api/plugins/telemetry/DEVICE/$DEVICE_UUID/values/attributes/CLIENT_SCOPE?keys=jetson_project,jetson_image,jetson_identity"
```

Response mau:

```json
[
  {
    "lastUpdateTs": 1781000000000,
    "key": "jetson_project",
    "value": "{\"schema\":\"jetson.inspect.v1\",\"device\":{\"id\":\"jetson-1\",\"mac\":\"48:b0:2d:xx:xx:xx\"},\"project\":{\"name\":\"jetson-inspect-v2\"}}"
  },
  {
    "lastUpdateTs": 1781000000000,
    "key": "jetson_image",
    "value": "{\"schema\":\"jetson.inspect.v1\",\"device\":{\"id\":\"jetson-1\",\"mac\":\"48:b0:2d:xx:xx:xx\"},\"endpoints\":{\"stream_url\":\"http://<JETSON_REACHABLE_IP>:8090/stream.mjpg\"}}"
  },
  {
    "lastUpdateTs": 1781000000000,
    "key": "jetson_identity",
    "value": "{\"schema\":\"jetson.identity.v1\",\"device\":{\"id\":\"jetson-1\",\"mac\":\"48:b0:2d:a1:b2:c3\"},\"setup\":{\"ssid\":\"JETSON-A1B2C3\"}}"
  }
]
```

Parse:

```js
const attrsByKey = Object.fromEntries(
  attributes.map(item => [item.key, item.value])
);

const project = attrsByKey.jetson_project
  ? JSON.parse(attrsByKey.jetson_project)
  : null;

const image = attrsByKey.jetson_image
  ? JSON.parse(attrsByKey.jetson_image)
  : null;

const identity = attrsByKey.jetson_identity
  ? JSON.parse(attrsByKey.jetson_identity)
  : null;
```

## 9. JS Helper Mau

```js
const TB_URL = "https://thingsboard.cloud";

async function tbGet(path, authHeader) {
  const res = await fetch(`${TB_URL}${path}`, {
    headers: {
      "X-Authorization": authHeader,
      "Accept": "application/json"
    }
  });

  if (!res.ok) {
    throw new Error(`ThingsBoard API ${res.status}: ${await res.text()}`);
  }

  return res.json();
}

async function getDeviceIdByName(deviceName, authHeader) {
  const page = await tbGet(`/api/tenant/devices?pageSize=100&page=0`, authHeader);
  const device = page.data.find(d => d.name === deviceName);

  if (!device) {
    throw new Error(`Device not found: ${deviceName}`);
  }

  return device.id.id;
}

async function getJetsonData(deviceName, authHeader) {
  const deviceId = await getDeviceIdByName(deviceName, authHeader);

  const telemetry = await tbGet(
    `/api/plugins/telemetry/DEVICE/${deviceId}/values/timeseries` +
    `?keys=jetson_inspection,jetson_health`,
    authHeader
  );

  const attributes = await tbGet(
    `/api/plugins/telemetry/DEVICE/${deviceId}/values/attributes/CLIENT_SCOPE` +
    `?keys=jetson_project,jetson_image,jetson_identity`,
    authHeader
  );

  const inspection = telemetry.jetson_inspection?.[0]?.value
    ? JSON.parse(telemetry.jetson_inspection[0].value)
    : null;

  const health = telemetry.jetson_health?.[0]?.value
    ? JSON.parse(telemetry.jetson_health[0].value)
    : null;

  const attrsByKey = Object.fromEntries(attributes.map(a => [a.key, a.value]));

  const project = attrsByKey.jetson_project
    ? JSON.parse(attrsByKey.jetson_project)
    : null;

  const image = attrsByKey.jetson_image
    ? JSON.parse(attrsByKey.jetson_image)
    : null;

  const identity = attrsByKey.jetson_identity
    ? JSON.parse(attrsByKey.jetson_identity)
    : null;

  return {
    deviceName,
    deviceId,
    inspection,
    health,
    project,
    image,
    identity
  };
}

// Example:
// const data = await getJetsonData("jetson-1", "Bearer JWT_TOKEN");
// console.log(data.inspection.product.result);
// console.log(data.inspection.device.mac);
```

## 10. Historical Data

Lay lich su `jetson_inspection`:

```http
GET /api/plugins/telemetry/DEVICE/{DEVICE_UUID}/values/timeseries?keys=jetson_inspection&startTs=START_MS&endTs=END_MS&limit=100&agg=NONE&orderBy=DESC
```

Vi du JS:

```js
const endTs = Date.now();
const startTs = endTs - 60 * 60 * 1000;

const history = await tbGet(
  `/api/plugins/telemetry/DEVICE/${deviceId}/values/timeseries` +
  `?keys=jetson_inspection&startTs=${startTs}&endTs=${endTs}` +
  `&limit=100&agg=NONE&orderBy=DESC`,
  authHeader
);
```

Moi item can parse:

```js
const rows = history.jetson_inspection.map(item => ({
  ts: item.ts,
  data: JSON.parse(item.value)
}));
```

## 11. RPC Dieu Khien Jetson

Endpoint two-way RPC:

```http
POST /api/rpc/twoway/{DEVICE_UUID}
```

Bat/tat den demo:

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -H "X-Authorization: $AUTH" \
  -d '{"method":"setLight","params":true}' \
  "$TB_URL/api/rpc/twoway/$DEVICE_UUID"
```

Reset counter:

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -H "X-Authorization: $AUTH" \
  -d '{"method":"resetCounters","params":{}}' \
  "$TB_URL/api/rpc/twoway/$DEVICE_UUID"
```

Reboot Jetson:

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -H "X-Authorization: $AUTH" \
  -d '{"method":"resetJetson","params":{}}' \
  "$TB_URL/api/rpc/twoway/$DEVICE_UUID"
```

Shutdown Jetson:

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -H "X-Authorization: $AUTH" \
  -d '{"method":"shutdownJetson","params":{}}' \
  "$TB_URL/api/rpc/twoway/$DEVICE_UUID"
```

Cac method demo hien co:

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

Luu y: `resetJetson`, `rebootJetson`, `shutdownJetson` chi thuc thi neu phia Jetson set:

```bash
export TB_ALLOW_POWER_RPC=1
```

Neu chua bat, response se co:

```json
{"ok": false, "error": "power_rpc_disabled"}
```

## 12. Image Stream Tren FE

Lay URL tu:

```js
const streamUrl = data.image.endpoints.stream_url;
const snapshotUrl = data.image.endpoints.snapshot_url;
```

Render MJPEG stream:

```html
<img src={image.endpoints.stream_url} />
```

Hoac React:

```jsx
function JetsonStream({ image }) {
  if (!image?.endpoints?.stream_url) return null;
  return <img src={image.endpoints.stream_url} alt="Jetson live stream" />;
}
```

Luu y quan trong:

```text
Neu FE/browser khac mang Jetson, URL IP noi bo cua Jetson se khong mo duoc.
Can dung Tailscale, VPN, Cloudflare Tunnel, reverse proxy, hoac public URL co bao mat.
```

## 13. Error Handling De Xuat

FE nen handle cac truong hop:

```text
Device name khong ton tai
JWT/API key het han
Telemetry key chua co data
JSON.parse fail do schema cu
schema khac jetson.inspect.v1
stream_url khong truy cap duoc tu browser
```

Vi du check schema:

```js
if (inspection?.schema !== "jetson.inspect.v1") {
  console.warn("Unsupported Jetson schema", inspection?.schema);
}
```

## 14. Ghi Chu Ve Key Cu

Neu ThingsBoard tung nhan schema cu, trong Latest telemetry co the van thay cac key cu nhu:

```text
product_id
total_ok
area
cpu_temp_c
```

FE cua project nay nen bo qua cac key cu do va chi dung:

```text
jetson_inspection
jetson_health
jetson_project
jetson_image
jetson_identity
```
