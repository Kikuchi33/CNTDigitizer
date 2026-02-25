#pragma once

#include "serialib.h"
#include <string>
#include <cstdint>
#include <cstddef>

class CNTDigitizer {
public:
    static constexpr size_t kNumChannels = 16;
    explicit CNTDigitizer(const std::string& port);
    ~CNTDigitizer();

    bool connect();
    void disconnect();

    bool enterIdleMode();
    bool enterMeasurementMode();

    bool setBiasVoltage(uint32_t mv);
    bool setGateVoltage(uint32_t mv);
    bool setBiasSampleDelay(uint32_t ms);
    bool setMeasurementDelay(uint32_t ms);

    struct Packet {
        uint32_t timestamp = 0;
        int32_t currents[kNumChannels]{};
    };

    bool getPacket(Packet& packet);

private:
    std::string port_name;
    serialib serial;

    bool sendCommand(const std::string& cmd, char terminator = '\n');
    bool sendCommandWithValue(const std::string& param, uint32_t value);
    bool readBytes(char* buffer, size_t length, unsigned int timeout_ms = 500);
};
