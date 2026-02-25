#include "CNTDigitizer.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem> // C++17


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


std::string loadPort(const std::string& filename = "port.txt") { 
    std::ifstream file(filename); std::string port; 
    if (file.is_open() && std::getline(file, port)) { 
        return port; 
    } else { 
        logMessage("Error: Could not read port from " + filename + ".");
        exit(1);
    } 
    }

int main(int argc, char* argv[]) {
    int duration = 300; // Default
    if (argc > 2 && std::string(argv[1]) == "-t") {
        duration = std::stoi(argv[2]);
    }

    // Change to your actual port if needed
    std::string port = loadPort(); // Read from "port.txt" 
    std::this_thread::sleep_for(std::chrono::seconds(2)); 
    CNTDigitizer digitizer(port);

    if (!digitizer.connect()) {
        logMessage("Error: Failed to connect to digitizer.");
        return 1;
    }

    if (!digitizer.setBiasSampleDelay(10000)) {
        logMessage("Warning: Failed to set bias sample delay (known issue).");
    }

    if (!digitizer.setMeasurementDelay(10000)) {
        logMessage("Warning: Failed to set measurement delay (known issue).");
    }

    logMessage("Starting measurement mode.");
    if (!digitizer.enterMeasurementMode()) {
        logMessage("Error: Failed to enter measurement mode.");
        return 1;
    }

    std::vector<CNTDigitizer::Packet> packets;

    for (int i = 0; i < 1200; ++i) {
        CNTDigitizer::Packet packet;
        if (digitizer.getPacket(packet)) {
            packets.push_back(packet);

            std::cout << "Packet " << i << " | Timestamp: " << packet.timestamp << " | Currents: ";
            for (int j = 0; j < 16; ++j)
                std::cout << packet.currents[j] << ' ';
            std::cout << '\n';
        } else {
            logMessage("Warning: Failed to read packet #" + std::to_string(i) + ".");
        }
    }


    if (!packets.empty())
        packets.erase(packets.begin());

    // Generate a unique filename like output0.txt, output1.txt, ...
    int num = 0;
    std::string newfilename;
    do {
        newfilename = "output" + std::to_string(num++) + ".txt";
    } while (std::filesystem::exists(std::filesystem::path("output") / newfilename));

    // Write to file
    std::filesystem::create_directories("output");
    std::ofstream file(std::filesystem::path("output") / newfilename);
    if (!file.is_open()) {
        logMessage("Error: Failed to open file for writing: " + newfilename + ".");
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds(1)); // Optional delay before writing

    for (const auto& packet : packets) {
        file << packet.timestamp;
        for (int i = 0; i < 16; ++i)
            file << ' ' << packet.currents[i];
        file << '\n';
    }

    file.close();
    logMessage("Saved output to " + newfilename + ".");

    return 0;
}
