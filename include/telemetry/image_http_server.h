// -*- coding: utf-8 -*-
// telemetry/image_http_server.h — Lightweight snapshot/MJPEG server for dashboard image view
#pragma once

#include <opencv2/opencv.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace telemetry {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

using SetupHttpHandler = std::function<std::string(const HttpRequest&)>;

struct ImageHttpServerConfig {
    std::string bind_host = "0.0.0.0";
    int         port = 8090;
    int         jpeg_quality = 75;
    int         stream_fps = 3;
    std::string public_base_url;

    static ImageHttpServerConfig from_env();
    std::string base_url() const;
};

class ImageHttpServer {
public:
    explicit ImageHttpServer(ImageHttpServerConfig config);
    ~ImageHttpServer();

    ImageHttpServer(const ImageHttpServer&) = delete;
    ImageHttpServer& operator=(const ImageHttpServer&) = delete;

    bool start();
    void stop();

    void update_frame(const cv::Mat& frame);
    void set_health_json(const std::string& json);
    void set_device_info_json(const std::string& json);
    void set_setup_handler(SetupHttpHandler handler);

    std::string snapshot_url() const { return config_.base_url() + "/snapshot.jpg"; }
    std::string stream_url() const { return config_.base_url() + "/stream.mjpg"; }
    std::string health_url() const { return config_.base_url() + "/health"; }
    std::string device_info_url() const { return config_.base_url() + "/device-info"; }

private:
    ImageHttpServerConfig config_;
    std::atomic<bool> running_{false};
    int server_fd_ = -1;
    std::thread accept_thread_;

    mutable std::mutex frame_mutex_;
    std::vector<unsigned char> latest_jpeg_;
    std::string health_json_ = "{}";
    std::string device_info_json_ = "{}";
    mutable std::mutex setup_mutex_;
    SetupHttpHandler setup_handler_;

    void accept_loop();
    void handle_client(int client_fd);
    void send_snapshot(int client_fd);
    void send_stream(int client_fd);
    void send_health(int client_fd);
    void send_device_info(int client_fd);
    void send_setup_page(int client_fd);
    void send_setup_api(int client_fd, const HttpRequest& request);
    void send_options(int client_fd);
    void send_not_found(int client_fd);

    bool copy_latest_jpeg(std::vector<unsigned char>* out) const;
};

} // namespace telemetry
