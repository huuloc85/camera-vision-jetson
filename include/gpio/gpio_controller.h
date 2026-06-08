// -*- coding: utf-8 -*-
// gpio/gpio_controller.h — UART GPIO via ESP32 bridge
// Interface-first: Vision calls this, không biết cụ thể UART hay gì
#pragma once

#include <string>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>

// ══════════════════════════════════════════════════════
// GPIOController — async UART writer + trigger listener
// ══════════════════════════════════════════════════════
class GPIOController {
public:
    GPIOController();
    ~GPIOController();

    // Signal OK or NG to PLC (CRITICAL PATH — direct UART, not queued)
    void signal_result(int pin);

    // Set BUSY state in-order with OK/NG result pulses
    void set_busy(bool state);

    // Trigger queue — filled by UART listener thread
    bool   has_pending_trigger() const;
    double consume_trigger();        // Returns trigger timestamp

    // Light control
    bool toggle_light();
    void light_off();

    void cleanup();

    bool light_on_ = true;

private:
    bool send_command(const std::string& cmd);
    bool reopen_serial();
    void enqueue_command(const std::string& cmd);
    void start_uart_listener();
    void start_uart_writer();

    std::atomic<int> serial_fd_{-1};
    std::timed_mutex serial_write_lock_;
    std::thread uart_listener_thread_;
    std::deque<double> trigger_queue_;
    mutable std::mutex trigger_mutex_;
    std::atomic<bool>   listening_{true};
    std::atomic<double> last_uart_rx_{0.0};
    std::atomic<double> last_trigger_enqueue_{0.0};

    // Async command queue for non-critical commands (PING, STATUS)
    std::thread   uart_writer_thread_;
    std::mutex    cmd_mutex_;
    std::condition_variable cmd_cv_;
    std::deque<std::string> cmd_queue_;
};
