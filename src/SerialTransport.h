#pragma once
// SerialTransport.h
// Thin wrapper around serialib implementing ITransport.

#include "ITransport.h"
#include "serialib.h"
#include <string>

class SerialTransport : public ITransport {
public:
    explicit SerialTransport(const std::string& port, unsigned int baud = 115200)
        : port_(port), baud_(baud) {}

    bool connect() override {
        int ok = serial_.openDevice(port_.c_str(), baud_);
        if (ok != 1) return false;
        serial_.flushReceiver();
        return true;
    }

    void disconnect() override {
        serial_.closeDevice();
    }

    int writeBytes(const char* data, int len) override {
        return serial_.writeBytes(data, len);
    }

    int readBytes(char* buf, int len, unsigned int timeout_ms) override {
        return serial_.readBytes(buf, len, timeout_ms);
    }

    void flushReceiver() override {
        serial_.flushReceiver();
    }

private:
    std::string port_;
    unsigned int baud_;
    serialib serial_;
};
