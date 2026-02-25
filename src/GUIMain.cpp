// GUIMain.cpp 1.0 Release
// - ImGui + ImPlot + GLFW + OpenGL3 backend
// - Adds: real device connect/disconnect, start/stop measurement, status panel,
//         CSV save for plotted dataset, basic error handling, simulation mode,
//         and framebuffer screenshot (BMP) capture.
// - True fullscreen toggle (F11) + 30/70 split layout that always fills the GLFW window.
#include "GUIMain.hpp"

#include "imgui.h"
#include "implot.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "CNTDigitizer.hpp"
#include "Logger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// ----------------------------
// Screenshot: Save framebuffer to BMP
// Capture theoretically happens AFTER ImGui render and BEFORE swap buffers.
// Read from GL_BACK to get the just-rendered frame.
// This is very slow, perhaps a workaround exists but imgui has no relevant library
// ----------------------------
static bool SaveFramebufferBMP(const std::string& filename, int width, int height) {
    std::filesystem::create_directories("output");
    std::filesystem::path path = std::filesystem::path("output") / filename;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    const int row_stride = width * 3;
    const int row_padded = (row_stride + 3) & ~3;
    const int data_size = row_padded * height;
    const int file_size = 54 + data_size;

    unsigned char header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    header[2] = static_cast<unsigned char>(file_size);
    header[3] = static_cast<unsigned char>(file_size >> 8);
    header[4] = static_cast<unsigned char>(file_size >> 16);
    header[5] = static_cast<unsigned char>(file_size >> 24);
    header[10] = 54;
    header[14] = 40;
    header[18] = static_cast<unsigned char>(width);
    header[19] = static_cast<unsigned char>(width >> 8);
    header[20] = static_cast<unsigned char>(width >> 16);
    header[21] = static_cast<unsigned char>(width >> 24);
    header[22] = static_cast<unsigned char>(height);
    header[23] = static_cast<unsigned char>(height >> 8);
    header[24] = static_cast<unsigned char>(height >> 16);
    header[25] = static_cast<unsigned char>(height >> 24);
    header[26] = 1;
    header[28] = 24;

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;

    out.write(reinterpret_cast<char*>(header), 54);

    std::vector<unsigned char> row(static_cast<size_t>(row_padded), 0);
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(row_stride);
        for (int x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 3 + 2]; // B
            row[x * 3 + 1] = src[x * 3 + 1]; // G
            row[x * 3 + 2] = src[x * 3 + 0]; // R
        }
        out.write(reinterpret_cast<char*>(row.data()), row_padded);
    }
    return true;
}

// ----------------------------
// Helpers
// ----------------------------
static void trimRight(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
}

static void copyPortToBuffer(AppState& state) {
    std::fill(state.port_buffer.begin(), state.port_buffer.end(), '\0');
    size_t count = std::min(state.port.size(), state.port_buffer.size() - 1);
    std::copy_n(state.port.begin(), count, state.port_buffer.begin());
}

static bool LoadPortFile(const std::string& path, std::string& out_port) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::getline(file, out_port);
    trimRight(out_port);
    return !out_port.empty();
}

static bool SavePortFile(const std::string& path, const std::string& port) {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << port << '\n';
    return true;
}

static void ClearData(AppState& state) {
    state.times.clear();
    for (auto& series : state.channels) series.clear();
}

static void AppendPacket(AppState& state, const CNTDigitizer::Packet& packet, float time_sec) {
    state.times.push_back(time_sec);

    const size_t n = std::min(state.channels.size(), std::size(packet.currents));
    for (size_t ch = 0; ch < n; ++ch) {
        state.channels[ch].push_back(static_cast<float>(packet.currents[ch]));
    }

    if (static_cast<int>(state.times.size()) > state.max_points) {
        size_t remove = state.times.size() - static_cast<size_t>(state.max_points);
        state.times.erase(state.times.begin(), state.times.begin() + static_cast<long long>(remove));
        for (auto& series : state.channels) {
            series.erase(series.begin(), series.begin() + static_cast<long long>(remove));
        }
    }
}

