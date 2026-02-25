#pragma once

#include "CNTDigitizer.hpp"
#include <array>
#include <string>
#include <vector>

struct AppState {
    std::string port;
    std::array<char, 128> port_buffer{};
    bool connected = false;
    bool simulate = true;
    bool auto_scroll = true;
    int selected_channel = 0;
    int max_points = 2000;

    std::vector<float> times;
    std::array<std::vector<float>, CNTDigitizer::kNumChannels> channels;
};