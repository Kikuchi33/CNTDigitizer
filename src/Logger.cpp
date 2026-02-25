#include <filesystem>
#include <iostream>
#include <cstring> 
#include <fstream>
#include <sstream>
#include "Logger.h"

void logMessage(const std::string& msg) {
    static std::ofstream log;
    if (!log.is_open()) {
        std::filesystem::create_directories("output");
        log.open(std::filesystem::path("output") / "digitizer.log", std::ios::app);
    }
    if (log.is_open()) {
        log << msg << '\n';
        log.flush();
    }
}