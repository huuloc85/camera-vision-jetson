// -*- coding: utf-8 -*-
// telemetry/thingsboard_mqtt_client.cpp — Async ThingsBoard MQTT telemetry client
#include "telemetry/thingsboard_mqtt_client.h"
#include "core/logger.h"

#include <mosquitto.h>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>

namespace telemetry {
namespace {

std::mutex g_mosquitto_mutex;
int g_mosquitto_users = 0;

void acquire_mosquitto_library() {
    std::lock_guard<std::mutex> lock(g_mosquitto_mutex);
    if (g_mosquitto_users++ == 0) mosquitto_lib_init();
}

void release_mosquitto_library() {
    std::lock_guard<std::mutex> lock(g_mosquitto_mutex);
    if (g_mosquitto_users > 0 && --g_mosquitto_users == 0) mosquitto_lib_cleanup();
}

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

bool getenv_bool(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (!value || !value[0]) return fallback;
    return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "yes") == 0;
}

std::string default_client_id() {
    char host[128];
    if (gethostname(host, sizeof(host)) == 0) {
        host[sizeof(host) - 1] = 0;
        return std::string("jetson-inspect-") + host;
    }
    return "jetson-inspect";
}

std::string result_name(ProductResult result) {
    switch (result) {
        case ProductResult::OK:   return "OK";
        case ProductResult::NG:   return "NG";
        case ProductResult::WAIT: return "WAIT";
    }
    return "UNKNOWN";
}

std::string json_escape(const std::string& value) {
    std::ostringstream os;
    for (unsigned char c : value) {
        switch (c) {
            case '\\': os << "\\\\"; break;
            case '"':  os << "\\\""; break;
            case '\b': os << "\\b";  break;
            case '\f': os << "\\f";  break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (c < 0x20) {
                    os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(c) << std::dec;
                } else {
                    os << c;
                }
        }
    }
    return os.str();
}

void append_metric(std::ostringstream& os, const char* key, double value) {
    os << ",\"" << key << "\":" << value;
}

void append_metric(std::ostringstream& os, const char* key, int value) {
    os << ",\"" << key << "\":" << value;
}

void append_metric(std::ostringstream& os, const char* key, bool value) {
    os << ",\"" << key << "\":" << (value ? "true" : "false");
}

void append_metric(std::ostringstream& os, const char* key, std::size_t value) {
    os << ",\"" << key << "\":" << value;
}

void append_string(std::ostringstream& os, const char* key, const std::string& value) {
    os << ",\"" << key << "\":\"" << json_escape(value) << "\"";
}

std::string make_attributes_payload(std::ostringstream& values) {
    return std::string("{") + values.str() + "}";
}

std::string as_json_string_value(const std::string& json) {
    return std::string("\"") + json_escape(json) + "\"";
}

int rpc_id_from_topic(const std::string& topic) {
    const std::string prefix = "v1/devices/me/rpc/request/";
    if (topic.rfind(prefix, 0) != 0) return 0;
    return std::atoi(topic.c_str() + prefix.size());
}

std::string json_string_field(const std::string& json, const std::string& key) {
    std::string marker = "\"" + key + "\"";
    std::size_t pos = json.find(marker);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";

    std::string out;
    bool escaped = false;
    for (std::size_t i = pos + 1; i < json.size(); ++i) {
        char c = json[i];
        if (escaped) {
            out += c;
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return out;
        } else {
            out += c;
        }
    }
    return out;
}

std::string json_raw_field(const std::string& json, const std::string& key) {
    std::string marker = "\"" + key + "\"";
    std::size_t pos = json.find(marker);
    if (pos == std::string::npos) return "null";
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return "null";
    pos++;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
    if (pos >= json.size()) return "null";

    if (json[pos] == '{' || json[pos] == '[') {
        char open = json[pos];
        char close = open == '{' ? '}' : ']';
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (std::size_t i = pos; i < json.size(); ++i) {
            char c = json[i];
            if (in_string) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') in_string = true;
            else if (c == open) depth++;
            else if (c == close && --depth == 0) return json.substr(pos, i - pos + 1);
        }
    }

    std::size_t end = pos;
    while (end < json.size() && json[end] != ',' && json[end] != '}') end++;
    return json.substr(pos, end - pos);
}

} // namespace

