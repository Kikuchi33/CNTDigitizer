#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include "Logger.h"

void logMessage(const std::string& msg) {
    static std::ofstream log;
    static std::mutex log_mutex;

    std::lock_guard<std::mutex> lock(log_mutex);

    if (!log.is_open()) {
        std::filesystem::create_directories("output");
        log.open(std::filesystem::path("output") / "digitizer.log", std::ios::app);
    }
    if (log.is_open()) {
        log << msg << '\n';
        log.flush();
    }
}
