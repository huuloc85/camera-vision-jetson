// -*- coding: utf-8 -*-
// gpio/gpio_controller.cpp — UART serial GPIO via ESP32
// Ported from full_calb.cpp — extracted into standalone module
#include "gpio/gpio_controller.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/time_utils.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <cctype>
#include <unistd.h>
#include <fcntl.h>

#ifdef __linux__
#include <termios.h>
#endif

using core::now_sec;
using core::sleep_sec;

static std::string trim_ascii(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) {
        first++;
    }
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) {
        last--;
    }
    return s.substr(first, last - first);
}

static bool write_all_nonblocking(int fd, const std::string& msg, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    size_t offset = 0;
    while (offset < msg.size()) {
        ssize_t n = ::write(fd, msg.data() + offset, msg.size() - offset);
        if (n > 0) {
            offset += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return false;
    }
    return true;
}

static int open_serial(const char* device, int baud) {
#ifdef __linux__
    int fd = ::open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        log_msg(LOG_ERROR, "UART: Failed to open %s", device);
        return -1;
    }
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) { ::close(fd); return -1; }

    speed_t speed = B115200;
    if (baud == 9600) speed = B9600;
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8 | CREAD | CLOCAL;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | INLCR | ICRNL | IGNCR);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    tcflush(fd, TCIFLUSH);
    if (tcsetattr(fd, TCSANOW, &tty) != 0) { ::close(fd); return -1; }
    return fd;
#else
    (void)device; (void)baud;
    return -1;
#endif
}

GPIOController::GPIOController() {
    log_msg(LOG_WARNING, "UART GPIO: %s @ %d baud",
            SerialConfig::UART_DEVICE, SerialConfig::BAUD_RATE);
    last_uart_rx_.store(now_sec());
    serial_fd_.store(open_serial(SerialConfig::UART_DEVICE, SerialConfig::BAUD_RATE));
    if (serial_fd_.load() >= 0) {
        log_msg(LOG_WARNING, "UART: Open OK (fd=%d)", serial_fd_.load());
    } else {
        log_msg(LOG_ERROR, "UART: Open FAILED! Check: sudo chmod 666 %s",
                SerialConfig::UART_DEVICE);
    }

    // Wait for ESP32 READY (max 5s)
    if (serial_fd_.load() >= 0) {
        auto start = std::chrono::steady_clock::now();
        std::string buf;
        bool ready = false;
        while (!ready) {
            double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > 5.0) {
                log_msg(LOG_WARNING, "UART: ESP32 READY timeout");
                break;
            }
            char c;
            int fd = serial_fd_.load();
            int n = ::read(fd, &c, 1);
            if (n > 0) {
                if (c == '\n' || c == '\r') {
                    if (!buf.empty() &&
                        buf.find(SerialConfig::MSG_READY) != std::string::npos) {
                        log_msg(LOG_WARNING, "UART: ESP32 READY received");
                        ready = true;
                    }
                    buf.clear();
                } else buf += c;
            } else sleep_sec(0.01);
        }
    }

    send_command(SerialConfig::CMD_LIGHT_ON);
    light_on_ = true;
    start_uart_listener();
    start_uart_writer();
}

GPIOController::~GPIOController() {
    listening_ = false;
    cleanup();
}

bool GPIOController::reopen_serial() {
    if (!listening_) return false;
    std::unique_lock<std::timed_mutex> lock(serial_write_lock_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(300))) return false;
    int old_fd = serial_fd_.exchange(-1);
    if (old_fd >= 0) ::close(old_fd);
    int new_fd = open_serial(SerialConfig::UART_DEVICE, SerialConfig::BAUD_RATE);
    serial_fd_.store(new_fd);
    if (new_fd >= 0) {
        log_msg(LOG_WARNING, "UART: Reconnected (fd=%d)", new_fd);
        return true;
    }
    log_msg(LOG_ERROR, "UART: Reconnect failed");
    return false;
}