ThingsBoardMqttConfig ThingsBoardMqttConfig::from_env() {
    ThingsBoardMqttConfig cfg;
    cfg.host = getenv_or("TB_MQTT_HOST", getenv_or("TB_HOST", cfg.host));
    cfg.port = getenv_int("TB_MQTT_PORT", cfg.port);
    cfg.access_token = getenv_or("TB_MQTT_ACCESS_TOKEN", getenv_or("TB_ACCESS_TOKEN", ""));
    cfg.username = getenv_or("TB_MQTT_USERNAME", getenv_or("TB_USERNAME", ""));
    cfg.password = getenv_or("TB_MQTT_PASSWORD", getenv_or("TB_PASSWORD", ""));
    if (cfg.username.empty()) cfg.username = cfg.access_token;
    if (cfg.access_token.empty()) cfg.access_token = cfg.username;
    cfg.client_id = getenv_or("TB_MQTT_CLIENT_ID", default_client_id());
    cfg.topic = getenv_or("TB_MQTT_TOPIC", cfg.topic);
    cfg.qos = getenv_int("TB_MQTT_QOS", cfg.qos);
    cfg.keepalive_sec = getenv_int("TB_MQTT_KEEPALIVE_SEC", cfg.keepalive_sec);
    cfg.reconnect_min_delay_sec = getenv_int("TB_MQTT_RECONNECT_MIN_SEC", cfg.reconnect_min_delay_sec);
    cfg.reconnect_max_delay_sec = getenv_int("TB_MQTT_RECONNECT_MAX_SEC", cfg.reconnect_max_delay_sec);
    cfg.queue_capacity = static_cast<std::size_t>(getenv_int("TB_MQTT_QUEUE_CAPACITY", static_cast<int>(cfg.queue_capacity)));
    cfg.use_tls = getenv_bool("TB_MQTT_TLS", cfg.use_tls);
    cfg.ca_file = getenv_or("TB_MQTT_CA_FILE", cfg.ca_file);
    if (cfg.use_tls && cfg.port == 1883) cfg.port = 8883;
    return cfg;
}

bool ThingsBoardMqttConfig::validate(std::string* error) const {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (host.empty()) return fail("TB_MQTT_HOST/TB_HOST is empty");
    if (username.empty()) return fail("TB_MQTT_USERNAME or TB_MQTT_ACCESS_TOKEN is empty");
    if (port <= 0 || port > 65535) return fail("TB_MQTT_PORT is outside 1..65535");
    if (topic.empty()) return fail("TB_MQTT_TOPIC is empty");
    if (qos < 0 || qos > 1) return fail("TB_MQTT_QOS must be 0 or 1");
    if (keepalive_sec <= 0) return fail("TB_MQTT_KEEPALIVE_SEC must be positive");
    if (queue_capacity == 0) return fail("TB_MQTT_QUEUE_CAPACITY must be positive");
    if (reconnect_min_delay_sec <= 0 || reconnect_max_delay_sec < reconnect_min_delay_sec)
        return fail("MQTT reconnect delays are invalid");
    if (use_tls && ca_file.empty()) return fail("TB_MQTT_TLS=1 requires TB_MQTT_CA_FILE");
    return true;
}

ThingsBoardMqttClient::ThingsBoardMqttClient(ThingsBoardMqttConfig config)
    : config_(std::move(config)) {}

ThingsBoardMqttClient::~ThingsBoardMqttClient() { stop(); }

