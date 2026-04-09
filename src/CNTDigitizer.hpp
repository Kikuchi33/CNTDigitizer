#pragma once
// CNTDigitizer.hpp
// Hardware abstraction for the CNT digitizer board.
// Transport-agnostic: pass a SerialTransport or UDPTransport at construction.

#include "ITransport.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class CNTDigitizer {
public:
    static constexpr size_t kNumChannels = 16;

    // Takes ownership of the transport.
    explicit CNTDigitizer(std::unique_ptr<ITransport> transport);
    ~CNTDigitizer();

    // Open the transport. Returns false on failure.
    bool connect();
    void disconnect();

    // Mode commands
    bool enterIdleMode();
    bool enterMeasurementMode();

    // Parameter setters (sent as $PAR\x01 NAME <4-byte LE value>)
    bool setBiasVoltage(int32_t mv);
    bool setGateVoltage(int32_t mv);
    bool setBiasSampleDelay(uint32_t ms);
    bool setMeasurementDelay(uint32_t ms);

    // Packet from the firmware stream
    struct Packet {
        uint32_t timestamp = 0;
        int32_t  currents[kNumChannels]{};
    };

    // Non-blocking packet read. Returns true if a complete packet was parsed.
    bool getPacket(Packet& packet);

private:
    std::unique_ptr<ITransport> transport_;

    bool sendCommand(const std::string& cmd);
    bool sendCommandWithValue(const std::string& param, uint32_t value);
    bool readBytes(char* buffer, size_t length, unsigned int timeout_ms = 500);
    bool readOkAck(unsigned int timeout_ms, bool log_on_fail);
};
