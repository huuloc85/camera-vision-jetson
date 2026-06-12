// -*- coding: utf-8 -*-
// telemetry/image_http_server.cpp — Lightweight snapshot/MJPEG server
#include "telemetry/image_http_server.h"
#include "core/logger.h"

#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace telemetry {
namespace {

std::string getenv_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value && value[0] ? value : fallback;
}

int getenv_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || !value[0]) return fallback;
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    return end && *end == '\0' ? static_cast<int>(parsed) : fallback;
}

std::string make_base_url(const std::string& host, int port) {
    std::ostringstream os;
    os << "http://" << host << ":" << port;
    return os.str();
}

bool usable_ipv4(const std::string& ip) {
    return !ip.empty() && ip != "0.0.0.0" && ip.rfind("127.", 0) != 0;
}

std::string detect_ipv4(const std::string& preferred_iface) {
    ifaddrs* addrs = nullptr;
    if (getifaddrs(&addrs) != 0 || !addrs) return "";

    auto find_ip = [&](const std::string& iface) {
        for (ifaddrs* it = addrs; it; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
            if ((it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0) continue;
            if (!iface.empty() && iface != it->ifa_name) continue;

            char buf[INET_ADDRSTRLEN] = {0};
            const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
            if (!inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) continue;
            std::string ip = buf;
            if (usable_ipv4(ip)) return ip;
        }
        return std::string();
    };

    std::string ip = find_ip(preferred_iface);
    if (ip.empty()) ip = find_ip("");
    freeifaddrs(addrs);
    return ip;
}

int send_flags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

bool send_all(int fd, const void* data, std::size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        ssize_t n = send(fd, p, size, send_flags());
        if (n <= 0) return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool send_text(int fd, const std::string& text) {
    return send_all(fd, text.data(), text.size());
}

std::string cors_headers() {
    return "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With\r\n"
           "Access-Control-Allow-Private-Network: true\r\n";
}

std::string first_request_method(const std::string& req) {
    std::istringstream in(req);
    std::string method;
    in >> method;
    return method.empty() ? "GET" : method;
}

std::string first_request_path(const std::string& req) {
    std::istringstream in(req);
    std::string method;
    std::string path;
    in >> method >> path;
    return path.empty() ? "/" : path;
}

std::size_t content_length(const std::string& req) {
    std::string lower = req;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::string key = "content-length:";
    std::size_t pos = lower.find(key);
    if (pos == std::string::npos) return 0;
    pos += key.size();
    while (pos < lower.size() && std::isspace(static_cast<unsigned char>(lower[pos]))) pos++;
    return static_cast<std::size_t>(std::strtoul(lower.c_str() + pos, nullptr, 10));
}

std::string request_body(const std::string& req) {
    std::size_t pos = req.find("\r\n\r\n");
    if (pos == std::string::npos) return "";
    return req.substr(pos + 4);
}

bool is_setup_api_path(const std::string& path) {
    return path == "/wifi/scan" || path == "/wifi/connect" || path == "/setup/status" ||
           path == "/tb/onboard" || path == "/api/command" || path == "/command" ||
           path == "/api/telemetry" || path == "/telemetry";
}

bool is_captive_probe_path(const std::string& path) {
    return path == "/generate_204" || path == "/gen_204" ||
           path == "/hotspot-detect.html" || path == "/library/test/success.html" ||
           path == "/connecttest.txt" || path == "/redirect" ||
           path == "/ncsi.txt" || path == "/canonical.html";
}

std::string setup_page_html() {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Jetson Setup</title>
  <style>
    :root{color-scheme:light;--ink:#151923;--muted:#6a7380;--line:#dfe5ee;--panel:#fff;--page:#f3f6fa;--blue:#1769e0;--green:#15803d;--red:#b42318;--amber:#a15c00;--shadow:0 16px 38px rgba(21,25,35,.10)}
    *{box-sizing:border-box}body{margin:0;background:var(--page);color:var(--ink);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}main{max-width:640px;margin:0 auto;padding:18px 14px 32px}h1{font-size:25px;margin:0 0 6px;line-height:1.15}h2{font-size:16px;margin:0}.lead,.hint{color:var(--muted);line-height:1.45}.lead{margin:0}.hint{font-size:14px;margin:10px 0 0}.top{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;margin-bottom:14px}.badge{display:flex;align-items:center;gap:8px;border:1px solid #cfead8;background:#ecf9f0;color:#126d33;border-radius:999px;padding:8px 10px;font-size:13px;font-weight:800;white-space:nowrap}.dot{width:8px;height:8px;border-radius:999px;background:#1aa34a;box-shadow:0 0 0 5px rgba(26,163,74,.13)}
    .panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;box-shadow:var(--shadow);padding:15px;margin-top:12px}.device{display:grid;grid-template-columns:auto 1fr;gap:7px 12px;margin-top:10px}.device span{color:var(--muted)}code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:13px;word-break:break-all}.toolbar{display:grid;grid-template-columns:1fr auto;gap:9px;margin-top:13px}.search{position:relative}.search input{padding-left:38px}.search:before{content:"";position:absolute;left:14px;top:50%;width:13px;height:13px;border:2px solid #8290a3;border-radius:50%;transform:translateY(-55%)}.search:after{content:"";position:absolute;left:27px;top:29px;width:8px;height:2px;background:#8290a3;transform:rotate(45deg);border-radius:4px}
    input{width:100%;border:1px solid #cbd3df;border-radius:8px;background:#fff;font-size:16px;padding:12px;outline:none}input:focus{border-color:#86b7ff;box-shadow:0 0 0 3px rgba(23,105,224,.14)}button{border:0;border-radius:8px;background:var(--blue);color:#fff;font-weight:800;font-size:15px;padding:12px 14px;cursor:pointer;transition:transform .14s ease,box-shadow .14s ease,background .14s ease}button:hover{transform:translateY(-1px);box-shadow:0 10px 20px rgba(23,105,224,.18)}button:disabled{background:#b8c2cf;box-shadow:none;transform:none;cursor:not-allowed}.secondary{background:#eef2f7;color:#18202a}.ghost{background:transparent;color:#1769e0;padding:6px}.ghost:hover{box-shadow:none}.wifi-list{display:grid;gap:8px;margin-top:12px;max-height:52vh;overflow:auto;padding-right:2px}.wifi-card{width:100%;display:grid;grid-template-columns:1fr auto;gap:12px;align-items:center;text-align:left;background:#fff;color:var(--ink);border:1px solid #d8e0ea;border-radius:8px;padding:14px;box-shadow:none}.wifi-card:hover{background:#f8fbff;border-color:#9fc2ff}.wifi-name{font-size:16px;font-weight:850;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.wifi-meta{display:flex;align-items:center;gap:8px;color:var(--muted);font-size:13px;margin-top:4px}.lock{width:10px;height:8px;border:2px solid currentColor;border-radius:2px;display:inline-block;position:relative}.lock:before{content:"";position:absolute;left:1px;top:-8px;width:6px;height:7px;border:2px solid currentColor;border-bottom:0;border-radius:8px 8px 0 0}.bars{display:flex;align-items:end;gap:2px;height:18px}.bars i{display:block;width:4px;border-radius:4px;background:#bcc6d3}.bars i:nth-child(1){height:6px}.bars i:nth-child(2){height:10px}.bars i:nth-child(3){height:14px}.bars i:nth-child(4){height:18px}.bars.s1 i:nth-child(-n+1),.bars.s2 i:nth-child(-n+2),.bars.s3 i:nth-child(-n+3),.bars.s4 i:nth-child(-n+4){background:#1769e0}
    details{border-top:1px solid var(--line);margin-top:14px;padding-top:12px}summary{cursor:pointer;color:#3b4655;font-weight:800}.empty{color:var(--muted);font-size:14px;border:1px dashed #c8d1dc;border-radius:8px;padding:16px;text-align:center}.status{min-height:22px;margin:12px 0 0;font-size:14px}.ok{color:var(--green)}.err{color:var(--red)}.warn{color:var(--amber)}.skeleton{height:64px;border-radius:8px;background:linear-gradient(90deg,#eef2f7,#f8fafc,#eef2f7);background-size:220% 100%;animation:shine 1.1s ease-in-out infinite}.spinner{width:16px;height:16px;border:2px solid rgba(255,255,255,.45);border-top-color:#fff;border-radius:50%;animation:spin .8s linear infinite;display:inline-block;vertical-align:-3px;margin-right:8px}
    .modal,.complete{position:fixed;inset:0;background:rgba(10,14,22,.45);display:none;align-items:flex-end;z-index:20}.modal.show,.complete.show{display:flex}.sheet{width:100%;max-width:640px;margin:0 auto;background:#fff;border-radius:16px 16px 0 0;padding:18px 16px 20px;box-shadow:0 -18px 45px rgba(0,0,0,.18);animation:slideUp .2s ease}.sheet-head{display:flex;justify-content:space-between;align-items:flex-start;gap:12px;margin-bottom:14px}.sheet-title{min-width:0}.sheet-title b{display:block;font-size:20px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.actions{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:16px}.complete{align-items:center;background:#f3f6fa}.complete-card{background:#fff;border:1px solid #cfead8;border-radius:8px;box-shadow:var(--shadow);margin:18px;padding:22px;text-align:center}.check{width:58px;height:58px;border-radius:999px;background:#eaf7ee;color:#15803d;display:grid;place-items:center;font-size:34px;margin:0 auto 12px}.count{font-size:42px;font-weight:900;color:#1769e0;margin:10px 0}.small{font-size:13px;color:var(--muted)}@keyframes spin{to{transform:rotate(360deg)}}@keyframes shine{to{background-position:-220% 0}}@keyframes slideUp{from{transform:translateY(24px);opacity:.7}to{transform:none;opacity:1}}@media(max-width:520px){main{padding:14px 10px 26px}.top{display:block}.badge{margin-top:12px}.toolbar{grid-template-columns:1fr}.wifi-list{max-height:56vh}.panel{padding:14px}.actions{grid-template-columns:1fr}}
  </style>
</head>
<body><main>
  <div class="top">
    <div><h1>Jetson WiFi Setup</h1><p class="lead">Choose the network that this Jetson should use after setup.</p></div>
    <div class="badge"><span class="dot"></span><span id="apiState">Setup online</span></div>
  </div>
  <section class="panel">
    <h2>Device</h2>
    <div class="device"><span>Name</span><code id="device">loading...</code><span>MAC</span><code id="mac">loading...</code></div>
  </section>
  <section class="panel">
    <h2>Available WiFi</h2>
    <div class="toolbar"><div class="search"><input id="filter" autocomplete="off" oninput="renderNetworks()" placeholder="Search network"></div><button id="scanBtn" class="secondary" onclick="scanWifi()">Scan</button></div>
    <div id="wifiList" class="wifi-list"><div class="skeleton"></div><div class="skeleton"></div><div class="skeleton"></div></div>
    <p class="hint" id="scanHint"></p>
    <details><summary>Hidden network</summary><div class="toolbar"><input id="manualSsid" autocomplete="off" placeholder="Enter SSID"><button onclick="openPasswordModal(el('manualSsid').value.trim(),true)">Next</button></div></details>
    <p id="status" class="status"></p>
  </section>
  <div id="passwordModal" class="modal" onclick="closeModalOnBackdrop(event)"><div class="sheet">
    <div class="sheet-head"><div class="sheet-title"><span class="hint">Connect to</span><b id="modalSsid">Network</b></div><button class="ghost" onclick="closePasswordModal()">Close</button></div>
    <label for="password">Password</label><input id="password" type="password" autocomplete="current-password" placeholder="WiFi password">
    <p class="hint" id="modalHint">Leave empty only if this network is open.</p>
    <div class="actions"><button class="secondary" onclick="closePasswordModal()">Cancel</button><button id="connectBtn" onclick="connectWifi()">Connect Jetson</button></div>
  </div></div>
  <div id="complete" class="complete"><div class="complete-card"><div class="check">OK</div><h1>WiFi saved</h1><p class="lead">Jetson will close this setup hotspot and join <b id="completeSsid"></b>.</p><div id="countdown" class="count">8</div><p class="small">This captive page may show an error after the hotspot closes. That is expected. Reconnect your phone/Mac to the target WiFi.</p></div></div>
<script>
async function j(path, opts){const r=await fetch(path,opts); if(!r.ok) throw new Error(await r.text()); const d=await r.json(); if(d&&d.ok===false) throw new Error(d.error||JSON.stringify(d)); return d;}
const el=id=>document.getElementById(id);let wifiNetworks=[],selectedSsid='',connecting=false;
function esc(s){return String(s||'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
function setStatus(text,type=''){el('status').textContent=text;el('status').className='status '+type;}
function setBusy(button,busy,text){button.disabled=busy;if(busy){button.dataset.label=button.textContent;button.innerHTML='<span class="spinner"></span>'+text;}else{button.textContent=button.dataset.label||button.textContent;}}
function signalClass(signal){const n=Number(signal)||0;return n>75?'s4':n>50?'s3':n>25?'s2':'s1';}
async function loadInfo(){try{const d=await j('/device-info');el('device').textContent=d.device?.id||'';el('mac').textContent=d.device?.mac||'';}catch(e){setStatus(e.message,'err');el('apiState').textContent='API error';}}
function openPasswordModal(name){if(!name){setStatus('Enter or select a WiFi name.','err');return;}selectedSsid=name;el('modalSsid').textContent=name;el('password').value='';el('passwordModal').classList.add('show');setTimeout(()=>el('password').focus(),80);}
function closePasswordModal(){if(connecting)return;el('passwordModal').classList.remove('show');}
function closeModalOnBackdrop(e){if(e.target===el('passwordModal'))closePasswordModal();}
function renderNetworks(){const q=(el('filter').value||'').toLowerCase();const rows=wifiNetworks.filter(n=>(n.ssid||'').toLowerCase().includes(q));el('wifiList').innerHTML='';rows.forEach(n=>{const b=document.createElement('button');b.className='wifi-card';b.type='button';const secured=(n.security||'').trim()!=='';b.innerHTML=`<div><div class="wifi-name">${esc(n.ssid)}</div><div class="wifi-meta">${secured?'<span class="lock"></span>':'Open'}<span>${esc(n.security||'No password')}</span><span>${Number(n.signal)||0}%</span></div></div><div class="bars ${signalClass(n.signal)}"><i></i><i></i><i></i><i></i></div>`;b.onclick=()=>openPasswordModal(n.ssid);el('wifiList').appendChild(b);});if(!rows.length){el('wifiList').innerHTML='<div class="empty">No matching WiFi. Scan again or use hidden network.</div>';}}
async function scanWifi(){const btn=el('scanBtn');setBusy(btn,true,'Scanning');el('wifiList').innerHTML='<div class="skeleton"></div><div class="skeleton"></div><div class="skeleton"></div>';setStatus('Scanning WiFi networks...');try{const d=await j('/wifi/scan');wifiNetworks=d.networks||[];el('scanHint').textContent=d.source==='cache'?'Showing networks captured before setup hotspot started.':'Live scan completed.';renderNetworks();setStatus(wifiNetworks.length?'Tap a network to continue.':'No WiFi found. Try Scan again or use hidden network.',wifiNetworks.length?'':'warn');}catch(e){el('wifiList').innerHTML='<div class="empty">Scan failed.</div>';setStatus(e.message,'err');}finally{setBusy(btn,false);}}
function showComplete(delay){el('completeSsid').textContent=selectedSsid;el('complete').classList.add('show');let left=Number(delay)||8;el('countdown').textContent=left;const timer=setInterval(()=>{left-=1;el('countdown').textContent=Math.max(left,0);if(left<=0)clearInterval(timer);},1000);}
async function connectWifi(){if(!selectedSsid){setStatus('Select a WiFi network first.','err');return;}connecting=true;const btn=el('connectBtn');setBusy(btn,true,'Saving');try{const d=await j('/wifi/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:selectedSsid,password:el('password').value})});el('passwordModal').classList.remove('show');showComplete(d.switch_delay_sec||8);}catch(e){connecting=false;setStatus(e.message,'err');setBusy(btn,false);}}
loadInfo();scanWifi();
</script>
</main></body></html>)HTML";
}

} // namespace

ImageHttpServerConfig ImageHttpServerConfig::from_env() {
    ImageHttpServerConfig cfg;
    cfg.bind_host = getenv_or("TB_IMAGE_BIND_HOST", cfg.bind_host);
    cfg.port = getenv_int("TB_IMAGE_PORT", cfg.port);
    cfg.jpeg_quality = getenv_int("TB_IMAGE_JPEG_QUALITY", cfg.jpeg_quality);
    cfg.stream_fps = getenv_int("TB_IMAGE_STREAM_FPS", cfg.stream_fps);
    cfg.public_base_url = getenv_or("TB_IMAGE_PUBLIC_BASE_URL", cfg.public_base_url);
    std::string public_host = getenv_or("TB_IMAGE_PUBLIC_HOST", getenv_or("TB_PUBLIC_HOST", ""));
    if (cfg.public_base_url.empty() && !public_host.empty()) {
        cfg.public_base_url = make_base_url(public_host, cfg.port);
    }
    return cfg;
}

std::string ImageHttpServerConfig::base_url() const {
    if (!public_base_url.empty()) return public_base_url;
    std::string host = bind_host;
    if (host.empty() || host == "0.0.0.0") {
        host = detect_ipv4(getenv_or("TB_IMAGE_INTERFACE", getenv_or("JETSON_PUBLIC_IFACE", "")));
    }
    if (!usable_ipv4(host)) host = "127.0.0.1";
    return make_base_url(host, port);
}

ImageHttpServer::ImageHttpServer(ImageHttpServerConfig config)
    : config_(std::move(config)) {}

ImageHttpServer::~ImageHttpServer() { stop(); }

bool ImageHttpServer::start() {
    if (running_.exchange(true)) return true;

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        running_ = false;
        log_msg(LOG_ERROR, "Image server socket failed");
        return false;
    }

    int yes = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));
    if (inet_pton(AF_INET, config_.bind_host.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_msg(LOG_ERROR, "Image server bind failed: %s:%d", config_.bind_host.c_str(), config_.port);
        close(server_fd_);
        server_fd_ = -1;
        running_ = false;
        return false;
    }

    if (listen(server_fd_, 8) < 0) {
        log_msg(LOG_ERROR, "Image server listen failed");
        close(server_fd_);
        server_fd_ = -1;
        running_ = false;
        return false;
    }

    accept_thread_ = std::thread(&ImageHttpServer::accept_loop, this);
    log_msg(LOG_WARNING, "Image server started: %s", config_.base_url().c_str());
    return true;
}

void ImageHttpServer::stop() {
    running_ = false;
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
}

void ImageHttpServer::update_frame(const cv::Mat& frame) {
    if (frame.empty()) return;

    std::vector<unsigned char> jpeg;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, config_.jpeg_quality};
    if (!cv::imencode(".jpg", frame, jpeg, params)) return;

    std::lock_guard<std::mutex> lock(frame_mutex_);
    latest_jpeg_ = std::move(jpeg);
}

void ImageHttpServer::set_health_json(const std::string& json) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    health_json_ = json.empty() ? "{}" : json;
}

void ImageHttpServer::set_device_info_json(const std::string& json) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    device_info_json_ = json.empty() ? "{}" : json;
}

void ImageHttpServer::set_setup_handler(SetupHttpHandler handler) {
    std::lock_guard<std::mutex> lock(setup_mutex_);
    setup_handler_ = std::move(handler);
}

bool ImageHttpServer::copy_latest_jpeg(std::vector<unsigned char>* out) const {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (latest_jpeg_.empty()) return false;
    *out = latest_jpeg_;
    return true;
}

void ImageHttpServer::accept_loop() {
    while (running_) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int client_fd = accept(server_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (client_fd < 0) continue;
        std::thread(&ImageHttpServer::handle_client, this, client_fd).detach();
    }
}

void ImageHttpServer::handle_client(int client_fd) {
    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buf[n] = 0;
    std::string request_data(buf, static_cast<std::size_t>(n));
    std::size_t header_end = request_data.find("\r\n\r\n");
    std::size_t expected_body = content_length(request_data);
    while (header_end != std::string::npos && request_body(request_data).size() < expected_body) {
        n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        request_data.append(buf, static_cast<std::size_t>(n));
    }

    std::string method = first_request_method(request_data);
    std::string path = first_request_path(request_data);
    HttpRequest request{method, path, request_body(request_data)};

    if (method == "OPTIONS") send_options(client_fd);
    else if (path == "/" || path == "/setup" || is_captive_probe_path(path)) send_setup_page(client_fd);
    else if (path == "/snapshot.jpg") send_snapshot(client_fd);
    else if (path == "/stream.mjpg") send_stream(client_fd);
    else if (path == "/health") send_health(client_fd);
    else if (path == "/device-info" || path == "/provisioning") send_device_info(client_fd);
    else if (is_setup_api_path(path)) send_setup_api(client_fd, request);
    else send_not_found(client_fd);

    close(client_fd);
}

void ImageHttpServer::send_snapshot(int client_fd) {
    std::vector<unsigned char> jpeg;
    if (!copy_latest_jpeg(&jpeg)) {
        send_text(client_fd, "HTTP/1.1 503 Service Unavailable\r\n" + cors_headers() +
                             "Content-Length: 0\r\n\r\n");
        return;
    }

    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: image/jpeg\r\n"
        << cors_headers()
        << "Cache-Control: no-store\r\n"
        << "Content-Length: " << jpeg.size() << "\r\n\r\n";
    send_text(client_fd, hdr.str());
    send_all(client_fd, jpeg.data(), jpeg.size());
}

void ImageHttpServer::send_stream(int client_fd) {
    send_text(client_fd,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With\r\n"
        "Access-Control-Allow-Private-Network: true\r\n"
        "Cache-Control: no-store\r\n\r\n");

    int fps = config_.stream_fps <= 0 ? 1 : config_.stream_fps;
    auto delay = std::chrono::milliseconds(1000 / fps);
    while (running_) {
        std::vector<unsigned char> jpeg;
        if (!copy_latest_jpeg(&jpeg)) {
            std::this_thread::sleep_for(delay);
            continue;
        }

        std::ostringstream part;
        part << "--frame\r\n"
             << "Content-Type: image/jpeg\r\n"
             << "Content-Length: " << jpeg.size() << "\r\n\r\n";
        if (!send_text(client_fd, part.str())) break;
        if (!send_all(client_fd, jpeg.data(), jpeg.size())) break;
        if (!send_text(client_fd, "\r\n")) break;
        std::this_thread::sleep_for(delay);
    }
}

void ImageHttpServer::send_health(int client_fd) {
    std::string body;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        body = health_json_;
    }

    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: application/json\r\n"
        << cors_headers()
        << "Cache-Control: no-store\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n";
    send_text(client_fd, hdr.str());
    send_text(client_fd, body);
}

void ImageHttpServer::send_device_info(int client_fd) {
    std::string body;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        body = device_info_json_;
    }

    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: application/json\r\n"
        << cors_headers()
        << "Cache-Control: no-store\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n";
    send_text(client_fd, hdr.str());
    send_text(client_fd, body);
}

void ImageHttpServer::send_setup_page(int client_fd) {
    const std::string body = setup_page_html();
    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: text/html; charset=utf-8\r\n"
        << cors_headers()
        << "Cache-Control: no-store\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n";
    send_text(client_fd, hdr.str());
    send_text(client_fd, body);
}

void ImageHttpServer::send_setup_api(int client_fd, const HttpRequest& request) {
    SetupHttpHandler handler;
    {
        std::lock_guard<std::mutex> lock(setup_mutex_);
        handler = setup_handler_;
    }

    std::string body = handler ? handler(request) : "{\"ok\":false,\"error\":\"setup_handler_not_configured\"}";
    if (body.empty()) body = "{\"ok\":true}";

    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: application/json\r\n"
        << cors_headers()
        << "Cache-Control: no-store\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n";
    send_text(client_fd, hdr.str());
    send_text(client_fd, body);
}

void ImageHttpServer::send_options(int client_fd) {
    std::ostringstream hdr;
    hdr << "HTTP/1.1 204 No Content\r\n"
        << cors_headers()
        << "Cache-Control: no-store\r\n"
        << "Content-Length: 0\r\n\r\n";
    send_text(client_fd, hdr.str());
}

void ImageHttpServer::send_not_found(int client_fd) {
    const std::string body = "not found\n";
    std::ostringstream hdr;
    hdr << "HTTP/1.1 404 Not Found\r\n"
        << "Content-Type: text/plain\r\n"
        << cors_headers()
        << "Content-Length: " << body.size() << "\r\n\r\n";
    send_text(client_fd, hdr.str());
    send_text(client_fd, body);
}

} // namespace telemetry
