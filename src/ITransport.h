#pragma once
// ITransport.h
// Abstract byte-level transport so CNTDigitizer doesn't care whether it's
// talking over serial or WiFi UDP.

#include <cstddef>
#include <string>

struct ITransport {
    virtual ~ITransport() = default;

    // Open/close the connection. connect() returns true on success.
    virtual bool connect() = 0;
    virtual void disconnect() = 0;

    // Write raw bytes. Returns number of bytes written (or -1 on error).
    virtual int writeBytes(const char* data, int len) = 0;
    int writeString(const char* s) {
        int len = 0;
        while (s[len]) ++len;
        return writeBytes(s, len);
    }

    // Read up to `len` bytes with `timeout_ms` deadline.
    // Returns number of bytes actually read (may be 0).
    virtual int readBytes(char* buf, int len, unsigned int timeout_ms) = 0;

    // Discard any buffered RX data.
    virtual void flushReceiver() = 0;
};
