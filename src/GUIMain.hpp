#pragma once
// GUIMain.hpp

#include "CNTDigitizer.hpp"
#include <array>
#include <deque>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Ring buffer – O(1) push_back / pop_front; no heap reallocation on overflow.
// Used for plot data so we never do a front-erase on a vector.
// ---------------------------------------------------------------------------
template<typename T>
struct RingBuffer {
    explicit RingBuffer(size_t cap = 2000) { reserve(cap); }

    void reserve(size_t cap) {
        buf_.resize(cap + 1);
        capacity_ = cap;
        head_ = tail_ = 0;
    }

    void push(T v) {
        buf_[tail_] = v;
        tail_ = next(tail_);
        if (tail_ == head_) head_ = next(head_); // drop oldest on overflow
    }

    void clear() { head_ = tail_ = 0; }

    size_t size() const {
        return (tail_ >= head_) ? (tail_ - head_) : (buf_.size() - head_ + tail_);
    }

    bool empty() const { return head_ == tail_; }

    // Linear copy into a plain vector for ImPlot (zero-copy path available if needed).
    // Returns number of elements copied.
    size_t linearize(std::vector<T>& out) const {
        size_t n = size();
        out.resize(n);
        for (size_t i = 0; i < n; ++i)
            out[i] = buf_[(head_ + i) % buf_.size()];
        return n;
    }

private:
    size_t next(size_t i) const { return (i + 1) % buf_.size(); }
    std::vector<T> buf_;
    size_t capacity_, head_, tail_;
};

// ---------------------------------------------------------------------------
// Session – one measurement run with metadata
// ---------------------------------------------------------------------------
struct Session {
    std::string name;          // auto-generated from timestamp
    double      start_wall;    // glfwGetTime() at Start
    int32_t     bias_mv  = 1000;
    int32_t     gate_mv  = -1000;
    uint32_t    bias_sample_delay_ms  = 1000;
    uint32_t    measurement_delay_ms  = 100;
};

// ---------------------------------------------------------------------------
// AppState
// ---------------------------------------------------------------------------
struct AppState {
    // Connection
    std::string port;
    std::array<char, 128> port_buffer{};
    bool connected  = false;
    bool simulate   = false;
    bool use_wifi   = false;   // if true, use UDPTransport instead of serial

    // WiFi settings (editable in UI)
    std::array<char, 64>  wifi_teensy_ip{};   // e.g. "192.168.4.1"
    uint16_t wifi_listen_port  = 5005;
    uint16_t wifi_teensy_port  = 5006;

    // Plot control
    bool auto_scroll     = true;
    bool show_all_ch     = false;  // overlay all 16 channels
    bool ema_enabled     = false;  // exponential moving average smoothing
    float ema_alpha      = 0.2f;   // EMA weight [0.01 .. 0.99]
    int   selected_channel = 0;
    int   max_points     = 2000;

    // Plot data (ring buffers – no O(n) erase ever)
    RingBuffer<float> times;
    std::array<RingBuffer<float>, CNTDigitizer::kNumChannels> channels;
    // EMA shadow buffers (linearized at render time)
    std::array<float, CNTDigitizer::kNumChannels> ema_state{};

    // Current session metadata
    Session session;

    // Flatten ring buffers to vectors for ImPlot (called once per frame)
    mutable std::vector<float> times_plot;
    mutable std::array<std::vector<float>, CNTDigitizer::kNumChannels> ch_plot;

    void refreshPlotBuffers() const {
        times.linearize(times_plot);
        for (size_t i = 0; i < CNTDigitizer::kNumChannels; ++i)
            channels[i].linearize(ch_plot[i]);
    }

    void clear() {
        times.clear();
        for (auto& c : channels) c.clear();
        ema_state.fill(0.f);
    }

    void resize(int max_pts) {
        max_points = max_pts;
        times.reserve((size_t)max_pts);
        for (auto& c : channels) c.reserve((size_t)max_pts);
    }
};
