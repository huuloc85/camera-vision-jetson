// -*- coding: utf-8 -*-
// gpio/gpio_controller.cpp — PLC GPIO via Python helper, matching opencv-detect
#include "gpio/gpio_controller.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/time_utils.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

using core::now_sec;

namespace {

constexpr int kPollMs = 5;
constexpr int kDefaultResultHoldMs = 200;
constexpr size_t kTriggerQueueMax = 8;

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !value[0]) return false;
    return std::strcmp(value, "1") == 0 ||
           std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "TRUE") == 0 ||
           std::strcmp(value, "yes") == 0 ||
           std::strcmp(value, "YES") == 0;
}

int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    try {
        return std::max(0, std::stoi(value));
    } catch (...) {
        return fallback;
    }
}

std::string executable_dir() {
#if defined(__linux__)
    std::vector<char> buffer(4096, '\0');
    const ssize_t n = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (n <= 0) return "";
    buffer[static_cast<size_t>(n)] = '\0';
    return fs::path(buffer.data()).parent_path().string();
#else
    return "";
#endif
}

std::string helper_path() {
    const char* env = std::getenv("JETSON_INSPECT_PLC_HELPER");
    if (env && *env) return env;

    const std::vector<fs::path> candidates = {
        fs::path("scripts/plc_gpio_helper.py"),
        fs::path(executable_dir()) / "../scripts/plc_gpio_helper.py",
        fs::path(executable_dir()) / "scripts/plc_gpio_helper.py",
    };
    for (const auto& candidate : candidates) {
        if (!candidate.empty() && fs::exists(candidate)) return candidate.string();
    }
    return "scripts/plc_gpio_helper.py";
}

bool write_all(int fd, const std::string& text) {
    const char* data = text.data();
    size_t left = text.size();
    while (left > 0) {
        const ssize_t n = write(fd, data, left);
        if (n <= 0) return false;
        data += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

bool read_line(int fd, std::string& line, int timeout_ms) {
    line.clear();
    while (true) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        const int ready = select(fd + 1, &set, nullptr, nullptr, &timeout);
        if (ready <= 0) return false;

        char c = '\0';
        const ssize_t n = read(fd, &c, 1);
        if (n <= 0) return false;
        if (c == '\n') return true;
        if (c != '\r') line.push_back(c);
    }
}

std::string result_command(int pin) {
    if (pin == GPIOConfig::PIN_OK) return "RESULT OK";
    if (pin == GPIOConfig::PIN_NG) return "RESULT NG";
    return "RESULT WAIT";
}

} // namespace

GPIOController::GPIOController() {
    std::signal(SIGPIPE, SIG_IGN);

    mock_mode_ = env_enabled("JETSON_GPIO_MOCK") || env_enabled("GPIO_MOCK");
    light_on_ = true;

    if (mock_mode_) {
        log_msg(LOG_WARNING, "Jetson GPIO mock mode enabled; GPIO output is disabled");
        return;
    }

    if (!start_gpio_worker()) {
        mock_mode_ = true;
        log_msg(LOG_ERROR, "Jetson GPIO helper failed to start; running GPIO in mock mode");
        return;
    }

    log_msg(LOG_WARNING, "Jetson GPIO ready: BOARD OK=%d NG=%d BUSY=%d TRIGGER=%d active=HIGH",
            GPIOConfig::BOARD_OK, GPIOConfig::BOARD_NG,
            GPIOConfig::BOARD_BUSY, GPIOConfig::BOARD_TRIGGER);
}

GPIOController::~GPIOController() {
    cleanup();
}

bool GPIOController::start_gpio_worker() {
    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    if (::pipe(to_child) != 0 || ::pipe(from_child) != 0) {
        log_msg(LOG_ERROR, "PLC GPIO pipe failed");
        if (to_child[0] >= 0) ::close(to_child[0]);
        if (to_child[1] >= 0) ::close(to_child[1]);
        if (from_child[0] >= 0) ::close(from_child[0]);
        if (from_child[1] >= 0) ::close(from_child[1]);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        log_msg(LOG_ERROR, "PLC GPIO fork failed");
        ::close(to_child[0]);
        ::close(to_child[1]);
        ::close(from_child[0]);
        ::close(from_child[1]);
        return false;
    }

    if (pid == 0) {
        ::dup2(to_child[0], STDIN_FILENO);
        ::dup2(from_child[1], STDOUT_FILENO);
        ::close(to_child[0]);
        ::close(to_child[1]);
        ::close(from_child[0]);
        ::close(from_child[1]);

        const std::string helper = helper_path();
        const std::string trigger = std::to_string(GPIOConfig::BOARD_TRIGGER);
        const std::string ok = std::to_string(GPIOConfig::BOARD_OK);
        const std::string ng = std::to_string(GPIOConfig::BOARD_NG);
        const std::string busy = std::to_string(GPIOConfig::BOARD_BUSY);
        ::execlp("python3", "python3", "-u", helper.c_str(),
                 "--trigger-pin", trigger.c_str(),
                 "--ok-pin", ok.c_str(),
                 "--ng-pin", ng.c_str(),
                 "--busy-pin", busy.c_str(),
                 static_cast<char*>(nullptr));
        _exit(127);
    }

    ::close(to_child[0]);
    ::close(from_child[1]);
    child_pid_ = static_cast<int>(pid);
    in_fd_ = to_child[1];
    out_fd_ = from_child[0];

    std::string response;
    if (!read_line(out_fd_, response, 5000)) {
        log_msg(LOG_ERROR, "PLC GPIO helper did not respond");
        close_process();
        return false;
    }
    if (response.rfind("READY", 0) != 0) {
        log_msg(LOG_ERROR, "PLC GPIO helper startup failed: %s", response.c_str());
        close_process();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        trigger_high_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(trigger_mutex_);
        trigger_queue_.clear();
    }
    start_worker();
    return true;
}

