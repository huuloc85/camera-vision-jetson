// -*- coding: utf-8 -*-
// gpio/gpio_controller.h — PLC GPIO via Python helper
// Interface-first: Vision calls this, không biết cụ thể GPIO backend là gì
#pragma once

#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <string>

// ══════════════════════════════════════════════════════
// GPIOController — OK/NG/BUSY output + PLC trigger input through helper process
// ══════════════════════════════════════════════════════
class GPIOController {
public:
    GPIOController();
    ~GPIOController();

    // Signal OK or NG to PLC.
    void signal_result(int pin);

    // Clear both result outputs.
    void clear_result();

    // Pulse OK/NG long enough for PLC scan, then clear the result outputs.
    void finish_result_cycle();

    bool trigger_active() const;

    // Set BUSY state in-order with OK/NG result handshakes.
    void set_busy(bool state);

    // Trigger queue — filled by PLC trigger input on Jetson BOARD pin 22.
    bool   has_pending_trigger() const;
    double consume_trigger();        // Returns trigger timestamp

    // Light control
    bool toggle_light();
    void light_off();

    void cleanup();

    bool light_on_ = true;

private:
    bool start_gpio_worker();
    bool request(const std::string& command, std::string& response, int timeout_ms = 500);
    bool command_ok(const std::string& command);
    bool read_trigger(bool& high);
    void start_worker();
    void stop_worker();
    void worker_loop();
    void close_process();

    int in_fd_ = -1;
    int out_fd_ = -1;
    std::atomic<bool> worker_running_{false};
    std::thread worker_;
    int child_pid_ = -1;
    std::mutex io_mutex_;
    mutable std::mutex state_mutex_;
    std::deque<double> trigger_queue_;
    mutable std::mutex trigger_mutex_;
    bool trigger_high_ = false;
    double result_on_time_ = 0.0;
    bool mock_mode_ = false;
};
