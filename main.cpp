#include "CNTDigitizer.hpp"
#include <iostream>

int main() {
    // Change this to your actual port:
    // Windows: "COM3", "COM5", etc.
    // macOS/Linux: "/dev/tty.usbmodemXXXX" or "/dev/ttyUSB0"
    CNTDigitizer digitizer("COM4");

    // Open serial port
    if (!digitizer.connect()) {
        std::cerr << "Failed to open serial port.\n";
        return 1;
    }

    std::cout << "Serial port opened successfully.\n";

    // Enter measurement mode
    if (!digitizer.enterMeasurementMode()) {
        std::cerr << "Failed to enter measurement mode.\n";
        return 1;
    }

    std::cout << "Waiting for measurement packet...\n";

    // Attempt to read a data packet
    CNTDigitizer::Packet packet;
    if (digitizer.getPacket(packet)) {
        std::cout << "Packet received!\n";
        std::cout << "Timestamp: " << packet.timestamp << " ms\n";
        for (int i = 0; i < 16; ++i) {
            std::cout << "Channel " << i << ": " << packet.currents[i] << " nA\n";
        }
    } else {
        std::cerr << "Failed to read packet.\n";
    }

    return 0;
}
