// -*- coding: utf-8 -*-
// telemetry/thingsboard_mqtt_client.h — Async ThingsBoard MQTT telemetry client
#pragma once

#include "core/types.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct mosquitto;
struct mosquitto_message;

namespace telemetry {

struct ThingsBoardMqttConfig {
    std::string host = "localhost";
    int         port = 1883;
    std::string access_token;
    std::string username;
    std::string password;
    std::string client_id;
    std::string topic = "v1/devices/me/telemetry";
    int         qos = 1;
    int         keepalive_sec = 60;
    int         reconnect_min_delay_sec = 1;
    int         reconnect_max_delay_sec = 30;
    std::size_t queue_capacity = 256;
    bool        use_tls = false;
    std::string ca_file;

    static ThingsBoardMqttConfig from_env();
    bool validate(std::string* error) const;
};

struct InspectionTelemetry {
    long long        ts_ms = 0;
    std::string      schema_version = "jetson.inspect.v1";
    std::string      device_id;
    std::string      mac_address;
    int              product_id = 0;
    int              total_ok = 0;
    int              total_ng = 0;
    double           cycle_rate = 0;
    AppState         app_state = AppState::IDLE;
    InspectionResult result;
};

struct DeviceHealthTelemetry {
    long long ts_ms = 0;
    std::string schema_version = "jetson.inspect.v1";
    std::string device_id;
    std::string mac_address;
    std::string hostname;
    double uptime_sec = 0;
    double cpu_load_1m = 0;
    double mem_used_pct = 0;
    double disk_used_pct = 0;
    double cpu_temp_c = 0;
    bool camera_ok = true;
    bool gpio_ok = true;
    bool mqtt_connected = false;
    bool image_server_ok = false;
    std::size_t mqtt_dropped = 0;
};

struct ProjectInfoAttributes {
    std::string schema_version = "jetson.inspect.v1";
    std::string device_id;
    std::string mac_address;
    std::string project_name = "jetson-inspect-v2";
    std::string project_version = "demo";
    std::string app_mode = "demo";
    std::string line_id;
    std::string station_id;
    std::string device_role = "vision-inspection";
};

struct ImageEndpointAttributes {
    std::string schema_version = "jetson.inspect.v1";
    std::string device_id;
    std::string mac_address;
    std::string snapshot_url;
    std::string stream_url;
    std::string health_url;
    int jpeg_quality = 75;
    int stream_fps = 3;
};

struct DeviceIdentityAttributes {
    std::string schema_version = "jetson.identity.v1";
    std::string device_id;
    std::string mac_address;
    std::string setup_ssid;
    std::string setup_ip;
    std::string device_info_url;
    std::string mqtt_host;
    std::string mqtt_client_id;
};

struct RpcRequest {
    int id = 0;
    std::string method;
    std::string params_json;
};

using RpcHandler = std::function<std::string(const RpcRequest&)>;

class ThingsBoardMqttClient {
public:
    explicit ThingsBoardMqttClient(ThingsBoardMqttConfig config);
    ~ThingsBoardMqttClient();

    ThingsBoardMqttClient(const ThingsBoardMqttClient&) = delete;
    ThingsBoardMqttClient& operator=(const ThingsBoardMqttClient&) = delete;

    bool start();
    void stop();

    bool enqueue(const InspectionTelemetry& telemetry);
    bool enqueue_health(const DeviceHealthTelemetry& health);
    bool publish_attributes(const ProjectInfoAttributes& attrs);
    bool publish_image_attributes(const ImageEndpointAttributes& attrs);
    bool publish_identity_attributes(const DeviceIdentityAttributes& attrs);
    bool publish_raw(const std::string& topic, const std::string& payload, int qos = -1, bool retain = false);
    void set_rpc_handler(RpcHandler handler);

    bool connected() const { return connected_.load(); }
    std::size_t dropped_count() const { return dropped_count_.load(); }

    static std::string build_payload(const InspectionTelemetry& telemetry);
    static std::string build_health_payload(const DeviceHealthTelemetry& health);
    static std::string build_project_attributes(const ProjectInfoAttributes& attrs);
    static std::string build_image_attributes(const ImageEndpointAttributes& attrs);
    static std::string build_identity_attributes(const DeviceIdentityAttributes& attrs);

private:
    struct PublishMessage {
        std::string topic;
        std::string payload;
        int qos = 0;
        bool retain = false;
    };

    ThingsBoardMqttConfig config_;
    mosquitto* mosq_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<std::size_t> dropped_count_{0};

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<PublishMessage> queue_;
    std::thread worker_;
    mutable std::mutex rpc_mutex_;
    RpcHandler rpc_handler_;

    bool init_client();
    void worker_loop();
    bool enqueue_message(PublishMessage message);
    void publish_one(const PublishMessage& message);
    void requeue_front(const PublishMessage& message);
    void handle_message(const std::string& topic, const std::string& payload);
    void respond_rpc(int request_id, const std::string& payload);

    static void on_connect(struct mosquitto* mosq, void* userdata, int rc);
    static void on_disconnect(struct mosquitto* mosq, void* userdata, int rc);
    static void on_message(struct mosquitto* mosq, void* userdata, const ::mosquitto_message* message);
};

} // namespace telemetry
