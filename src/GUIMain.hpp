#pragma once
// GUIMain.hpp  v2.1

#include "CNTDigitizer.hpp"
#include <array>
#include <deque>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Ring buffer – O(1) push_back / pop_front; no heap reallocation on overflow.
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
        if (tail_ == head_) head_ = next(head_);
    }

    void clear() { head_ = tail_ = 0; }

    size_t size() const {
        return (tail_ >= head_) ? (tail_ - head_) : (buf_.size() - head_ + tail_);
    }

    bool empty() const { return head_ == tail_; }

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
// CsvOverlay – a loaded CSV file rendered as a ghost trace on the live plot
// ---------------------------------------------------------------------------
struct CsvOverlay {
    std::string              filename;   // display name (basename only)
    std::vector<float>       times;
    // One vector per channel; may be empty if that channel wasn't in the file
    std::array<std::vector<float>, CNTDigitizer::kNumChannels> channels;
    bool                     visible = true;
    // Metadata parsed from CSV header comments
    std::string              meta;       // raw "#" header lines joined
};

// ---------------------------------------------------------------------------
// Session – one measurement run with metadata
// ---------------------------------------------------------------------------
struct Session {
    std::string name;
    double      start_wall   = 0.0;
    int32_t     bias_mv      = 1000;
    int32_t     gate_mv      = -1000;
    uint32_t    bias_sample_delay_ms  = 1000;
    uint32_t    measurement_delay_ms  = 100;
    std::string notes;       // free-text saved into CSV header
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
    bool use_wifi   = false;

    // WiFi settings
    std::array<char, 64>  wifi_teensy_ip{};
    uint16_t wifi_listen_port  = 5005;
    uint16_t wifi_teensy_port  = 5006;

    // Plot control
    bool  auto_scroll       = true;
    float scroll_window_sec = 10.f;   // user-adjustable auto-scroll window
    bool  show_all_ch       = false;
    bool  ema_enabled       = false;
    float ema_alpha         = 0.2f;
    int   selected_channel  = 0;
    int   max_points        = 2000;

    // Live plot data (ring buffers)
    RingBuffer<float> times;
    std::array<RingBuffer<float>, CNTDigitizer::kNumChannels> channels;
    std::array<float, CNTDigitizer::kNumChannels> ema_state{};

    // Current session metadata
    Session session;

    // Loaded CSV overlays
    std::vector<CsvOverlay> overlays;

    // Flattened vectors for ImPlot (refreshed once per frame)
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
