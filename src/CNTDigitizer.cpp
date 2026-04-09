#include "CNTDigitizer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

#include "Logger.h"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

CNTDigitizer::CNTDigitizer(std::unique_ptr<ITransport> transport)
    : transport_(std::move(transport)) {}

CNTDigitizer::~CNTDigitizer() {
    disconnect();
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

bool CNTDigitizer::connect() {
    logMessage("CNTDigitizer: opening transport...");
    if (!transport_->connect()) {
        logMessage("CNTDigitizer: transport connect() failed.");
        return false;
    }
    transport_->flushReceiver();
    logMessage("CNTDigitizer: transport connected.");
    return true;
}

void CNTDigitizer::disconnect() {
    transport_->disconnect();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

bool CNTDigitizer::readOkAck(unsigned int timeout_ms, bool log_on_fail) {
    char buf[32] = {};
    int total = 0;

    const unsigned int chunk_ms = 20;
    unsigned int elapsed = 0;

    while (elapsed < timeout_ms && total < (int)sizeof(buf) - 1) {
        int r = transport_->readBytes(buf + total, 1, chunk_ms);
        if (r == 1) {
            if (buf[total] == '\r' || buf[total] == '\n') { ++total; break; }
            ++total;
        } else {
            elapsed += chunk_ms;
        }
    }

    if (total <= 0) {
        if (log_on_fail)
            logMessage("Warning: No ACK received (known firmware issue). Continuing.");
        return false;
    }

    std::string resp(buf, total);
    while (!resp.empty() && (resp.back() == '\r' || resp.back() == '\n'))
        resp.pop_back();

    if (resp.rfind("$OK", 0) == 0) return true;

    if (log_on_fail) {
        std::ostringstream oss;
        oss << "Warning: Unexpected ACK [";
        for (int i = 0; i < total; ++i)
            oss << std::hex << (int)(unsigned char)buf[i] << " ";
        oss << "] (non-fatal)";
        logMessage(oss.str());
    }
    return false;
}

bool CNTDigitizer::readBytes(char* buffer, size_t length, unsigned int timeout_ms) {
    size_t got = 0;
    unsigned int per_byte_ms = std::max(1u, timeout_ms / (unsigned int)length);
    while (got < length) {
        int r = transport_->readBytes(buffer + got, (int)(length - got), per_byte_ms);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

bool CNTDigitizer::sendCommand(const std::string& cmd) {
    transport_->writeBytes(cmd.data(), (int)cmd.size());
    (void)readOkAck(150, false);
    return true;
}

bool CNTDigitizer::sendCommandWithValue(const std::string& param, uint32_t value) {
    // Protocol: "$PAR\x01" + param_name + 4-byte LE value
    std::string cmd = std::string("$PAR\x01") + param;
    transport_->flushReceiver();
    transport_->writeBytes(cmd.data(), (int)cmd.size());

    char packed[4];
    std::memcpy(packed, &value, 4);
    transport_->writeBytes(packed, 4);

    (void)readOkAck(150, true);
    return true;
}

// ---------------------------------------------------------------------------
// Mode commands
// ---------------------------------------------------------------------------

bool CNTDigitizer::enterIdleMode() {
    transport_->flushReceiver();
    return sendCommand("$RUN\x00");
}

bool CNTDigitizer::enterMeasurementMode() {
    logMessage("CNTDigitizer: sending $RUN\\x01");
    transport_->flushReceiver();
    transport_->writeBytes("$RUN\x01", 5);

    char ack[32] = {};
    int r = transport_->readBytes(ack, sizeof(ack) - 1, 200);
    if (r > 0) {
        std::string s(ack, r);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
        if (s.rfind("$OK", 0) != 0) {
            std::ostringstream oss;
            oss << "Warning: unexpected response after $RUN\\x01 [";
            for (int i = 0; i < r; ++i) oss << std::hex << (int)(unsigned char)ack[i] << " ";
            oss << "] (continuing)";
            logMessage(oss.str());
        }
    } else {
        logMessage("Warning: No $OK after $RUN\\x01 (known issue). Assuming measurement mode.");
    }

    logMessage("CNTDigitizer: measurement mode entered.");
    return true;
}

// ---------------------------------------------------------------------------
// Parameter setters  (value in mV cast to uint32 for protocol, signed supported via cast)
// ---------------------------------------------------------------------------

bool CNTDigitizer::setBiasVoltage(int32_t mv) {
    return sendCommandWithValue("BIAS_VOLTAGE", (uint32_t)mv);
}

bool CNTDigitizer::setGateVoltage(int32_t mv) {
    return sendCommandWithValue("GATE_VOLTAGE", (uint32_t)mv);
}

bool CNTDigitizer::setBiasSampleDelay(uint32_t ms) {
    return sendCommandWithValue("BIAS_SAMPLE_DELAY", ms);
}

bool CNTDigitizer::setMeasurementDelay(uint32_t ms) {
    return sendCommandWithValue("MEASUREMENT_DELAY", ms);
}

// ---------------------------------------------------------------------------
// Packet parsing
// ---------------------------------------------------------------------------

bool CNTDigitizer::getPacket(Packet& packet) {
    // Packet: '$' + "MEAS"(4) + timestamp(4) + currents[16](64) = 1 + 72 bytes total
    constexpr unsigned int dollar_ms  = 5;
    constexpr unsigned int payload_ms = 20;

    // Hunt for '$' start byte
    char ch = 0;
    while (true) {
        int r = transport_->readBytes(&ch, 1, dollar_ms);
        if (r != 1) return false;
        if (ch == '$') break;
    }

    char buf[72];
    if (!readBytes(buf, sizeof(buf), payload_ms)) return false;

    if (std::memcmp(buf, "MEAS", 4) != 0) return false;

    std::memcpy(&packet.timestamp, buf + 4, 4);
    std::memcpy(packet.currents,   buf + 8, 64);
    return true;
}
