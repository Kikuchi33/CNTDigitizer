// GUIMain.cpp  v2.0
// ImGui + ImPlot + GLFW + OpenGL3
// Changes from v1.4:
//   - Transport abstraction: Serial or UDP (AirLift WiFi)
//   - Ring buffers: O(1) append, no front-erase stutter
//   - Session system: named runs, auto-CSV on stop
//   - Multi-channel overlay plot with per-channel toggle
//   - EMA smoothing toggle + alpha slider
//   - Bias/Gate voltage UI controls (sent to device)
//   - Improved ImGui theme
//   - Thread-safe logger
//   - Device timestamp used for graph

#include "GUIMain.hpp"

#include "imgui.h"
#include "implot.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "CNTDigitizer.hpp"
#include "SerialTransport.h"
#include "UDPTransport.h"
#include "Logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
static void ApplyCNTTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 8.f;
    s.FrameRounding     = 5.f;
    s.GrabRounding      = 4.f;
    s.PopupRounding     = 6.f;
    s.ScrollbarRounding = 6.f;
    s.TabRounding       = 4.f;
    s.FramePadding      = {8, 5};
    s.ItemSpacing       = {8, 6};
    s.WindowPadding     = {14, 12};
    s.IndentSpacing     = 16.f;
    s.ScrollbarSize     = 12.f;
    s.GrabMinSize       = 10.f;
    s.WindowBorderSize  = 0.f;
    s.FrameBorderSize   = 0.5f;

    ImVec4* c = s.Colors;
    // Dark base
    c[ImGuiCol_WindowBg]          = {0.10f, 0.10f, 0.13f, 1.f};
    c[ImGuiCol_ChildBg]           = {0.12f, 0.12f, 0.15f, 1.f};
    c[ImGuiCol_PopupBg]           = {0.13f, 0.13f, 0.17f, 0.97f};
    c[ImGuiCol_Border]            = {0.28f, 0.28f, 0.34f, 0.60f};
    // Header / title bar
    c[ImGuiCol_TitleBg]           = {0.08f, 0.08f, 0.10f, 1.f};
    c[ImGuiCol_TitleBgActive]     = {0.12f, 0.20f, 0.35f, 1.f};
    // Frame (inputs, sliders)
    c[ImGuiCol_FrameBg]           = {0.16f, 0.16f, 0.20f, 1.f};
    c[ImGuiCol_FrameBgHovered]    = {0.22f, 0.22f, 0.28f, 1.f};
    c[ImGuiCol_FrameBgActive]     = {0.19f, 0.30f, 0.48f, 1.f};
    // Buttons
    c[ImGuiCol_Button]            = {0.20f, 0.32f, 0.52f, 1.f};
    c[ImGuiCol_ButtonHovered]     = {0.28f, 0.42f, 0.65f, 1.f};
    c[ImGuiCol_ButtonActive]      = {0.16f, 0.25f, 0.45f, 1.f};
    // Sliders / checkboxes
    c[ImGuiCol_SliderGrab]        = {0.40f, 0.60f, 0.90f, 1.f};
    c[ImGuiCol_SliderGrabActive]  = {0.55f, 0.75f, 1.00f, 1.f};
    c[ImGuiCol_CheckMark]         = {0.40f, 0.80f, 0.55f, 1.f};
    // Separators / scrollbar
    c[ImGuiCol_Separator]         = {0.28f, 0.28f, 0.34f, 0.80f};
    c[ImGuiCol_ScrollbarBg]       = {0.08f, 0.08f, 0.10f, 1.f};
    c[ImGuiCol_ScrollbarGrab]     = {0.30f, 0.30f, 0.38f, 1.f};
    c[ImGuiCol_Tab]               = {0.15f, 0.15f, 0.20f, 1.f};
    c[ImGuiCol_TabHovered]        = {0.28f, 0.42f, 0.65f, 1.f};
    c[ImGuiCol_TabActive]         = {0.20f, 0.32f, 0.52f, 1.f};
    // Text
    c[ImGuiCol_Text]              = {0.92f, 0.92f, 0.95f, 1.f};
    c[ImGuiCol_TextDisabled]      = {0.45f, 0.45f, 0.52f, 1.f};
    // Header (collapsible, selectable)
    c[ImGuiCol_Header]            = {0.20f, 0.32f, 0.52f, 0.55f};
    c[ImGuiCol_HeaderHovered]     = {0.28f, 0.42f, 0.65f, 0.80f};
    c[ImGuiCol_HeaderActive]      = {0.20f, 0.32f, 0.52f, 1.f};
}