bool ThingsBoardMqttClient::start() {
    if (running_.exchange(true)) return true;

    std::string error;
    if (!config_.validate(&error)) {
        running_ = false;
        log_msg(LOG_ERROR, "ThingsBoard MQTT config invalid: %s", error.c_str());
        return false;
    }

    acquire_mosquitto_library();
    if (!init_client()) {
        if (mosq_) {
            mosquitto_destroy(mosq_);
            mosq_ = nullptr;
        }
        running_ = false;
        release_mosquitto_library();
        return false;
    }

    int rc = mosquitto_connect_async(mosq_, config_.host.c_str(), config_.port,
                                     config_.keepalive_sec);
    if (rc != MOSQ_ERR_SUCCESS) {
        log_msg(LOG_ERROR, "ThingsBoard MQTT connect setup failed: %s", mosquitto_strerror(rc));
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        running_ = false;
        release_mosquitto_library();
        return false;
    }

    rc = mosquitto_loop_start(mosq_);
    if (rc != MOSQ_ERR_SUCCESS) {
        log_msg(LOG_ERROR, "ThingsBoard MQTT loop start failed: %s", mosquitto_strerror(rc));
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        running_ = false;
        release_mosquitto_library();
        return false;
    }

    worker_ = std::thread(&ThingsBoardMqttClient::worker_loop, this);
    return true;
}

void ThingsBoardMqttClient::stop() {
    bool was_running = running_.exchange(false);
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    if (mosq_) {
        mosquitto_disconnect(mosq_);
        mosquitto_loop_stop(mosq_, true);
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        connected_ = false;
        release_mosquitto_library();
    } else if (was_running) {
        release_mosquitto_library();
    }
}

bool ThingsBoardMqttClient::enqueue(const InspectionTelemetry& telemetry) {
    return publish_raw(config_.topic, build_payload(telemetry), config_.qos, false);
}

bool ThingsBoardMqttClient::enqueue_health(const DeviceHealthTelemetry& health) {
    return publish_raw(config_.topic, build_health_payload(health), config_.qos, false);
}

bool ThingsBoardMqttClient::publish_attributes(const ProjectInfoAttributes& attrs) {
    return publish_raw("v1/devices/me/attributes", build_project_attributes(attrs), config_.qos, false);
}

bool ThingsBoardMqttClient::publish_image_attributes(const ImageEndpointAttributes& attrs) {
    return publish_raw("v1/devices/me/attributes", build_image_attributes(attrs), config_.qos, false);
}

bool ThingsBoardMqttClient::publish_identity_attributes(const DeviceIdentityAttributes& attrs) {
    return publish_raw("v1/devices/me/attributes", build_identity_attributes(attrs), config_.qos, false);
}

bool ThingsBoardMqttClient::publish_raw(const std::string& topic, const std::string& payload,
                                        int qos, bool retain) {
    PublishMessage message;
    message.topic = topic;
    message.payload = payload;
    message.qos = qos < 0 ? config_.qos : qos;
    message.retain = retain;
    return enqueue_message(std::move(message));
}

void ThingsBoardMqttClient::set_rpc_handler(RpcHandler handler) {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    rpc_handler_ = std::move(handler);
}

bool ThingsBoardMqttClient::init_client() {
    mosq_ = mosquitto_new(config_.client_id.c_str(), true, this);
    if (!mosq_) {
        log_msg(LOG_ERROR, "ThingsBoard MQTT: mosquitto_new failed");
        return false;
    }

    const char* password = config_.password.empty() ? nullptr : config_.password.c_str();
    mosquitto_username_pw_set(mosq_, config_.username.c_str(), password);
    mosquitto_connect_callback_set(mosq_, &ThingsBoardMqttClient::on_connect);
    mosquitto_disconnect_callback_set(mosq_, &ThingsBoardMqttClient::on_disconnect);
    mosquitto_message_callback_set(mosq_, &ThingsBoardMqttClient::on_message);
    mosquitto_reconnect_delay_set(mosq_, config_.reconnect_min_delay_sec,
                                  config_.reconnect_max_delay_sec, true);

    if (config_.use_tls) {
        int rc = mosquitto_tls_set(mosq_, config_.ca_file.c_str(), nullptr, nullptr, nullptr, nullptr);
        if (rc != MOSQ_ERR_SUCCESS) {
            log_msg(LOG_ERROR, "ThingsBoard MQTT TLS setup failed: %s", mosquitto_strerror(rc));
            return false;
        }
    }
    return true;
}

