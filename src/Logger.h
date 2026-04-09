// Logger.h
#pragma once
#include <string>

// Writes a message to output/digitizer.log
// Thread-safe: safe to call from any thread simultaneously.
void logMessage(const std::string& msg);