// ---------------------------------------------------------------------------
// Screenshot helper
// ---------------------------------------------------------------------------
static bool SaveFramebufferBMP(const std::string& filename, int w, int h) {
    std::filesystem::create_directories("output");
    std::filesystem::path path = std::filesystem::path("output") / filename;

    std::vector<unsigned char> pixels((size_t)w * (size_t)h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    // BMP header (safe for large framebuffers)
    int64_t row_stride = (int64_t)w * 3;
    int64_t row_padded = (row_stride + 3) & ~3;
    int64_t data_size  = row_padded * h;
    int64_t file_size  = 54 + data_size;

    unsigned char hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    for (int i = 0; i < 4; ++i) hdr[2+i] = (unsigned char)(file_size >> (i*8));
    hdr[10] = 54; hdr[14] = 40;
    for (int i = 0; i < 4; ++i) hdr[18+i] = (unsigned char)(w >> (i*8));
    for (int i = 0; i < 4; ++i) hdr[22+i] = (unsigned char)(h >> (i*8));
    hdr[26] = 1; hdr[28] = 24;

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write((char*)hdr, 54);

    std::vector<unsigned char> row((size_t)row_padded, 0);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = pixels.data() + (size_t)y * (size_t)row_stride;
        for (int x = 0; x < w; ++x) {
            row[x*3+0] = src[x*3+2];
            row[x*3+1] = src[x*3+1];
            row[x*3+2] = src[x*3+0];
        }
        out.write((char*)row.data(), row_padded);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void trimRight(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.pop_back();
}

static void copyPortToBuffer(AppState& st) {
    st.port_buffer.fill('\0');
    size_t n = std::min(st.port.size(), st.port_buffer.size() - 1);
    std::copy_n(st.port.begin(), n, st.port_buffer.begin());
}

static bool LoadPortFile(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::getline(f, out);
    trimRight(out);
    return !out.empty();
}

static bool SavePortFile(const std::string& path, const std::string& port) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << port << '\n';
    return true;
}

static std::string nowTimestampString() {
    using namespace std::chrono;
    auto tt = system_clock::to_time_t(system_clock::now());
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

static bool SaveSessionCSV(const AppState& st, const std::string& filename, bool all_channels) {
    std::filesystem::create_directories("output");
    std::ofstream out(std::filesystem::path("output") / filename, std::ios::trunc);
    if (!out) return false;

    // Write session metadata as comments
    out << "# session=" << st.session.name << "\n";
    out << "# bias_mv=" << st.session.bias_mv
        << " gate_mv=" << st.session.gate_mv
        << " bias_sample_delay_ms=" << st.session.bias_sample_delay_ms
        << " measurement_delay_ms=" << st.session.measurement_delay_ms << "\n";

    // Header
    out << "t_sec";
    if (all_channels) {
        for (int ch = 0; ch < (int)CNTDigitizer::kNumChannels; ++ch)
            out << ",ch" << ch << "_nA";
    } else {
        out << ",ch" << st.selected_channel << "_nA";
    }
    out << "\n";

    const auto& times = st.times_plot;
    const size_t n = times.size();
    for (size_t i = 0; i < n; ++i) {
        out << std::fixed << std::setprecision(6) << times[i];
        if (all_channels) {
            for (int ch = 0; ch < (int)CNTDigitizer::kNumChannels; ++ch) {
                if (i < st.ch_plot[ch].size()) out << "," << st.ch_plot[ch][i];
                else out << ",";
            }
        } else {
            int ch = st.selected_channel;
            if (ch >= 0 && ch < (int)CNTDigitizer::kNumChannels && i < st.ch_plot[ch].size())
                out << "," << st.ch_plot[ch][i];
            else
                out << ",";
        }
        out << "\n";
    }
    return true;
}

static void AppendPacket(AppState& st, const CNTDigitizer::Packet& pkt, float t_sec) {
    st.times.push(t_sec);
    for (size_t ch = 0; ch < CNTDigitizer::kNumChannels; ++ch) {
        float raw = (float)pkt.currents[ch];
        float val = raw;
        if (st.ema_enabled) {
            st.ema_state[ch] = st.ema_alpha * raw + (1.f - st.ema_alpha) * st.ema_state[ch];
            val = st.ema_state[ch];
        }
        st.channels[ch].push(val);
    }
}

static void DrawStatusBadge(bool ok, const char* ok_text, const char* bad_text) {
    ImVec4 col = ok ? ImVec4(0.20f, 0.85f, 0.40f, 1.f) : ImVec4(0.95f, 0.30f, 0.30f, 1.f);
    ImGui::TextColored(col, "%s", ok ? ok_text : bad_text);
}

// Per-channel plot colours (ImPlot default cycling is fine; we define a small set for overlays)
static const ImVec4 kChannelColors[16] = {
    {0.40f,0.70f,1.00f,1.f}, {0.40f,1.00f,0.60f,1.f}, {1.00f,0.70f,0.30f,1.f}, {1.00f,0.40f,0.60f,1.f},
    {0.70f,0.40f,1.00f,1.f}, {0.40f,0.90f,0.90f,1.f}, {1.00f,0.90f,0.30f,1.f}, {0.80f,0.50f,0.30f,1.f},
    {0.55f,0.80f,0.50f,1.f}, {0.90f,0.55f,0.80f,1.f}, {0.50f,0.60f,0.90f,1.f}, {0.90f,0.80f,0.50f,1.f},
    {0.60f,0.90f,0.70f,1.f}, {0.80f,0.60f,0.40f,1.f}, {0.50f,0.70f,0.80f,1.f}, {0.95f,0.60f,0.50f,1.f},
};

// ---------------------------------------------------------------------------
// PacketQueue
// ---------------------------------------------------------------------------
struct PacketQueue {
    std::mutex m;
    std::deque<CNTDigitizer::Packet> q;
    static constexpr size_t kMax = 5000;

    void push(const CNTDigitizer::Packet& p) {
        std::lock_guard<std::mutex> lk(m);
        if (q.size() >= kMax) q.pop_front();
        q.push_back(p);
    }
    bool try_pop(CNTDigitizer::Packet& out) {
        std::lock_guard<std::mutex> lk(m);
        if (q.empty()) return false;
        out = q.front(); q.pop_front();
        return true;
    }
    void clear() { std::lock_guard<std::mutex> lk(m); q.clear(); }
};

// ---------------------------------------------------------------------------
// Fullscreen toggle
// ---------------------------------------------------------------------------
struct WindowRestoreInfo {
    int x=100, y=100, w=1360, h=780;
    bool valid = false;
};
static void SetFullscreen(GLFWwindow* win, bool fs, WindowRestoreInfo& ri) {
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(mon);
    if (fs) {
        glfwGetWindowPos(win, &ri.x, &ri.y);
        glfwGetWindowSize(win, &ri.w, &ri.h);
        ri.valid = true;
        glfwSetWindowMonitor(win, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        if (!ri.valid) { ri.x=100; ri.y=100; ri.w=1360; ri.h=780; }
        glfwSetWindowMonitor(win, nullptr, ri.x, ri.y, ri.w, ri.h, 0);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWmonitor* mon  = glfwGetPrimaryMonitor();
    const GLFWvidmode* vm = glfwGetVideoMode(mon);
    GLFWwindow* window = glfwCreateWindow(vm->width, vm->height, "CNT Digitizer", mon, nullptr);
    if (!window) { glfwTerminate(); return 1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = 1.8f;

    ApplyCNTTheme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // -----------------------------------------------------------------------
    // App state
    // -----------------------------------------------------------------------
    AppState state;
    state.resize(state.max_points);

    // Load saved port
    if (LoadPortFile("port.txt", state.port)) {
        copyPortToBuffer(state);
        logMessage("Loaded port.txt: " + state.port);
    } else {
        state.port = "COM4";
        copyPortToBuffer(state);
        logMessage("port.txt not found. Defaulting to COM4.");
    }
    // Default WiFi target IP
    const char* default_ip = "192.168.4.1";
    std::copy_n(default_ip, strlen(default_ip) + 1, state.wifi_teensy_ip.begin());

    bool measuring          = false;
    bool save_all_channels  = true;
    bool auto_clear_on_start = false;
    bool request_screenshot = false;
    std::string screenshot_name;
    std::string status_line = "Idle.";
    std::string last_error;

    // Channel visibility toggles for overlay mode
    std::array<bool, CNTDigitizer::kNumChannels> ch_visible;
    ch_visible.fill(true);

    PacketQueue pkt_queue;
    std::thread reader_thread;
    std::atomic<bool> reader_running{false};
    std::atomic<bool> reader_should_stop{false};

    std::unique_ptr<CNTDigitizer> device;

    double t0 = glfwGetTime();
    double last_sim_time = t0;
    uint32_t device_timestamp_origin = 0;  // set on first real packet; makes plot start at t=0

    bool is_fullscreen  = true;
    bool f11_was_down   = false;
    WindowRestoreInfo restore{100,100,1360,780,true};

    // -----------------------------------------------------------------------
    // Reader thread helpers
    // -----------------------------------------------------------------------
    auto start_reader = [&](CNTDigitizer* dev) {
        if (!dev || reader_running.load()) return;
        pkt_queue.clear();
        reader_should_stop.store(false);
        reader_running.store(true);
        CNTDigitizer* ptr = dev;
        reader_thread = std::thread([&, ptr]() {
            while (!reader_should_stop.load()) {
                CNTDigitizer::Packet p{};
                if (ptr->getPacket(p)) pkt_queue.push(p);
                else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            reader_running.store(false);
        });
    };

    auto stop_reader = [&]() {
        if (!reader_running.load()) return;
        reader_should_stop.store(true);
        if (reader_thread.joinable()) reader_thread.join();
        reader_running.store(false);
    };

    // -----------------------------------------------------------------------
    // Main loop
    // -----------------------------------------------------------------------
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // F11 fullscreen toggle
        bool f11 = glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS;
        if (f11 && !f11_was_down) {
            is_fullscreen = !is_fullscreen;
            SetFullscreen(window, is_fullscreen, restore);
            status_line = is_fullscreen ? "Fullscreen (F11 toggles)." : "Windowed (F11 toggles).";
        }
        f11_was_down = f11;

        double now = glfwGetTime();

        // ------------------------------------------------------------------
        // Data ingestion
        // ------------------------------------------------------------------
        if (state.simulate && measuring) {
            const double sim_interval = 0.02;
            while (now - last_sim_time >= sim_interval) {
                CNTDigitizer::Packet pkt{};
                pkt.timestamp = (uint32_t)((last_sim_time - t0) * 1000.0);
                for (size_t ch = 0; ch < CNTDigitizer::kNumChannels; ++ch) {
                    double ph = (last_sim_time - t0) * (0.9 + 0.08 * ch);
                    pkt.currents[ch] = (int32_t)(std::sin(ph) * 2500.0 + 250.0 * ch);
                }
                AppendPacket(state, pkt, (float)(last_sim_time - t0));
                last_sim_time += sim_interval;
            }
        } else if (state.connected && measuring && device) {
            for (int i = 0; i < 256; ++i) {
                CNTDigitizer::Packet pkt{};
                if (!pkt_queue.try_pop(pkt)) break;
                // Subtract the first packet timestamp so the plot always starts
                // at t=0 relative to Start, not the board's uptime since boot.
                if (device_timestamp_origin == 0)
                    device_timestamp_origin = pkt.timestamp;
                float t_sec = (float)(pkt.timestamp - device_timestamp_origin) / 1000.0f;
                AppendPacket(state, pkt, t_sec);
            }
        }

        // Linearize ring buffers to contiguous vectors for ImPlot (once per frame)
        state.refreshPlotBuffers();

        // ------------------------------------------------------------------
        // ImGui frame
        // ------------------------------------------------------------------
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImVec2 display  = io.DisplaySize;
        const float lw  = std::max(380.f, display.x * 0.28f);
        const float rw  = display.x - lw;

        ImGuiWindowFlags panel_flags = ImGuiWindowFlags_NoMove
                                     | ImGuiWindowFlags_NoResize
                                     | ImGuiWindowFlags_NoCollapse;

        // ==================================================================
        // LEFT PANEL – Controls
        // ==================================================================
        ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({lw, display.y}, ImGuiCond_Always);
        ImGui::Begin("CNT Digitizer", nullptr, panel_flags);

        // ---- View ----
        ImGui::SeparatorText("View");
        ImGui::Text("F11: Toggle Fullscreen");
        ImGui::SameLine(); DrawStatusBadge(is_fullscreen, "FULLSCREEN", "WINDOWED");

        if (ImGui::Button("Screenshot (BMP)")) {
            request_screenshot = true;
            screenshot_name = "screenshot_" + nowTimestampString() + ".bmp";
            status_line = "Screenshot queued: output/" + screenshot_name;
        }

        // ---- Connection ----
        ImGui::Spacing();
        ImGui::SeparatorText("Connection");

        ImGui::Checkbox("Use WiFi (AirLift UDP)", &state.use_wifi);

        if (!state.use_wifi) {
            ImGui::InputText("Serial Port", state.port_buffer.data(), state.port_buffer.size());
            if (ImGui::Button("Save Port")) {
                state.port = state.port_buffer.data();
                SavePortFile("port.txt", state.port);
                status_line = "Saved port: " + state.port;
            }
        } else {
            ImGui::InputText("Teensy IP", state.wifi_teensy_ip.data(), state.wifi_teensy_ip.size());
            int lp = (int)state.wifi_listen_port;
            int tp = (int)state.wifi_teensy_port;
            if (ImGui::InputInt("Listen Port", &lp)) state.wifi_listen_port = (uint16_t)std::clamp(lp, 1, 65535);
            if (ImGui::InputInt("Teensy Port", &tp)) state.wifi_teensy_port = (uint16_t)std::clamp(tp, 1, 65535);
            ImGui::TextDisabled("Board must be running with CNT_ENABLE_WIFI 1");
        }
        ImGui::Spacing();

        if (!state.connected) {
            if (ImGui::Button("Connect")) {
                last_error.clear();
                state.port = state.port_buffer.data();
                try {
                    std::unique_ptr<ITransport> transport;
                    if (state.use_wifi) {
                        transport = std::make_unique<UDPTransport>(
                            state.wifi_listen_port,
                            std::string(state.wifi_teensy_ip.data()),
                            state.wifi_teensy_port);
                    } else {
                        transport = std::make_unique<SerialTransport>(state.port, 115200);
                    }
                    device = std::make_unique<CNTDigitizer>(std::move(transport));
                    if (device->connect()) {
                        state.connected = true;
                        status_line = state.use_wifi
                            ? "Connected via WiFi to " + std::string(state.wifi_teensy_ip.data())
                            : "Connected via Serial to " + state.port;
                        logMessage(status_line);
                    } else {
                        device.reset();
                        last_error = "Connect failed. Check port/IP.";
                        logMessage("Error: " + last_error);
                    }
                } catch (const std::exception& e) {
                    device.reset();
                    last_error = std::string("Connect exception: ") + e.what();
                    logMessage("Error: " + last_error);
                }
            }
        } else {
            if (ImGui::Button("Disconnect")) {
                stop_reader();
                if (measuring && device) device->enterIdleMode();
                measuring = false;
                if (device) device->disconnect();
                device.reset();
                state.connected = false;
                status_line = "Disconnected.";
                logMessage(status_line);
            }
        }
        ImGui::SameLine(); DrawStatusBadge(state.connected, "CONNECTED", "DISCONNECTED");

        // ---- Device Parameters ----
        ImGui::Spacing();
        ImGui::SeparatorText("Device Parameters");
        ImGui::BeginDisabled(!state.connected || !device);

        bool params_changed = false;
        params_changed |= ImGui::SliderInt("Bias (mV)",  &state.session.bias_mv,  -5000, 5000);
        params_changed |= ImGui::SliderInt("Gate (mV)",  &state.session.gate_mv,  -5000, 5000);

        int bsd = (int)state.session.bias_sample_delay_ms;
        int md  = (int)state.session.measurement_delay_ms;
        if (ImGui::SliderInt("Bias Sample Delay (ms)", &bsd, 10, 5000))  { state.session.bias_sample_delay_ms  = (uint32_t)bsd; params_changed = true; }
        if (ImGui::SliderInt("Measurement Delay (ms)", &md,  10, 2000))  { state.session.measurement_delay_ms  = (uint32_t)md;  params_changed = true; }

        if (params_changed && device) {
            device->setBiasVoltage(state.session.bias_mv);
            device->setGateVoltage(state.session.gate_mv);
            device->setBiasSampleDelay(state.session.bias_sample_delay_ms);
            device->setMeasurementDelay(state.session.measurement_delay_ms);
        }
        ImGui::EndDisabled();

        // ---- Measurement ----
        ImGui::Spacing();
        ImGui::SeparatorText("Measurement");
        ImGui::Checkbox("Simulate data", &state.simulate);

        const bool can_measure = (state.connected && device) || state.simulate;
        ImGui::BeginDisabled(!can_measure);

        if (!measuring) {
            if (ImGui::Button("Start")) {
                last_error.clear();
                if (auto_clear_on_start) state.clear();

                state.session.name       = "run_" + nowTimestampString();
                state.session.start_wall = glfwGetTime();
                t0                       = state.session.start_wall;
                last_sim_time            = t0;
                device_timestamp_origin  = 0;  // reset so next run starts at t=0

                bool ok = true;
                if (!state.simulate && device) {
                    // Push current params before starting
                    device->setBiasVoltage(state.session.bias_mv);
                    device->setGateVoltage(state.session.gate_mv);
                    device->setBiasSampleDelay(state.session.bias_sample_delay_ms);
                    device->setMeasurementDelay(state.session.measurement_delay_ms);
                    ok = device->enterMeasurementMode();
                }
                if (ok) {
                    measuring = true;
                    if (!state.simulate && device) start_reader(device.get());
                    status_line = "Measuring: " + state.session.name;
                    logMessage(status_line);
                } else {
                    last_error = "Failed to enter measurement mode.";
                    logMessage("Error: " + last_error);
                }
            }
        } else {
            if (ImGui::Button("Stop")) {
                stop_reader();
                if (!state.simulate && device) device->enterIdleMode();
                measuring = false;

                // Auto-save session CSV
                state.refreshPlotBuffers();
                std::string csv_name = state.session.name + ".csv";
                if (SaveSessionCSV(state, csv_name, true)) {
                    status_line = "Saved output/" + csv_name;
                    logMessage(status_line);
                } else {
                    last_error = "Auto-save CSV failed.";
                    logMessage("Error: " + last_error);
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); DrawStatusBadge(measuring, "RUNNING", "HALTED");
        ImGui::Spacing();
        ImGui::Checkbox("Auto-clear on Start", &auto_clear_on_start);

        // ---- Plot Controls ----
        ImGui::Spacing();
        ImGui::SeparatorText("Plot");

        ImGui::Checkbox("Auto-scroll (10s)", &state.auto_scroll);
        ImGui::Checkbox("Overlay all 16 channels", &state.show_all_ch);

        if (!state.show_all_ch) {
            ImGui::SliderInt("Channel", &state.selected_channel, 0, 15);
        } else {
            // Small toggles per channel in a grid
            ImGui::Text("Visible channels:");
            for (int ch = 0; ch < 16; ++ch) {
                char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", ch);
                ImGui::PushStyleColor(ImGuiCol_CheckMark, kChannelColors[ch]);
                ImGui::Checkbox(lbl, &ch_visible[ch]);
                ImGui::PopStyleColor();
                if (ch % 8 != 7) ImGui::SameLine();
            }
        }

        ImGui::Spacing();
        ImGui::Checkbox("EMA Smoothing", &state.ema_enabled);
        if (state.ema_enabled) {
            ImGui::SliderFloat("Alpha", &state.ema_alpha, 0.01f, 0.99f, "%.2f");
            ImGui::TextDisabled("Low alpha = smoother. High = raw.");
        }

        int mp = state.max_points;
        if (ImGui::InputInt("Max Points", &mp)) {
            mp = std::clamp(mp, 200, 200000);
            if (mp != state.max_points) state.resize(mp);
        }
        ImGui::TextDisabled("Range: 200 – 200000");

        if (ImGui::Button("Clear Data")) {
            state.clear();
            status_line = "Data cleared.";
        }

        // ---- Save ----
        ImGui::Spacing();
        ImGui::SeparatorText("Save");
        ImGui::Checkbox("All channels in CSV", &save_all_channels);
        if (ImGui::Button("Save CSV")) {
            state.refreshPlotBuffers();
            std::string fname = "dataset_" + nowTimestampString() + ".csv";
            if (SaveSessionCSV(state, fname, save_all_channels)) {
                status_line = "Saved output/" + fname;
                logMessage(status_line);
            } else {
                last_error = "CSV save failed.";
                logMessage("Error: " + last_error);
            }
        }

        // ---- Status ----
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Samples: %d", (int)state.times_plot.size());
        if (measuring) ImGui::Text("Session: %s", state.session.name.c_str());
        ImGui::TextWrapped("Status: %s", status_line.c_str());
        if (!last_error.empty())
            ImGui::TextColored({0.95f,0.30f,0.30f,1.f}, "Error: %s", last_error.c_str());

        ImGui::End();

        // ==================================================================
        // RIGHT PANEL – Plot
        // ==================================================================
        ImGui::SetNextWindowPos({lw, 0}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({rw, display.y}, ImGuiCond_Always);
        ImGui::Begin("Plot", nullptr, panel_flags);

        if (ImPlot::BeginPlot("##currents", {-1, -1})) {
            ImPlot::SetupAxes("Time (s)", "Current (nA)",
                              ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);

            if (!state.times_plot.empty()) {
                if (state.auto_scroll) {
                    float tmax = state.times_plot.back();
                    float tmin = std::max(0.f, tmax - 10.f);
                    ImPlot::SetupAxisLimits(ImAxis_X1, tmin, tmax, ImGuiCond_Always);
                }

                if (state.show_all_ch) {
                    for (int ch = 0; ch < (int)CNTDigitizer::kNumChannels; ++ch) {
                        if (!ch_visible[ch]) continue;
                        const auto& series = state.ch_plot[ch];
                        int n = (int)std::min(state.times_plot.size(), series.size());
                        if (n == 0) continue;
                        char lbl[16]; snprintf(lbl, sizeof(lbl), "Ch%d", ch);
                        ImPlot::SetNextLineStyle(kChannelColors[ch]);
                        ImPlot::PlotLine(lbl, state.times_plot.data(), series.data(), n);
                    }
                } else {
                    int ch = state.selected_channel;
                    const auto& series = state.ch_plot[ch];
                    int n = (int)std::min(state.times_plot.size(), series.size());
                    if (n > 0) {
                        char lbl[16]; snprintf(lbl, sizeof(lbl), "Ch%d (nA)", ch);
                        ImPlot::SetNextLineStyle(kChannelColors[ch]);
                        ImPlot::PlotLine(lbl, state.times_plot.data(), series.data(), n);
                    }
                }
            }

            ImPlot::EndPlot();
        }

        ImGui::End();

        // ------------------------------------------------------------------
        // Render
        // ------------------------------------------------------------------
        ImGui::Render();
        int fw = 0, fh = 0;
        glfwGetFramebufferSize(window, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.09f, 0.09f, 0.11f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (request_screenshot) {
            request_screenshot = false;
            glFinish();
            if (SaveFramebufferBMP(screenshot_name, fw, fh))
                status_line = "Screenshot saved: output/" + screenshot_name;
            else
                last_error = "Screenshot failed.";
        }

        glfwSwapBuffers(window);
    }

    // ------------------------------------------------------------------
    // Cleanup
    // ------------------------------------------------------------------
    stop_reader();
    if (device) {
        if (measuring) device->enterIdleMode();
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