bool ThingsBoardMqttClient::enqueue_message(PublishMessage message) {
    if (!running_) return false;
    if (message.topic.empty() || message.payload.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= config_.queue_capacity) {
            queue_.pop_front();
            dropped_count_++;
        }
        queue_.push_back(std::move(message));
    }
    queue_cv_.notify_one();
    return true;
}

void ThingsBoardMqttClient::worker_loop() {
    while (running_) {
        PublishMessage message;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (queue_.empty()) {
                queue_cv_.wait_for(lock, std::chrono::milliseconds(250));
            }
            if (!running_) break;
            if (!connected_.load() || queue_.empty()) {
                continue;
            }
            message = std::move(queue_.front());
            queue_.pop_front();
        }

        publish_one(message);
    }
}

void ThingsBoardMqttClient::publish_one(const PublishMessage& message) {
    int mid = 0;
    int rc = mosquitto_publish(mosq_, &mid, message.topic.c_str(),
                               static_cast<int>(message.payload.size()), message.payload.data(),
                               message.qos, message.retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        log_msg(LOG_ERROR, "ThingsBoard MQTT publish failed: %s", mosquitto_strerror(rc));
        connected_ = false;
        requeue_front(message);
    }
}

void ThingsBoardMqttClient::requeue_front(const PublishMessage& message) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= config_.queue_capacity) {
        queue_.pop_back();
        dropped_count_++;
    }
    queue_.push_front(message);
}

void ThingsBoardMqttClient::on_connect(struct mosquitto* /*mosq*/, void* userdata, int rc) {
    auto* self = static_cast<ThingsBoardMqttClient*>(userdata);
    if (rc == 0) {
        self->connected_ = true;
        mosquitto_subscribe(self->mosq_, nullptr, "v1/devices/me/rpc/request/+", self->config_.qos);
        log_msg(LOG_WARNING, "ThingsBoard MQTT connected: %s:%d topic=%s",
                self->config_.host.c_str(), self->config_.port, self->config_.topic.c_str());
    } else {
        self->connected_ = false;
        log_msg(LOG_ERROR, "ThingsBoard MQTT refused connection: rc=%d", rc);
    }
}

void ThingsBoardMqttClient::on_message(struct mosquitto* /*mosq*/, void* userdata,
                                       const ::mosquitto_message* message) {
    auto* self = static_cast<ThingsBoardMqttClient*>(userdata);
    if (!message || !message->topic || !message->payload) return;
    std::string topic(message->topic);
    std::string payload(static_cast<const char*>(message->payload),
                        static_cast<std::size_t>(message->payloadlen));
    self->handle_message(topic, payload);
}

void ThingsBoardMqttClient::handle_message(const std::string& topic, const std::string& payload) {
    int request_id = rpc_id_from_topic(topic);
    if (request_id <= 0) return;

    RpcRequest request;
    request.id = request_id;
    request.method = json_string_field(payload, "method");
    request.params_json = json_raw_field(payload, "params");

    RpcHandler handler;
    {
        std::lock_guard<std::mutex> lock(rpc_mutex_);
        handler = rpc_handler_;
    }

    std::string response;
    if (handler) {
        response = handler(request);
        if (response.empty()) response = "{\"ok\":true}";
    } else {
        response = "{\"ok\":false,\"error\":\"rpc_handler_not_configured\"}";
    }
    respond_rpc(request_id, response);
}