static std::string nowTimestampString() {
    using namespace std::chrono;
    const auto t = system_clock::now();
    const auto tt = system_clock::to_time_t(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

static bool SaveDatasetCSV(const AppState& state, const std::string& filename, bool save_all_channels) {
    std::filesystem::create_directories("output");
    std::ofstream out(std::filesystem::path("output") / filename, std::ios::trunc);
    if (!out.is_open()) return false;

    out << "t_sec";
    if (save_all_channels) {
        for (int ch = 0; ch < static_cast<int>(state.channels.size()); ++ch) out << ",ch" << ch;
    } else {
        out << ",ch" << state.selected_channel;
    }
    out << "\n";

    const size_t n = state.times.size();
    for (size_t i = 0; i < n; ++i) {
        out << std::fixed << std::setprecision(6) << state.times[i];
        if (save_all_channels) {
            for (int ch = 0; ch < static_cast<int>(state.channels.size()); ++ch) {
                if (i < state.channels[ch].size()) out << "," << state.channels[ch][i];
                else out << ",";
            }
        } else {
            const int ch = state.selected_channel;
            if (ch >= 0 && ch < static_cast<int>(state.channels.size()) && i < state.channels[ch].size()) {
                out << "," << state.channels[ch][i];
            } else {
                out << ",";
            }
        }
        out << "\n";
    }
    return true;
}

static void DrawStatusBadge(bool ok, const char* ok_text, const char* bad_text) {
    ImVec4 col = ok ? ImVec4(0.20f, 0.85f, 0.35f, 1.0f) : ImVec4(0.95f, 0.25f, 0.25f, 1.0f);
    ImGui::TextColored(col, "%s", ok ? ok_text : bad_text);
}

// ----------------------------
// Fullscreen toggle support
// ----------------------------
struct WindowRestoreInfo {
    int x = 100;
    int y = 100;
    int w = 1360;
    int h = 780;
    bool valid = false;
};

static void SetFullscreen(GLFWwindow* window, bool fullscreen, WindowRestoreInfo& restore) {
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);

    if (fullscreen) {
        // Save current windowed placement
        glfwGetWindowPos(window, &restore.x, &restore.y);
        glfwGetWindowSize(window, &restore.w, &restore.h);
        restore.valid = true;

        // Switch to fullscreen
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        // Restore windowed placement
        if (!restore.valid) {
            restore.x = 100; restore.y = 100; restore.w = 1360; restore.h = 780;
        }
        glfwSetWindowMonitor(window, nullptr, restore.x, restore.y, restore.w, restore.h, 0);
    }
}

int main() {
    if (!glfwInit()) return 1;

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    // Start fullscreen by default (toggle with F11 to go windowed)
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);

    GLFWwindow* window = glfwCreateWindow(mode->width, mode->height, "CNT Digitizer", monitor, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::StyleColorsDark();

    //UI SCALE
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = 1.8f;

ImGui_ImplGlfw_InitForOpenGL(window, true);
ImGui_ImplOpenGL3_Init(glsl_version);

    AppState state;

    // Defaults
    state.connected = false;
    state.simulate = false;
    state.auto_scroll = true;
    state.max_points = std::max(200, state.max_points);
    state.selected_channel = std::clamp(state.selected_channel, 0, 15);

    bool measuring = false;
    bool save_all_channels = true;
    bool auto_clear_on_start = false;

    bool request_screenshot = false;
    std::string screenshot_name;

    std::string status_line = "Idle.";
    std::string last_error;

    std::unique_ptr<CNTDigitizer> device;

    if (LoadPortFile("port.txt", state.port)) {
        copyPortToBuffer(state);
        logMessage("Loaded port.txt into GUI: " + state.port);
    } else {
        state.port = "COM4";
        copyPortToBuffer(state);
        logMessage("port.txt not found. Defaulting to COM4.");
    }

    double last_sim_time = glfwGetTime();
    double last_poll_time = glfwGetTime();
    const double sim_interval = 0.02;
    const double poll_interval = 0.002;

    double t0 = glfwGetTime();

    // Fullscreen toggle state
    bool is_fullscreen = true;
    WindowRestoreInfo restore{};
    // Started fullscreen, but store a sensible default windowed size for later
    restore.x = 100; restore.y = 100; restore.w = 1360; restore.h = 780; restore.valid = true;

    bool f11_was_down = false;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // F11 toggle fullscreen/windowed
        const bool f11_down = glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS;
        if (f11_down && !f11_was_down) {
            is_fullscreen = !is_fullscreen;
            SetFullscreen(window, is_fullscreen, restore);
            status_line = is_fullscreen ? "Switched to fullscreen (F11 toggles)." : "Switched to windowed (F11 toggles).";
            logMessage(status_line);
        }
        f11_was_down = f11_down;

        // ----------------------------
        // Data acquisition section
        // ----------------------------
        double now = glfwGetTime();

        if (state.simulate && measuring) {
            while (now - last_sim_time >= sim_interval) {
                CNTDigitizer::Packet packet{};
                packet.timestamp = static_cast<uint32_t>((last_sim_time - t0) * 1000.0);

                for (size_t ch = 0; ch < std::size(packet.currents); ++ch) {
                    double phase = (last_sim_time - t0) * (0.9 + 0.08 * ch);
                    packet.currents[ch] = static_cast<int32_t>(std::sin(phase) * 2500.0 + 250.0 * ch);
                }

                AppendPacket(state, packet, static_cast<float>(last_sim_time - t0));
                last_sim_time += sim_interval;
            }
        } else if (state.connected && measuring && device) {
            if (now - last_poll_time >= poll_interval) {
                last_poll_time = now;

                for (int i = 0; i < 8; ++i) {
                    CNTDigitizer::Packet packet{};
                    if (!device->getPacket(packet)) break;

                    const float t_sec = static_cast<float>((now - t0));
                    //const float t_sec = static_cast<float>(packet.timestamp) / 1000.0f;
                    AppendPacket(state, packet, t_sec);
                    now = glfwGetTime();
                }
            }
        }

        // ----------------------------
        // UI frame
        // ----------------------------
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 30/70 split always fills the GLFW window
        ImVec2 display = ImGui::GetIO().DisplaySize;
        const float left_width = std::max(360.0f, display.x * 0.30f);
        const float right_width = std::max(0.0f, display.x - left_width);

        ImGuiWindowFlags panel_flags =
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse;

        // ===== Left panel: Controls =====
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(left_width, display.y), ImGuiCond_Always);
        ImGui::Begin("CNT Digitizer Controls", nullptr, panel_flags);

        ImGui::Text("View");
        ImGui::Separator();
        ImGui::Text("F11: Toggle Fullscreen/Windowed");
        DrawStatusBadge(is_fullscreen, "FULLSCREEN", "WINDOWED");

        ImGui::Spacing();
        ImGui::Text("Connection");
        ImGui::Separator();

        ImGui::InputText("Port", state.port_buffer.data(), state.port_buffer.size());

        if (ImGui::Button("Save Port")) {
            state.port = state.port_buffer.data();
            if (SavePortFile("port.txt", state.port)) {
                status_line = "Saved port.txt: " + state.port;
                logMessage(status_line);
            } else {
                last_error = "Failed to save port.txt";
                logMessage("Error: " + last_error);
            }
        }

        if (ImGui::Button("Save Screenshot (BMP)")) {
            request_screenshot = true;
            screenshot_name = std::string("screenshot_") + nowTimestampString() + ".bmp";
            status_line = "Screenshot queued: output/" + screenshot_name;
            logMessage(status_line);
        }

        if (!state.connected) {
            if (ImGui::Button("Connect")) {
                state.port = state.port_buffer.data();
                last_error.clear();

                try {
                    device = std::make_unique<CNTDigitizer>(state.port);
                    if (device->connect()) {
                        state.connected = true;
                        status_line = "Connected to " + state.port;
                        logMessage(status_line);
                    } else {
                        device.reset();
                        state.connected = false;
                        last_error = "Connect failed (openDevice returned false)";
                        logMessage("Error: " + last_error);
                    }
                } catch (const std::exception& e) {
                    device.reset();
                    state.connected = false;
                    last_error = std::string("Connect exception: ") + e.what();
                    logMessage("Error: " + last_error);
                }
            }
        } else {
            if (ImGui::Button("Disconnect")) {
                if (measuring && device) {
                    device->enterIdleMode();
                }
                measuring = false;

                if (device) device->disconnect();
                device.reset();
                state.connected = false;

                status_line = "Disconnected.";
                logMessage(status_line);
            }
        }

        ImGui::SameLine();
        DrawStatusBadge(state.connected, "CONNECTED", "DISCONNECTED");

        ImGui::Spacing();
        ImGui::Checkbox("Simulate data (no device)", &state.simulate);

        const bool can_control_measurement = (state.connected && device) || state.simulate;

        ImGui::Spacing();
        ImGui::Text("Measurement");
        ImGui::Separator();

        ImGui::BeginDisabled(!can_control_measurement);
        if (!measuring) {
            if (ImGui::Button("Start Measurement")) {
                last_error.clear();

                if (auto_clear_on_start) {
                    ClearData(state);
                }

                t0 = glfwGetTime();
                last_sim_time = t0;
                last_poll_time = t0;

                bool ok = true;
                if (!state.simulate && device) {
                    ok = device->enterMeasurementMode();
                }

                if (ok) {
                    measuring = true;
                    status_line = "Measurement started.";
                    logMessage(status_line);
                } else {
                    measuring = false;
                    last_error = "Failed to enter measurement mode.";
                    logMessage("Error: " + last_error);
                }
            }
        } else {
            if (ImGui::Button("Stop Measurement")) {
                last_error.clear();
                if (!state.simulate && device) {
                    device->enterIdleMode();
                }
                measuring = false;

                status_line = "Measurement stopped.";
                logMessage(status_line);
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        DrawStatusBadge(measuring, "RUNNING", "HALTED");

        ImGui::Spacing();
        ImGui::Checkbox("Auto-clear data on Start", &auto_clear_on_start);

        ImGui::Spacing();
        ImGui::Text("Plot / Data");
        ImGui::Separator();

        ImGui::Checkbox("Auto-scroll (last 10s)", &state.auto_scroll);
        ImGui::SliderInt("Max Points", &state.max_points, 200, 200000);
        ImGui::SliderInt("Channel", &state.selected_channel, 0, 15);

        if (ImGui::Button("Clear Data")) {
            ClearData(state);
            status_line = "Cleared plot data.";
            logMessage(status_line);
        }

        ImGui::Spacing();
        ImGui::Text("Save Data");
        ImGui::Separator();

        ImGui::Checkbox("Save all channels (CSV)", &save_all_channels);

        if (ImGui::Button("Save CSV")) {
            const std::string fname = std::string("dataset_") + nowTimestampString() + ".csv";
            if (SaveDatasetCSV(state, fname, save_all_channels)) {
                status_line = "Saved output/" + fname;
                logMessage(status_line);
            } else {
                last_error = "Failed to write CSV to output/.";
                logMessage("Error: " + last_error);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Samples: %d", static_cast<int>(state.times.size()));
        ImGui::TextWrapped("Status: %s", status_line.c_str());

        if (!last_error.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.95f, 0.25f, 0.25f, 1.0f), "Last Error: %s", last_error.c_str());
        }

        ImGui::End();

        // ===== Right panel: Plot =====
        ImGui::SetNextWindowPos(ImVec2(left_width, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(right_width, display.y), ImGuiCond_Always);
        ImGui::Begin("Plot", nullptr, panel_flags);

        if (ImPlot::BeginPlot("Currents", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("Time (s)", "Current (a.u.)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);

            if (!state.times.empty()) {
                const auto& series = state.channels[state.selected_channel];

                if (state.auto_scroll) {
                    float max_t = state.times.back();
                    float min_t = max_t - 10.0f;
                    if (min_t < 0.0f) min_t = 0.0f;
                    ImPlot::SetupAxisLimits(ImAxis_X1, min_t, max_t, ImGuiCond_Always);
                }

                ImPlot::PlotLine("Current",
                                 state.times.data(),
                                 series.data(),
                                 static_cast<int>(std::min(state.times.size(), series.size())));
            }

            ImPlot::EndPlot();
        }

        ImGui::End();

        // ----------------------------
        // Render
        // ----------------------------
        ImGui::Render();
        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (request_screenshot) {
            request_screenshot = false;
            glFinish();

            if (SaveFramebufferBMP(screenshot_name, display_w, display_h)) {
                status_line = "Saved output/" + screenshot_name;
                logMessage(status_line);
            } else {
                last_error = "Failed to save screenshot.";
                logMessage("Error: " + last_error);
            }
        }

        glfwSwapBuffers(window);
    }

    // Cleanup
    if (device) {
        if (measuring) {
            device->enterIdleMode();
        }
        device->disconnect();
        device.reset();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}