bool GPIOController::request(const std::string& command, std::string& response, int timeout_ms) {
    if (mock_mode_) {
        log_msg(LOG_WARNING, "GPIO MOCK>> %s", command.c_str());
        response = command == "READ" ? "0" : "OK";
        return true;
    }

    if (in_fd_ < 0 || out_fd_ < 0) return false;
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!write_all(in_fd_, command + "\n")) return false;
    return read_line(out_fd_, response, timeout_ms);
}

bool GPIOController::command_ok(const std::string& command) {
    std::string response;
    const bool ok = request(command, response) && response == "OK";
    if (!ok) log_msg(LOG_WARNING, "GPIO command failed: %s", command.c_str());
    return ok;
}

bool GPIOController::read_trigger(bool& high) {
    std::string response;
    if (!request("READ", response, 200)) return false;
    high = response == "1";
    return true;
}

void GPIOController::start_worker() {
    worker_running_.store(true);
    worker_ = std::thread(&GPIOController::worker_loop, this);
}

void GPIOController::stop_worker() {
    worker_running_.store(false);
    if (worker_.joinable()) worker_.join();
}

void GPIOController::worker_loop() {
    bool have_sample = false;
    bool last_high = false;

    while (worker_running_.load()) {
        bool high = false;
        if (!read_trigger(high)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            trigger_high_ = high;
        }

        if (!have_sample) {
            have_sample = true;
            last_high = high;
        } else if (high && !last_high) {
            double t_now = now_sec();
            std::lock_guard<std::mutex> lock(trigger_mutex_);
            if (trigger_queue_.size() >= kTriggerQueueMax) {
                trigger_queue_.pop_front();
                log_msg(LOG_WARNING, "GPIO trigger queue overflow");
            }
            trigger_queue_.push_back(t_now);
            log_msg(LOG_WARNING, "GPIO<< PLC TRIGGER enqueued (queue=%d)",
                    static_cast<int>(trigger_queue_.size()));
        }

        last_high = high;
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
}

void GPIOController::close_process() {
    stop_worker();

    if (in_fd_ >= 0 && out_fd_ >= 0) {
        command_ok("ALL_LOW");
        command_ok("QUIT");
    }

    if (in_fd_ >= 0) ::close(in_fd_);
    if (out_fd_ >= 0) ::close(out_fd_);
    in_fd_ = -1;
    out_fd_ = -1;

    if (child_pid_ > 0) {
        int status = 0;
        const pid_t waited = ::waitpid(child_pid_, &status, WNOHANG);
        if (waited == 0) {
            ::kill(child_pid_, SIGTERM);
            ::waitpid(child_pid_, &status, 0);
        }
    }
    child_pid_ = -1;
}

void GPIOController::signal_result(int pin) {
    log_msg(LOG_WARNING, "GPIO>> %s", GPIOConfig::pin_name(pin).c_str());
    if (command_ok(result_command(pin))) {
        result_on_time_ = now_sec();
    }
}

void GPIOController::clear_result() {
    log_msg(LOG_WARNING, "GPIO>> RESULT_OFF");
    command_ok("RESULT WAIT");
}

bool GPIOController::trigger_active() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return trigger_high_;
}

void GPIOController::finish_result_cycle() {
    const int hold_ms = env_int("JETSON_INSPECT_PLC_RESULT_HOLD_MS",
                                env_int("OPENCV_TAIL_PLC_RESULT_HOLD_MS", kDefaultResultHoldMs));
    const double elapsed_ms = result_on_time_ > 0.0 ? (now_sec() - result_on_time_) * 1000.0 : 0.0;
    log_msg(LOG_WARNING, "PLC handshake: holding result for %dms before RESULT_OFF", hold_ms);
    if (elapsed_ms < hold_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms - static_cast<int>(elapsed_ms)));
    }

    clear_result();
}

void GPIOController::set_busy(bool s) {
    log_msg(LOG_WARNING, "GPIO>> BUSY_%s", s ? "ON" : "OFF");
    command_ok(s ? "BUSY 1" : "BUSY 0");
}

bool GPIOController::has_pending_trigger() const {
    std::lock_guard<std::mutex> lock(trigger_mutex_);
    return !trigger_queue_.empty();
}

double GPIOController::consume_trigger() {
    std::lock_guard<std::mutex> lock(trigger_mutex_);
    if (trigger_queue_.empty()) return 0;
    double t = trigger_queue_.front();
    trigger_queue_.pop_front();
    return t;
}

bool GPIOController::toggle_light() {
    light_on_ = !light_on_;
    log_msg(LOG_WARNING, "GPIO light toggle requested, but no Jetson light pin is configured");
    return light_on_;
}

void GPIOController::light_off() {
    light_on_ = false;
}

void GPIOController::cleanup() {
    close_process();
}