void ThingsBoardMqttClient::respond_rpc(int request_id, const std::string& payload) {
    std::ostringstream topic;
    topic << "v1/devices/me/rpc/response/" << request_id;
    int mid = 0;
    mosquitto_publish(mosq_, &mid, topic.str().c_str(), static_cast<int>(payload.size()),
                      payload.data(), config_.qos, false);
}

void ThingsBoardMqttClient::on_disconnect(struct mosquitto* /*mosq*/, void* userdata, int rc) {
    auto* self = static_cast<ThingsBoardMqttClient*>(userdata);
    self->connected_ = false;
    if (self->running_.load()) {
        log_msg(LOG_ERROR, "ThingsBoard MQTT disconnected: rc=%d", rc);
    }
}

std::string ThingsBoardMqttClient::build_payload(const InspectionTelemetry& telemetry) {
    const auto& r = telemetry.result;
    const auto& m = r.metrics;
    const std::string label = r.label.empty() ? result_name(r.result) : r.label;

    std::ostringstream body;
    body << std::fixed << std::setprecision(3);
    body << "{\"schema\":\"" << json_escape(telemetry.schema_version) << "\""
         << ",\"device\":{\"id\":\"" << json_escape(telemetry.device_id) << "\""
         << ",\"mac\":\"" << json_escape(telemetry.mac_address) << "\"}"
         << ",\"product\":{"
         << "\"id\":" << telemetry.product_id
         << ",\"result\":\"" << json_escape(label) << "\""
         << ",\"is_ok\":" << (r.result == ProductResult::OK ? "true" : "false")
         << ",\"is_ng\":" << (r.result == ProductResult::NG ? "true" : "false")
         << "},\"counter\":{" 
         << "\"ok\":" << telemetry.total_ok
         << ",\"ng\":" << telemetry.total_ng
         << ",\"total\":" << (telemetry.total_ok + telemetry.total_ng)
         << ",\"cycle_rate\":" << telemetry.cycle_rate
         << "},\"process\":{" 
         << "\"app_state\":\"" << state_name(telemetry.app_state) << "\""
         << ",\"cycle_ms\":" << r.cycle_ms
         << ",\"info\":\"" << json_escape(r.info_text) << "\""
         << "},\"metrics\":{" 
         << "\"available\":" << (r.has_metrics ? "true" : "false");

    if (r.has_metrics) {
        append_metric(body, "area", m.area);
        append_metric(body, "perimeter", m.perimeter);
        append_metric(body, "solidity", m.solidity);
        append_metric(body, "width", m.width);
        append_metric(body, "height", m.height);
        append_metric(body, "top_points", m.top_points);
        append_metric(body, "top_width", m.top_width);
        append_metric(body, "top_width_ratio", m.top_width_ratio);
        append_metric(body, "spike_ratio", m.spike_ratio);
        append_metric(body, "spike_min_w", m.spike_min_w);
        append_metric(body, "spike_max_w", m.spike_max_w);
    }

    body << "}}";

    std::ostringstream os;
    os << "{\"ts\":" << telemetry.ts_ms
       << ",\"values\":{\"jetson_inspection\":" << as_json_string_value(body.str()) << "}}";
    return os.str();
}

std::string ThingsBoardMqttClient::build_health_payload(const DeviceHealthTelemetry& health) {
    std::ostringstream body;
    body << std::fixed << std::setprecision(2);
    body << "{\"schema\":\"" << json_escape(health.schema_version) << "\""
         << ",\"device\":{"
         << "\"id\":\"" << json_escape(health.device_id) << "\""
         << ",\"mac\":\"" << json_escape(health.mac_address) << "\""
         << ",\"hostname\":\"" << json_escape(health.hostname) << "\""
         << "},\"system\":{";
    body << "\"uptime_sec\":" << health.uptime_sec;
    append_metric(body, "cpu_load_1m", health.cpu_load_1m);
    append_metric(body, "mem_used_pct", health.mem_used_pct);
    append_metric(body, "disk_used_pct", health.disk_used_pct);
    append_metric(body, "cpu_temp_c", health.cpu_temp_c);
    body << "},\"services\":{";
    body << "\"camera_ok\":" << (health.camera_ok ? "true" : "false");
    append_metric(body, "gpio_ok", health.gpio_ok);
    append_metric(body, "mqtt_connected", health.mqtt_connected);
    append_metric(body, "image_server_ok", health.image_server_ok);
    append_metric(body, "mqtt_dropped", health.mqtt_dropped);
    body << "}}";

    std::ostringstream os;
    os << "{\"ts\":" << health.ts_ms
       << ",\"values\":{\"jetson_health\":" << as_json_string_value(body.str()) << "}}";
    return os.str();
}

