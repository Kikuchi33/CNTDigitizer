#include "CNTDigitizer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>   // memcpy, memcmp
#include <sstream>
#include <string>

#include "Logger.h"

// ----------------------------
// Helpers
// ----------------------------

// Read a line-ish ACK like "$OK\r\n" with small chunk timeouts.
// Returns true if "$OK" prefix is observed. False otherwise.
// NOTE: ACKs may be missing. So this is "best effort", not a religion.
static bool readOkAck(serialib& serial, unsigned int timeout_ms, bool log_on_fail) {
    char buf[32] = {};
    int total = 0;

    const unsigned int chunk_timeout_ms = 20;
    unsigned int elapsed = 0;

    while (elapsed < timeout_ms && total < static_cast<int>(sizeof(buf))) {
        int ret = serial.readBytes(buf + total, 1, chunk_timeout_ms);
        if (ret == 1) {
            // stop at newline-ish
            if (buf[total] == '\r' || buf[total] == '\n') {
                ++total;
                break;
            }
            ++total;
        } else {
            elapsed += chunk_timeout_ms;
        }
    }

    if (total <= 0) {
        if (log_on_fail) {
            logMessage("Warning: No ACK received (known issue). Continuing anyway because life is short.");
        }
        return false;
    }

    std::string resp(buf, total);
    while (!resp.empty() && (resp.back() == '\r' || resp.back() == '\n')) {
        resp.pop_back();
    }

    if (resp.rfind("$OK", 0) == 0) {
        return true;
    }

    if (log_on_fail) {
        std::ostringstream oss;
        oss << "Warning: Unexpected ACK bytes (still treating as non-fatal): [";
        for (int i = 0; i < total; ++i) {
            oss << std::hex << static_cast<int>(static_cast<unsigned char>(buf[i])) << " ";
        }
        oss << "]";
        logMessage(oss.str());
    }

    return false;
}

// Best-effort write of a string command (no terminator logic here; protocol handles it).
static void writeCmd(serialib& serial, const std::string& cmd) {
    serial.writeString(cmd.c_str());
}

// ----------------------------
// CNTDigitizer
// ----------------------------

CNTDigitizer::CNTDigitizer(const std::string& port) : port_name(port) {}

CNTDigitizer::~CNTDigitizer() {
    disconnect();
}

bool CNTDigitizer::connect() {
    logMessage("Note: ACK for parameter commands may be missing (known issue). Treating as non-fatal.");
    const int ok = serial.openDevice(port_name.c_str(), 115200);
    if (ok != 1) {
        logMessage("Error: Failed to open serial device on port: " + port_name);
        return false;
    }

    // Flush any garbage in the RX buffer so first packet parsing isn't an archaeological dig.
    serial.flushReceiver();
    return true;
}

void CNTDigitizer::disconnect() {
    serial.closeDevice();
}

bool CNTDigitizer::sendCommand(const std::string& cmd, char terminator) {
    (void)terminator; // keeping signature for compatibility; terminator is unused in current protocol
    writeCmd(serial, cmd);

    // "Best effort" ACK read. If it doesn't show, we don't brick the GUI.
    (void)readOkAck(serial, 150, false);
    return true;
}

bool CNTDigitizer::sendCommandWithValue(const std::string& param, uint32_t value) {
    // Protocol: "$PAR\x01" + param + 4-byte LE value
    const std::string cmd = std::string("$PAR\x01") + param;

    // Flush before parameter write to avoid interpreting old bytes as ACK.
    serial.flushReceiver();
    writeCmd(serial, cmd);

    // Send little-endian 32-bit value
    char packed[4];
    std::memcpy(packed, &value, 4);
    serial.writeBytes(packed, 4);

    // Some firmware versions are allergic to replying. We'll pretend it's fine.
    (void)readOkAck(serial, 150, true);
    return true;
}