bool GPIOController::send_command(const std::string& cmd) {
    std::string msg = cmd + "\n";
    for (int attempt = 0; attempt < 2; attempt++) {
        if (serial_fd_.load() < 0 && !reopen_serial()) return false;

        std::unique_lock<std::timed_mutex> lock(serial_write_lock_, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::milliseconds(200))) return false;

        int fd = serial_fd_.load();
        if (fd < 0) continue;
        if (write_all_nonblocking(fd, msg, 200)) return true;

        int err = errno;
        log_msg(LOG_WARNING, "UART write failed for '%s' (errno=%d)",
                cmd.c_str(), err);
        int old_fd = serial_fd_.exchange(-1);
        if (old_fd >= 0) ::close(old_fd);
        lock.unlock();
        if (listening_) reopen_serial();
    }
    return false;
}

void GPIOController::enqueue_command(const std::string& cmd) {
    constexpr size_t CMD_QUEUE_MAX = 32;
    std::lock_guard<std::mutex> lock(cmd_mutex_);
    if (cmd_queue_.size() >= CMD_QUEUE_MAX) {
        cmd_queue_.pop_front();
        log_msg(LOG_WARNING, "UART command queue overflow, dropping oldest");
    }
    cmd_queue_.push_back(cmd);
}

void GPIOController::start_uart_listener() {
    uart_listener_thread_ = std::thread([this]() {
        constexpr double PING_INTERVAL_S        = 15.0;
        constexpr double STATUS_INTERVAL_S      = 120.0;
        constexpr double STALE_WARN_S           = 45.0;
        constexpr double TRIGGER_DEBOUNCE_S     = 0.04;
        constexpr size_t TRIGGER_QUEUE_MAX      = 8;
        std::string buf;
        double last_reopen_try  = 0.0;
        double last_ping        = 0.0;
        double last_status      = 0.0;
        double last_stale_warn  = 0.0;
        double last_trig_overflow_warn = 0.0;

        while (listening_) {
            double now = now_sec();

            // Periodic PING (non-critical → async queue)
            if (now - last_ping >= PING_INTERVAL_S) {
                enqueue_command("PING");
                cmd_cv_.notify_one();
                last_ping = now;
            }
            if (now - last_status >= STATUS_INTERVAL_S) {
                enqueue_command("STATUS");
                cmd_cv_.notify_one();
                last_status = now;
            }

            double last_rx = last_uart_rx_.load();
            if (now - last_rx >= STALE_WARN_S && now - last_stale_warn >= STALE_WARN_S) {
                log_msg(LOG_WARNING, "UART: no ESP32 msg for %.0fs", now - last_rx);
                last_stale_warn = now;
            }

            if (serial_fd_.load() < 0) {
                if (now - last_reopen_try >= 1.0) {
                    reopen_serial();
                    last_reopen_try = now;
                }
                sleep_sec(0.05);
                continue;
            }

            char c;
            int fd = serial_fd_.load();
            int n = ::read(fd, &c, 1);
            if (n > 0) {
                if (c == '\n' || c == '\r') {
                    if (!buf.empty()) {
                        buf = trim_ascii(buf);
                        last_uart_rx_.store(now_sec());
                        if (buf == SerialConfig::MSG_TRIGGER) {
                            double t_now = now_sec();
                            double last_enq = last_trigger_enqueue_.load();
                            if (t_now - last_enq >= TRIGGER_DEBOUNCE_S) {
                                last_trigger_enqueue_.store(t_now);
                                std::lock_guard<std::mutex> lock(trigger_mutex_);
                                if (trigger_queue_.size() >= TRIGGER_QUEUE_MAX) {
                                    trigger_queue_.pop_front();
                                    if (t_now - last_trig_overflow_warn >= 2.0) {
                                        log_msg(LOG_WARNING, "Trigger queue overflow");
                                        last_trig_overflow_warn = t_now;
                                    }
                                }
                                trigger_queue_.push_back(t_now);
                                log_msg(LOG_WARNING, "UART<< TRIGGER enqueued (queue=%d)",
                                        (int)trigger_queue_.size());
                            } else {
                                log_msg(LOG_WARNING, "UART<< TRIGGER debounced (%.1fms)",
                                        (t_now - last_enq) * 1000.0);
                            }
                        } else if (buf.find(SerialConfig::MSG_READY) != std::string::npos
                                   || buf.rfind("ESP32 ", 0) == 0 || buf == "PONG") {
                            log_msg(LOG_WARNING, "UART<< %s", buf.c_str());
                        } else {
                            log_msg(LOG_WARNING, "UART<< UNKNOWN [%s]", buf.c_str());
                        }
                        buf.clear();
                    }
                } else {
                    buf += c;
                    if (buf.size() > 128) {
                        log_msg(LOG_WARNING, "UART line too long, dropping buffer");
                        buf.clear();
                    }
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                log_msg(LOG_WARNING, "UART read error (errno=%d), reconnecting...", errno);
                {
                    std::unique_lock<std::timed_mutex> lock(serial_write_lock_, std::defer_lock);
                    if (lock.try_lock_for(std::chrono::milliseconds(100))) {
                        int old_fd = serial_fd_.exchange(-1);
                        if (old_fd >= 0) ::close(old_fd);
                    }
                }
                reopen_serial();
                sleep_sec(0.05);
            } else {
                sleep_sec(0.001);
            }
        }
    });
}

void GPIOController::start_uart_writer() {
    uart_writer_thread_ = std::thread([this]() {
        while (listening_) {
            std::string cmd;
            {
                std::unique_lock<std::mutex> lock(cmd_mutex_);
                cmd_cv_.wait_for(lock, std::chrono::milliseconds(100),
                    [this]() { return !cmd_queue_.empty() || !listening_; });
                if (!listening_) break;
                if (cmd_queue_.empty()) continue;
                cmd = cmd_queue_.front();
                cmd_queue_.pop_front();
            }
            if (!send_command(cmd)) {
                log_msg(LOG_WARNING, "UART writer: failed '%s'", cmd.c_str());
                if (cmd == SerialConfig::CMD_OK_ON || cmd == SerialConfig::CMD_NG_ON) {
                    send_command(SerialConfig::CMD_BUSY_OFF);
                }
            }
        }
    });
}

void GPIOController::signal_result(int pin) {
    std::string cmd;
    if (pin == SerialConfig::PIN_OK) cmd = SerialConfig::CMD_OK_ON;
    else if (pin == SerialConfig::PIN_NG) cmd = SerialConfig::CMD_NG_ON;
    else return;
    // CRITICAL PATH: direct UART (not queued) for minimal latency
    log_msg(LOG_WARNING, "UART>> %s (direct)", cmd.c_str());
    if (!send_command(cmd)) {
        log_msg(LOG_WARNING, "UART>> %s FAILED, retry once", cmd.c_str());
        send_command(cmd);
    }
}

void GPIOController::set_busy(bool s) {
    std::string cmd = s ? SerialConfig::CMD_BUSY_ON : SerialConfig::CMD_BUSY_OFF;
    if (!send_command(cmd)) {
        log_msg(LOG_WARNING, "UART>> %s FAILED", cmd.c_str());
    }
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
    send_command(light_on_ ? SerialConfig::CMD_LIGHT_ON : SerialConfig::CMD_LIGHT_OFF);
    return light_on_;
}

void GPIOController::light_off() {
    light_on_ = false;
    send_command(SerialConfig::CMD_LIGHT_OFF);
}

void GPIOController::cleanup() {
    listening_ = false;
    cmd_cv_.notify_all();
    if (uart_listener_thread_.joinable()) uart_listener_thread_.join();
    if (uart_writer_thread_.joinable()) uart_writer_thread_.join();
    light_off();
    send_command(SerialConfig::CMD_BUSY_OFF);
    std::unique_lock<std::timed_mutex> lock(serial_write_lock_, std::defer_lock);
    if (lock.try_lock_for(std::chrono::milliseconds(200))) {
        int old_fd = serial_fd_.exchange(-1);
        if (old_fd >= 0) ::close(old_fd);
    }
}