std::string ThingsBoardMqttClient::build_project_attributes(const ProjectInfoAttributes& attrs) {
    std::ostringstream body;
    body << "{\"schema\":\"" << json_escape(attrs.schema_version) << "\""
         << ",\"device\":{\"id\":\"" << json_escape(attrs.device_id) << "\""
         << ",\"mac\":\"" << json_escape(attrs.mac_address) << "\"}"
         << ",\"project\":{"
         << "\"name\":\"" << json_escape(attrs.project_name) << "\""
         << ",\"version\":\"" << json_escape(attrs.project_version) << "\""
         << ",\"mode\":\"" << json_escape(attrs.app_mode) << "\""
         << "},\"production\":{" 
         << "\"line_id\":\"" << json_escape(attrs.line_id) << "\""
         << ",\"station_id\":\"" << json_escape(attrs.station_id) << "\""
         << ",\"device_role\":\"" << json_escape(attrs.device_role) << "\""
         << "}}";

    std::ostringstream os;
    os << "\"jetson_project\":" << as_json_string_value(body.str());
    return make_attributes_payload(os);
}

std::string ThingsBoardMqttClient::build_image_attributes(const ImageEndpointAttributes& attrs) {
    std::ostringstream body;
    body << "{\"schema\":\"" << json_escape(attrs.schema_version) << "\""
         << ",\"device\":{\"id\":\"" << json_escape(attrs.device_id) << "\""
         << ",\"mac\":\"" << json_escape(attrs.mac_address) << "\"}"
         << ",\"endpoints\":{"
         << "\"snapshot_url\":\"" << json_escape(attrs.snapshot_url) << "\""
         << ",\"stream_url\":\"" << json_escape(attrs.stream_url) << "\""
         << ",\"health_url\":\"" << json_escape(attrs.health_url) << "\""
         << "},\"settings\":{" 
         << "\"jpeg_quality\":" << attrs.jpeg_quality
         << ",\"stream_fps\":" << attrs.stream_fps
         << "}}";

    std::ostringstream os;
    os << "\"jetson_image\":" << as_json_string_value(body.str());
    return make_attributes_payload(os);
}

std::string ThingsBoardMqttClient::build_identity_attributes(const DeviceIdentityAttributes& attrs) {
    std::ostringstream body;
    body << "{\"schema\":\"" << json_escape(attrs.schema_version) << "\""
         << ",\"device\":{\"id\":\"" << json_escape(attrs.device_id) << "\""
         << ",\"mac\":\"" << json_escape(attrs.mac_address) << "\"}"
         << ",\"setup\":{"
         << "\"ssid\":\"" << json_escape(attrs.setup_ssid) << "\""
         << ",\"ip\":\"" << json_escape(attrs.setup_ip) << "\""
         << ",\"device_info_url\":\"" << json_escape(attrs.device_info_url) << "\""
         << "},\"thingsboard\":{"
         << "\"mqtt_host\":\"" << json_escape(attrs.mqtt_host) << "\""
         << ",\"mqtt_client_id\":\"" << json_escape(attrs.mqtt_client_id) << "\""
         << "}}";

    std::ostringstream os;
    os << "\"jetson_identity\":" << as_json_string_value(body.str());
    return make_attributes_payload(os);
}

} // namespace telemetry