bool CNTDigitizer::enterIdleMode() {
    serial.flushReceiver();
    return sendCommand("$RUN\x00");
}

bool CNTDigitizer::enterMeasurementMode() {
    logMessage("Sending $RUN\\x01 command.");
    serial.flushReceiver();
    writeCmd(serial, "$RUN\x01");

    // Teensy may respond with "$OK" before streaming MEAS packets.
    // Keep this short so the UI doesn't feel like it's running on a toaster.
    char ack[32] = {};
    const int ack_ret = serial.readString(ack, '\n', sizeof(ack), 150);

    if (ack_ret > 0) {
        std::string ack_str(ack, ack_ret);
        while (!ack_str.empty() && (ack_str.back() == '\r' || ack_str.back() == '\n')) {
            ack_str.pop_back();
        }

        if (ack_str.rfind("$OK", 0) != 0) {
            std::ostringstream oss;
            oss << "Warning: Unexpected response after $RUN\\x01: [";
            for (int i = 0; i < ack_ret; ++i) {
                oss << std::hex << static_cast<int>(static_cast<unsigned char>(ack[i])) << " ";
            }
            oss << "] (Continuing anyway.)";
            logMessage(oss.str());
            // We don't hard-fail here because firmware might already be streaming.
        }
    } else {
        // No ACK — still ok if device streams packets.
        logMessage("Warning: No $OK after $RUN\\x01 (known issue). Assuming measurement mode anyway.");
    }

    logMessage("Entered measurement mode.");
    return true;
}

bool CNTDigitizer::setBiasVoltage(uint32_t mv) {
    return sendCommandWithValue("BIAS_VOLTAGE", mv);
}

bool CNTDigitizer::setGateVoltage(uint32_t mv) {
    return sendCommandWithValue("GATE_VOLTAGE", mv);
}

bool CNTDigitizer::setBiasSampleDelay(uint32_t ms) {
    return sendCommandWithValue("BIAS_SAMPLE_DELAY", ms);
}

bool CNTDigitizer::setMeasurementDelay(uint32_t ms) {
    return sendCommandWithValue("MEASUREMENT_DELAY", ms);
}

// Read exactly length bytes with a timeout; returns true only if full payload arrived.
bool CNTDigitizer::readBytes(char* buffer, size_t length, unsigned int timeout_ms) {
    return serial.readBytes(buffer, length, timeout_ms) == static_cast<int>(length);
}

bool CNTDigitizer::getPacket(Packet& packet) {
    // Packet format expected (after '$'):
    //   "MEAS" (4 bytes)
    //   timestamp (uint32_t, 4 bytes, little-endian)
    //   currents[16] (int32_t, 64 bytes, little-endian)
    //
    // Total payload after '$' = 72 bytes.
    //
    // IMPORTANT: keep these small so the reader thread can stop quickly.
    constexpr unsigned int dollar_timeout_ms  = 5;   // per-byte wait while hunting '$'
    constexpr unsigned int payload_timeout_ms = 20;  // wait for remaining payload

    // Step 1: find '$' start marker
    char ch = 0;
    while (true) {
        const int r = serial.readBytes(&ch, 1, dollar_timeout_ms);
        if (r != 1) {
            return false; // no data right now (normal)
        }
        if (ch == '$') break;
        // else; :).
    }

    // Step 2: read fixed payload
    char buf[72];
    if (!readBytes(buf, sizeof(buf), payload_timeout_ms)) {
        // Partial packet: let caller try again; stream will likely resync on next '$'
        return false;
    }

    // Step 3: validate type
    if (std::memcmp(buf, "MEAS", 4) != 0) {
        // Could be "$OK" or cosmic rays or something else in the stream. Not the packet.
        // Returning false makes the reader keep scanning.
        return false;
    }

    // Step 4: unpack
    std::memcpy(&packet.timestamp, buf + 4, 4);
    std::memcpy(packet.currents,  buf + 8, 64);

    return true;
}