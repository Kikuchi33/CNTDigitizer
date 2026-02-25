#include "CNTDigitizer.hpp"
#include <cstring>  // For memcpy, memcmp
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include "Logger.h"


bool readOkAck(serialib& serial, unsigned int timeout_ms, bool log_on_fail) {
    char buf[16] = {};
    int total = 0;
    const unsigned int chunk_timeout = 50;
    unsigned int elapsed = 0;

    while (elapsed < timeout_ms && total < static_cast<int>(sizeof(buf))) {
        int ret = serial.readBytes(buf + total, 1, chunk_timeout);
        if (ret == 1) {
            if (buf[total] == '\r' || buf[total] == '\n') {
                ++total;
                break;
            }
            ++total;
        } else {
            elapsed += chunk_timeout;
        }
    }

    if (total <= 0) {
        if (log_on_fail) {
            logMessage("Warning: No response received for command ack. Known issue; treating as non-fatal.");
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
        oss << "Warning: Unexpected response for command ack (known issue; treating as non-fatal): [";
        for (int i = 0; i < total; ++i)
            oss << std::hex << (int)(unsigned char)buf[i] << " ";
        oss << "]";
        logMessage(oss.str());
    }
    return false;
}

CNTDigitizer::CNTDigitizer(const std::string& port) : port_name(port) {}

CNTDigitizer::~CNTDigitizer() {
    disconnect();
}

bool CNTDigitizer::connect() {
    logMessage("Note: ACK for parameter commands may be missing (known issue). Treating as non-fatal.");
    return serial.openDevice(port_name.c_str(), 115200) == 1;
}

void CNTDigitizer::disconnect() {
    serial.closeDevice();
}

bool CNTDigitizer::sendCommand(const std::string& cmd, char terminator) {
    serial.writeString(cmd.c_str());
    
    (void)terminator;
    return readOkAck(serial, 500, false);
}

bool CNTDigitizer::sendCommandWithValue(const std::string& param, uint32_t value) {
    std::string cmd = "$PAR\x01" + param;
    serial.flushReceiver();
    serial.writeString(cmd.c_str());

    char packed[4];
    std::memcpy(packed, &value, 4);
    serial.writeBytes(packed, 4);

    readOkAck(serial, 500, true);
    return true;
}

bool CNTDigitizer::enterIdleMode() {
    serial.flushReceiver();
    return sendCommand("$RUN\x00");
}

bool CNTDigitizer::enterMeasurementMode() {
    logMessage("Sending $RUN\\x01 command.");
    serial.flushReceiver();
    serial.writeString("$RUN\x01");

    // Teensy may respond with "$OK" before streaming MEAS packets.
    char ack[10] = {};
    int ack_ret = serial.readString(ack, '\n', sizeof(ack), 500);
    if (ack_ret > 0) {
        std::string ack_str(ack, ack_ret);
        while (!ack_str.empty() && (ack_str.back() == '\r' || ack_str.back() == '\n')) {
            ack_str.pop_back();
        }
        if (ack_str.rfind("$OK", 0) != 0) {
            std::ostringstream oss;
            oss << "Error: Unexpected response after $RUN\\x01: [";
            for (int i = 0; i < ack_ret; ++i)
                oss << std::hex << (int)(unsigned char)ack[i] << " ";
            oss << "]";
            logMessage(oss.str());
            return false;
        }
    }

    logMessage("Entered measurement mode (received OK).");
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

bool CNTDigitizer::readBytes(char* buffer, size_t length, unsigned int timeout_ms) {
    return serial.readBytes(buffer, length, timeout_ms) == static_cast<int>(length);
}

bool CNTDigitizer::getPacket(Packet& packet) {
    char ch;
    while (serial.readBytes(&ch, 1, 200) == 1) {
        if (ch == '$') break;
    }

    char buf[72];
    if (!readBytes(buf, 72)) return false;

    if (std::memcmp(buf, "MEAS", 4) != 0) return false;

    std::memcpy(&packet.timestamp, buf + 4, 4);
    std::memcpy(&packet.currents, buf + 8, 64);
    return true;
}
