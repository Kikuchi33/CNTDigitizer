// GUIMain.cpp  v2.2
// ImGui + ImPlot + GLFW + OpenGL3
// Changes from v2.0:
//   - PNG export of plot region only (stb_image_write, no external dep)
//   - CSV overlay: load old runs as ghost traces on the live plot
//   - Adjustable auto-scroll window (replaces hardcoded 10 s)
//   - Crosshair cursor with timestamp + current readout tooltip
//   - Device parameter inputs changed from sliders to InputInt + Send button
//   - Session notes field saved into CSV header
//   - config.ini: persistent preferences (port, params, window state, plot options)

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
// stb_image_write – header-only PNG encoder (single-file, no extra dep)
// Place stb_image_write.h in external/stb/ (or anywhere on the include path).
// ---------------------------------------------------------------------------
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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
    c[ImGuiCol_WindowBg]          = {0.10f, 0.10f, 0.13f, 1.f};
    c[ImGuiCol_ChildBg]           = {0.12f, 0.12f, 0.15f, 1.f};
    c[ImGuiCol_PopupBg]           = {0.13f, 0.13f, 0.17f, 0.97f};
    c[ImGuiCol_Border]            = {0.28f, 0.28f, 0.34f, 0.60f};
    c[ImGuiCol_TitleBg]           = {0.08f, 0.08f, 0.10f, 1.f};
    c[ImGuiCol_TitleBgActive]     = {0.12f, 0.20f, 0.35f, 1.f};
    c[ImGuiCol_FrameBg]           = {0.16f, 0.16f, 0.20f, 1.f};
    c[ImGuiCol_FrameBgHovered]    = {0.22f, 0.22f, 0.28f, 1.f};
    c[ImGuiCol_FrameBgActive]     = {0.19f, 0.30f, 0.48f, 1.f};
    c[ImGuiCol_Button]            = {0.20f, 0.32f, 0.52f, 1.f};
    c[ImGuiCol_ButtonHovered]     = {0.28f, 0.42f, 0.65f, 1.f};
    c[ImGuiCol_ButtonActive]      = {0.16f, 0.25f, 0.45f, 1.f};
    c[ImGuiCol_SliderGrab]        = {0.40f, 0.60f, 0.90f, 1.f};
    c[ImGuiCol_SliderGrabActive]  = {0.55f, 0.75f, 1.00f, 1.f};
    c[ImGuiCol_CheckMark]         = {0.40f, 0.80f, 0.55f, 1.f};
    c[ImGuiCol_Separator]         = {0.28f, 0.28f, 0.34f, 0.80f};
    c[ImGuiCol_ScrollbarBg]       = {0.08f, 0.08f, 0.10f, 1.f};
    c[ImGuiCol_ScrollbarGrab]     = {0.30f, 0.30f, 0.38f, 1.f};
    c[ImGuiCol_Tab]               = {0.15f, 0.15f, 0.20f, 1.f};
    c[ImGuiCol_TabHovered]        = {0.28f, 0.42f, 0.65f, 1.f};
    c[ImGuiCol_TabActive]         = {0.20f, 0.32f, 0.52f, 1.f};
    c[ImGuiCol_Text]              = {0.92f, 0.92f, 0.95f, 1.f};
    c[ImGuiCol_TextDisabled]      = {0.45f, 0.45f, 0.52f, 1.f};
    c[ImGuiCol_Header]            = {0.20f, 0.32f, 0.52f, 0.55f};
    c[ImGuiCol_HeaderHovered]     = {0.28f, 0.42f, 0.65f, 0.80f};
    c[ImGuiCol_HeaderActive]      = {0.20f, 0.32f, 0.52f, 1.f};
}

// ---------------------------------------------------------------------------
// PNG screenshot – plot region only
// plot_pos / plot_size come from ImPlot::GetPlotPos() / GetPlotSize()
// called AFTER glfwSwapBuffers so the back buffer has the rendered frame.
// We read from GL_FRONT instead.
// ---------------------------------------------------------------------------
static bool SavePlotPNG(const std::string& filename,
                        ImVec2 plot_pos, ImVec2 plot_size,
                        int fb_w, int fb_h)
{
    std::filesystem::create_directories("output");
    std::filesystem::path path = std::filesystem::path("output") / filename;

    int x  = (int)plot_pos.x;
    int y  = (int)plot_pos.y;
    int pw = (int)plot_size.x;
    int ph = (int)plot_size.y;

    // Clamp to framebuffer
    x  = std::max(0, x);
    y  = std::max(0, y);
    pw = std::min(pw, fb_w - x);
    ph = std::min(ph, fb_h - y);
    if (pw <= 0 || ph <= 0) return false;

    // OpenGL origin is bottom-left; ImGui origin is top-left.
    int gl_y = fb_h - y - ph;

    std::vector<unsigned char> pixels((size_t)pw * (size_t)ph * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_FRONT);
    glReadPixels(x, gl_y, pw, ph, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    // Flip vertically (OpenGL gives bottom-up rows)
    std::vector<unsigned char> flipped((size_t)pw * (size_t)ph * 3);
    for (int row = 0; row < ph; ++row) {
        const unsigned char* src = pixels.data()  + (size_t)(ph - 1 - row) * pw * 3;
        unsigned char*       dst = flipped.data() + (size_t)row             * pw * 3;
        std::memcpy(dst, src, (size_t)pw * 3);
    }

    // stbi_write_png: stride = pw*3
    return stbi_write_png(path.string().c_str(), pw, ph, 3, flipped.data(), pw * 3) != 0;
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

// ---------------------------------------------------------------------------
// Config – persistent preferences saved to config.ini
// Format: simple key=value, one per line. Lines starting with # are comments.
// Saved on exit and whenever connection/parameter settings change.
// ---------------------------------------------------------------------------
struct AppConfig {
    // Connection
    std::string port          = "COM4";
    bool        use_wifi      = false;
    std::string wifi_ip       = "192.168.4.1";
    int         wifi_listen   = 5005;
    int         wifi_teensy   = 5006;

    // Device parameters
    int      bias_mv     = 1000;
    int      gate_mv     = -1000;
    int      bsd_ms      = 1000;
    int      md_ms       = 100;

    // Plot
    bool     auto_scroll      = true;
    float    scroll_window    = 10.f;
    bool     show_all_ch      = false;
    bool     ema_enabled      = false;
    float    ema_alpha        = 0.2f;
    int      max_points       = 2000;
    int      selected_channel = 0;

    // Window
    bool     fullscreen       = true;
    int      win_x = 100, win_y = 100;
    int      win_w = 1360, win_h = 780;

    // UI
    bool     save_all_channels   = true;
    bool     auto_clear_on_start = false;
};

static void SaveConfig(const std::string& path, const AppConfig& cfg) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    f << "# CNT Digitizer config — edited automatically on exit\n";
    f << "# Connection\n";
    f << "port="         << cfg.port        << "\n";
    f << "use_wifi="     << cfg.use_wifi     << "\n";
    f << "wifi_ip="      << cfg.wifi_ip      << "\n";
    f << "wifi_listen="  << cfg.wifi_listen  << "\n";
    f << "wifi_teensy="  << cfg.wifi_teensy  << "\n";
    f << "# Device parameters\n";
    f << "bias_mv="      << cfg.bias_mv      << "\n";
    f << "gate_mv="      << cfg.gate_mv      << "\n";
    f << "bsd_ms="       << cfg.bsd_ms       << "\n";
    f << "md_ms="        << cfg.md_ms        << "\n";
    f << "# Plot\n";
    f << "auto_scroll="      << cfg.auto_scroll      << "\n";
    f << "scroll_window="    << cfg.scroll_window    << "\n";
    f << "show_all_ch="      << cfg.show_all_ch      << "\n";
    f << "ema_enabled="      << cfg.ema_enabled       << "\n";
    f << "ema_alpha="        << cfg.ema_alpha         << "\n";
    f << "max_points="       << cfg.max_points        << "\n";
    f << "selected_channel=" << cfg.selected_channel  << "\n";
    f << "# Window\n";
    f << "fullscreen="  << cfg.fullscreen  << "\n";
    f << "win_x="       << cfg.win_x       << "\n";
    f << "win_y="       << cfg.win_y       << "\n";
    f << "win_w="       << cfg.win_w       << "\n";
    f << "win_h="       << cfg.win_h       << "\n";
    f << "# UI\n";
    f << "save_all_channels="   << cfg.save_all_channels   << "\n";
    f << "auto_clear_on_start=" << cfg.auto_clear_on_start << "\n";
}

static AppConfig LoadConfig(const std::string& path) {
    AppConfig cfg;
    std::ifstream f(path);
    if (!f) return cfg;  // return defaults if file missing

    std::string line;
    while (std::getline(f, line)) {
        trimRight(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        trimRight(key); trimRight(val);

        try {
            if      (key == "port")               cfg.port             = val;
            else if (key == "use_wifi")            cfg.use_wifi         = (val == "1");
            else if (key == "wifi_ip")             cfg.wifi_ip          = val;
            else if (key == "wifi_listen")         cfg.wifi_listen      = std::stoi(val);
            else if (key == "wifi_teensy")         cfg.wifi_teensy      = std::stoi(val);
            else if (key == "bias_mv")             cfg.bias_mv          = std::stoi(val);
            else if (key == "gate_mv")             cfg.gate_mv          = std::stoi(val);
            else if (key == "bsd_ms")              cfg.bsd_ms           = std::stoi(val);
            else if (key == "md_ms")               cfg.md_ms            = std::stoi(val);
            else if (key == "auto_scroll")         cfg.auto_scroll      = (val == "1");
            else if (key == "scroll_window")       cfg.scroll_window    = std::stof(val);
            else if (key == "show_all_ch")         cfg.show_all_ch      = (val == "1");
            else if (key == "ema_enabled")         cfg.ema_enabled      = (val == "1");
            else if (key == "ema_alpha")           cfg.ema_alpha        = std::stof(val);
            else if (key == "max_points")          cfg.max_points       = std::stoi(val);
            else if (key == "selected_channel")    cfg.selected_channel = std::stoi(val);
            else if (key == "fullscreen")          cfg.fullscreen       = (val == "1");
            else if (key == "win_x")               cfg.win_x            = std::stoi(val);
            else if (key == "win_y")               cfg.win_y            = std::stoi(val);
            else if (key == "win_w")               cfg.win_w            = std::stoi(val);
            else if (key == "win_h")               cfg.win_h            = std::stoi(val);
            else if (key == "save_all_channels")   cfg.save_all_channels   = (val == "1");
            else if (key == "auto_clear_on_start") cfg.auto_clear_on_start = (val == "1");
        } catch (...) {}  // ignore malformed values, keep defaults
    }
    return cfg;
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

// ---------------------------------------------------------------------------
// CSV save – now includes session notes in header
// ---------------------------------------------------------------------------
static bool SaveSessionCSV(const AppState& st, const std::string& filename, bool all_channels) {
    std::filesystem::create_directories("output");
    std::ofstream out(std::filesystem::path("output") / filename, std::ios::trunc);
    if (!out) return false;

    out << "# session=" << st.session.name << "\n";
    out << "# bias_mv=" << st.session.bias_mv
        << " gate_mv=" << st.session.gate_mv
        << " bias_sample_delay_ms=" << st.session.bias_sample_delay_ms
        << " measurement_delay_ms=" << st.session.measurement_delay_ms << "\n";
    if (!st.session.notes.empty())
        out << "# notes=" << st.session.notes << "\n";

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

// ---------------------------------------------------------------------------
// CSV overlay loader
// Supports files saved by this application (header comment lines start with #,
// first data column is t_sec, remaining columns are ch0_nA .. ch15_nA or a
// subset).  Unknown column names are silently ignored.
// ---------------------------------------------------------------------------
static bool LoadCsvOverlay(const std::string& filepath, CsvOverlay& ov) {
    std::ifstream f(filepath);
    if (!f) return false;

    ov.times.clear();
    for (auto& v : ov.channels) v.clear();
    ov.meta.clear();
    ov.filename = std::filesystem::path(filepath).filename().string();

    std::string line;
    std::vector<int> col_to_ch;  // index: column index → channel (-1 = time, -2 = skip)
    bool header_parsed = false;

    while (std::getline(f, line)) {
        trimRight(line);
        if (line.empty()) continue;

        // Comment / metadata lines
        if (line[0] == '#') {
            if (!ov.meta.empty()) ov.meta += ' ';
            ov.meta += line.substr(1);
            continue;
        }

        // First non-comment line is the column header
        if (!header_parsed) {
            header_parsed = true;
            std::istringstream iss(line);
            std::string token;
            while (std::getline(iss, token, ',')) {
                trimRight(token);
                if (token == "t_sec") {
                    col_to_ch.push_back(-1);
                } else if (token.size() > 2 && token[0] == 'c' && token[1] == 'h') {
                    // "ch0_nA", "ch12_nA", etc.
                    int ch = std::stoi(token.substr(2));
                    if (ch >= 0 && ch < (int)CNTDigitizer::kNumChannels)
                        col_to_ch.push_back(ch);
                    else
                        col_to_ch.push_back(-2);
                } else {
                    col_to_ch.push_back(-2);
                }
            }
            continue;
        }

        // Data row
        std::istringstream iss(line);
        std::string token;
        int col = 0;
        float t_val = 0.f;
        bool  has_t = false;
        std::array<float, CNTDigitizer::kNumChannels> row_vals{};
        std::array<bool,  CNTDigitizer::kNumChannels> row_set{};
        row_set.fill(false);

        while (std::getline(iss, token, ',')) {
            if (col >= (int)col_to_ch.size()) break;
            int mapping = col_to_ch[col];
            if (!token.empty()) {
                float v = std::stof(token);
                if (mapping == -1) { t_val = v; has_t = true; }
                else if (mapping >= 0) { row_vals[mapping] = v; row_set[mapping] = true; }
            }
            ++col;
        }

        if (has_t) {
            ov.times.push_back(t_val);
            for (int ch = 0; ch < (int)CNTDigitizer::kNumChannels; ++ch) {
                if (row_set[ch])
                    ov.channels[ch].push_back(row_vals[ch]);
                else if (!ov.channels[ch].empty())
                    ov.channels[ch].push_back(std::numeric_limits<float>::quiet_NaN());
            }
        }
    }

    return !ov.times.empty();
}

// ---------------------------------------------------------------------------
// Packet helpers
// ---------------------------------------------------------------------------
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

// Per-channel colours
static const ImVec4 kChannelColors[16] = {
    {0.40f,0.70f,1.00f,1.f}, {0.40f,1.00f,0.60f,1.f}, {1.00f,0.70f,0.30f,1.f}, {1.00f,0.40f,0.60f,1.f},
    {0.70f,0.40f,1.00f,1.f}, {0.40f,0.90f,0.90f,1.f}, {1.00f,0.90f,0.30f,1.f}, {0.80f,0.50f,0.30f,1.f},
    {0.55f,0.80f,0.50f,1.f}, {0.90f,0.55f,0.80f,1.f}, {0.50f,0.60f,0.90f,1.f}, {0.90f,0.80f,0.50f,1.f},
    {0.60f,0.90f,0.70f,1.f}, {0.80f,0.60f,0.40f,1.f}, {0.50f,0.70f,0.80f,1.f}, {0.95f,0.60f,0.50f,1.f},
};

// Overlay colours are the same palette but drawn at reduced alpha / dashed style
static ImVec4 OverlayColor(int overlay_idx, int ch) {
    // Shift hue slightly per overlay so multiple overlays are distinguishable
    ImVec4 base = kChannelColors[ch % 16];
    float shift = 0.15f * (float)(overlay_idx % 4);
    return ImVec4(
        std::min(1.f, base.x + shift),
        std::min(1.f, base.y + shift),
        std::min(1.f, base.z + shift),
        0.55f   // semi-transparent to distinguish from live data
    );
}

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
    // Read config early to determine initial window mode
    AppConfig _pre_cfg = LoadConfig("config.ini");
    GLFWwindow* window = _pre_cfg.fullscreen
        ? glfwCreateWindow(vm->width, vm->height, "CNT Digitizer", mon, nullptr)
        : glfwCreateWindow(_pre_cfg.win_w, _pre_cfg.win_h, "CNT Digitizer", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    if (!_pre_cfg.fullscreen)
        glfwSetWindowPos(window, _pre_cfg.win_x, _pre_cfg.win_y);

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
    // Load config before building AppState so all fields are pre-populated
    AppConfig cfg = LoadConfig("config.ini");
    logMessage(cfg.port != "COM4" || true ? "Loaded config.ini" : "config.ini not found, using defaults");

    AppState state;

    // Apply config to state
    state.port             = cfg.port;
    state.use_wifi         = cfg.use_wifi;
    state.wifi_listen_port = (uint16_t)cfg.wifi_listen;
    state.wifi_teensy_port = (uint16_t)cfg.wifi_teensy;
    state.auto_scroll      = cfg.auto_scroll;
    state.scroll_window_sec = cfg.scroll_window;
    state.show_all_ch      = cfg.show_all_ch;
    state.ema_enabled      = cfg.ema_enabled;
    state.ema_alpha        = cfg.ema_alpha;
    state.selected_channel = cfg.selected_channel;
    state.session.bias_mv               = cfg.bias_mv;
    state.session.gate_mv               = cfg.gate_mv;
    state.session.bias_sample_delay_ms  = (uint32_t)cfg.bsd_ms;
    state.session.measurement_delay_ms  = (uint32_t)cfg.md_ms;

    state.resize(cfg.max_points);
    copyPortToBuffer(state);

    // Apply WiFi IP
    std::copy_n(cfg.wifi_ip.begin(),
                std::min(cfg.wifi_ip.size(), state.wifi_teensy_ip.size() - 1),
                state.wifi_teensy_ip.begin());
    state.wifi_teensy_ip[cfg.wifi_ip.size()] = '\0';

    bool measuring           = false;
    bool save_all_channels   = cfg.save_all_channels;
    bool auto_clear_on_start = cfg.auto_clear_on_start;
    std::string status_line  = "Idle.";
    std::string last_error;

    // Channel visibility toggles
    std::array<bool, CNTDigitizer::kNumChannels> ch_visible;
    ch_visible.fill(true);

    // PNG screenshot state – we capture the plot region rect from the previous
    // frame so we know exactly where to read pixels.
    bool        request_plot_png = false;
    std::string png_filename;
    ImVec2      last_plot_pos{};
    ImVec2      last_plot_size{};

    // CSV overlay file-path input buffer
    static char overlay_path_buf[512] = {};

    // Device parameter input buffers (text entry)
    // We keep local int copies and only send to device when the user clicks Send.
    int  param_bias_mv   = state.session.bias_mv;
    int  param_gate_mv   = state.session.gate_mv;
    int  param_bsd_ms    = (int)state.session.bias_sample_delay_ms;
    int  param_md_ms     = (int)state.session.measurement_delay_ms;

    // Session notes buffer
    static char notes_buf[512] = {};

    PacketQueue pkt_queue;
    std::thread reader_thread;
    std::atomic<bool> reader_running{false};
    std::atomic<bool> reader_should_stop{false};

    std::unique_ptr<CNTDigitizer> device;

    double t0 = glfwGetTime();
    double last_sim_time = t0;
    uint32_t device_timestamp_origin = 0;

    bool is_fullscreen = cfg.fullscreen;
    bool f11_was_down  = false;
    WindowRestoreInfo restore{cfg.win_x, cfg.win_y, cfg.win_w, cfg.win_h, true};

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
                if (device_timestamp_origin == 0)
                    device_timestamp_origin = pkt.timestamp;
                float t_sec = (float)(pkt.timestamp - device_timestamp_origin) / 1000.0f;
                AppendPacket(state, pkt, t_sec);
            }
        }

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
        ImGui::Begin("CNT Digitizer", nullptr,
                     panel_flags | ImGuiWindowFlags_NoTitleBar);

        // ---- View ----
        ImGui::SeparatorText("View");
        ImGui::Text("F11: Toggle Fullscreen");
        ImGui::SameLine(); DrawStatusBadge(is_fullscreen, "FULLSCREEN", "WINDOWED");

        // Plot PNG button – captures the plot canvas region only
        if (ImGui::Button("Save Plot PNG")) {
            request_plot_png = true;
            png_filename = "plot_" + nowTimestampString() + ".png";
            status_line = "Plot PNG queued: output/" + png_filename;
        }

        // ---- Connection ----
        ImGui::Spacing();
        ImGui::SeparatorText("Connection");
        ImGui::Checkbox("Use WiFi (AirLift UDP)", &state.use_wifi);

        if (!state.use_wifi) {
            ImGui::InputText("Serial Port", state.port_buffer.data(), state.port_buffer.size());
            if (ImGui::Button("Save Port")) {
                state.port = state.port_buffer.data();
                AppConfig save_cfg = cfg;
                save_cfg.port = state.port;
                SaveConfig("config.ini", save_cfg);
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
                        // Sync UI param values to session defaults
                        param_bias_mv  = state.session.bias_mv;
                        param_gate_mv  = state.session.gate_mv;
                        param_bsd_ms   = (int)state.session.bias_sample_delay_ms;
                        param_md_ms    = (int)state.session.measurement_delay_ms;
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
        // All inputs are now text/integer entry fields.
        // Changes are staged locally and sent to the device only when Send is clicked,
        // preventing accidental mid-edit transmissions.
        ImGui::Spacing();
        ImGui::SeparatorText("Device Parameters");
        ImGui::BeginDisabled(!state.connected || !device);

        ImGui::InputInt("Bias (mV)##input",  &param_bias_mv);
        ImGui::InputInt("Gate (mV)##input",  &param_gate_mv);
        ImGui::InputInt("Bias Sample Delay (ms)##input", &param_bsd_ms);
        ImGui::InputInt("Measurement Delay (ms)##input", &param_md_ms);

        // Clamp to safe ranges
        param_bias_mv  = std::clamp(param_bias_mv,  -5000, 5000);
        param_gate_mv  = std::clamp(param_gate_mv,  -5000, 5000);
        param_bsd_ms   = std::clamp(param_bsd_ms,   10,    60000);
        param_md_ms    = std::clamp(param_md_ms,    10,    2000);

        if (ImGui::Button("Send Parameters")) {
            state.session.bias_mv               = param_bias_mv;
            state.session.gate_mv               = param_gate_mv;
            state.session.bias_sample_delay_ms  = (uint32_t)param_bsd_ms;
            state.session.measurement_delay_ms  = (uint32_t)param_md_ms;
            if (device) {
                device->setBiasVoltage(state.session.bias_mv);
                device->setGateVoltage(state.session.gate_mv);
                device->setBiasSampleDelay(state.session.bias_sample_delay_ms);
                device->setMeasurementDelay(state.session.measurement_delay_ms);
            }
            status_line = "Parameters sent.";
            logMessage(status_line);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Range: Bias/Gate ±5000 mV");
        ImGui::EndDisabled();

        // ---- Measurement ----
        ImGui::Spacing();
        ImGui::SeparatorText("Measurement");
        ImGui::Checkbox("Simulate data", &state.simulate);

        // Session notes
        ImGui::InputText("Notes", notes_buf, sizeof(notes_buf));
        ImGui::TextDisabled("Saved into CSV header");

        const bool can_measure = (state.connected && device) || state.simulate;
        ImGui::BeginDisabled(!can_measure);

        if (!measuring) {
            if (ImGui::Button("Start")) {
                last_error.clear();
                if (auto_clear_on_start) state.clear();

                state.session.name       = "run_" + nowTimestampString();
                state.session.notes      = notes_buf;
                state.session.start_wall = glfwGetTime();
                t0                       = state.session.start_wall;
                last_sim_time            = t0;
                device_timestamp_origin  = 0;

                bool ok = true;
                if (!state.simulate && device) {
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

        ImGui::Checkbox("Auto-scroll", &state.auto_scroll);
        if (state.auto_scroll) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.f);
            ImGui::InputFloat("s##win", &state.scroll_window_sec, 1.f, 10.f, "%.0f s");
            state.scroll_window_sec = std::clamp(state.scroll_window_sec, 1.f, 3600.f);
        }

        ImGui::Checkbox("Overlay all 16 channels", &state.show_all_ch);
        if (!state.show_all_ch) {
            ImGui::SetNextItemWidth(120.f);
            ImGui::InputInt("Channel##sel", &state.selected_channel);
            state.selected_channel = std::clamp(state.selected_channel, 0, 15);
        } else {
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
            ImGui::InputFloat("Alpha##ema", &state.ema_alpha, 0.01f, 0.1f, "%.2f");
            state.ema_alpha = std::clamp(state.ema_alpha, 0.01f, 0.99f);
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

        // ---- CSV Overlays ----
        ImGui::Spacing();
        ImGui::SeparatorText("CSV Overlays");
        ImGui::InputText("File Path##ovpath", overlay_path_buf, sizeof(overlay_path_buf));
        ImGui::TextDisabled("Drag-drop or type a path to output/*.csv");
        if (ImGui::Button("Load CSV Overlay")) {
            std::string path = overlay_path_buf;
            trimRight(path);
            if (!path.empty()) {
                CsvOverlay ov;
                if (LoadCsvOverlay(path, ov)) {
                    state.overlays.push_back(std::move(ov));
                    status_line = "Loaded overlay: " + state.overlays.back().filename;
                    logMessage(status_line);
                    overlay_path_buf[0] = '\0';
                } else {
                    last_error = "Failed to load CSV: " + path;
                    logMessage("Error: " + last_error);
                }
            }
        }

        if (!state.overlays.empty()) {
            ImGui::Spacing();
            ImGui::Text("Loaded overlays:");
            int remove_idx = -1;
            for (int oi = 0; oi < (int)state.overlays.size(); ++oi) {
                auto& ov = state.overlays[oi];
                ImGui::PushID(oi);
                ImGui::Checkbox("##vis", &ov.visible);
                ImGui::SameLine();
                ImGui::TextUnformatted(ov.filename.c_str());
                if (!ov.meta.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", ov.meta.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) remove_idx = oi;
                ImGui::PopID();
            }
            if (remove_idx >= 0)
                state.overlays.erase(state.overlays.begin() + remove_idx);

            if (ImGui::Button("Clear All Overlays"))
                state.overlays.clear();
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

        ImGui::End();  // Left panel

        // ==================================================================
        // RIGHT PANEL – Plot
        // ==================================================================
        ImGui::SetNextWindowPos({lw, 0}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({rw, display.y}, ImGuiCond_Always);
        ImGui::Begin("Plot", nullptr, panel_flags | ImGuiWindowFlags_NoTitleBar);

        // Capture the full window rect for PNG (includes axes, tick labels, legend).
        // Must be read while the window is current, before EndPlot/End.
        ImVec2 plot_win_pos  = ImGui::GetWindowPos();
        ImVec2 plot_win_size = ImGui::GetWindowSize();

        // We want the plot to fill the panel. Pass -1,-1 so ImPlot expands.
        if (ImPlot::BeginPlot("##currents", {-1, -1})) {

            // X axis: no AutoFit – we manage the view window ourselves so that
            // manual pan/zoom is always respected when auto-scroll is off.
            // Y axis: AutoFit so current scale stays sensible.
            ImPlot::SetupAxes("Time (s)", "Current (nA)",
                              ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);

            // Auto-scroll: push limits every frame only while enabled.
            // When the user turns auto-scroll off the condition becomes _Once
            // which means ImPlot won't override subsequent user interaction.
            if (!state.times_plot.empty() && state.auto_scroll) {
                float tmax = state.times_plot.back();
                float tmin = std::max(0.f, tmax - state.scroll_window_sec);
                ImPlot::SetupAxisLimits(ImAxis_X1, (double)tmin, (double)tmax,
                                        ImPlotCond_Always);
            } else if (!state.times_plot.empty()) {
                // First time we have data (or auto-scroll just turned off):
                // fit the full range once so the user starts at a sensible view,
                // but don't override if they've already panned/zoomed.
                float tmin = state.times_plot.front();
                float tmax = state.times_plot.back();
                ImPlot::SetupAxisLimits(ImAxis_X1, (double)tmin, (double)tmax,
                                        ImPlotCond_Once);
            }

            // -- Live data --
            if (!state.times_plot.empty()) {
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

            // -- CSV overlays (ghost traces) --
            for (int oi = 0; oi < (int)state.overlays.size(); ++oi) {
                const auto& ov = state.overlays[oi];
                if (!ov.visible || ov.times.empty()) continue;

                if (state.show_all_ch) {
                    for (int ch = 0; ch < (int)CNTDigitizer::kNumChannels; ++ch) {
                        if (!ch_visible[ch]) continue;
                        const auto& series = ov.channels[ch];
                        int n = (int)std::min(ov.times.size(), series.size());
                        if (n == 0) continue;
                        char lbl[64];
                        snprintf(lbl, sizeof(lbl), "%s Ch%d", ov.filename.c_str(), ch);
                        ImPlot::SetNextLineStyle(OverlayColor(oi, ch), 1.5f);
                        ImPlot::PlotLine(lbl, ov.times.data(), series.data(), n);
                    }
                } else {
                    int ch = state.selected_channel;
                    const auto& series = ov.channels[ch];
                    int n = (int)std::min(ov.times.size(), series.size());
                    if (n > 0) {
                        char lbl[64];
                        snprintf(lbl, sizeof(lbl), "%s Ch%d", ov.filename.c_str(), ch);
                        ImPlot::SetNextLineStyle(OverlayColor(oi, ch), 1.5f);
                        ImPlot::PlotLine(lbl, ov.times.data(), series.data(), n);
                    }
                }
            }

            // -- Crosshair cursor + tooltip --
            // Show a crosshair and value readout when the mouse is inside the plot.
            if (ImPlot::IsPlotHovered()) {
                ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                double mx = mouse.x;
                double my = mouse.y;

                // Vertical line at cursor x
                ImPlot::SetNextLineStyle({1.f, 1.f, 0.4f, 0.6f}, 1.f);
                double xs[2] = { mx, mx };
                double ys_v[2] = { ImPlot::GetPlotLimits().Y.Min,
                                   ImPlot::GetPlotLimits().Y.Max };
                ImPlot::PlotLine("##vline", xs, ys_v, 2);

                // Horizontal line at cursor y
                ImPlot::SetNextLineStyle({1.f, 1.f, 0.4f, 0.6f}, 1.f);
                double xs_h[2] = { ImPlot::GetPlotLimits().X.Min,
                                   ImPlot::GetPlotLimits().X.Max };
                double ys_h[2] = { my, my };
                ImPlot::PlotLine("##hline", xs_h, ys_h, 2);

                // Tooltip
                // Find the nearest live data point to the cursor x for all visible channels
                ImGui::BeginTooltip();
                ImGui::Text("t = %.4f s", mx);
                ImGui::Text("y = %.2f nA", my);
                ImGui::Separator();

                // Helper: binary-search nearest index
                auto nearest_idx = [](const std::vector<float>& times, double tx) -> int {
                    if (times.empty()) return -1;
                    int lo = 0, hi = (int)times.size() - 1;
                    while (lo < hi) {
                        int mid = (lo + hi) / 2;
                        if (times[mid] < (float)tx) lo = mid + 1;
                        else hi = mid;
                    }
                    // Check both lo and lo-1, pick closer
                    if (lo > 0 && std::fabs(times[lo-1] - tx) < std::fabs(times[lo] - tx))
                        return lo - 1;
                    return lo;
                };

                if (!state.times_plot.empty()) {
                    int idx = nearest_idx(state.times_plot, mx);
                    if (idx >= 0) {
                        if (state.show_all_ch) {
                            for (int ch = 0; ch < (int)CNTDigitizer::kNumChannels; ++ch) {
                                if (!ch_visible[ch]) continue;
                                const auto& s = state.ch_plot[ch];
                                if (idx < (int)s.size())
                                    ImGui::TextColored(kChannelColors[ch],
                                        "Ch%d: %.2f nA", ch, s[idx]);
                            }
                        } else {
                            int ch = state.selected_channel;
                            const auto& s = state.ch_plot[ch];
                            if (idx < (int)s.size())
                                ImGui::TextColored(kChannelColors[ch],
                                    "Ch%d: %.2f nA", ch, s[idx]);
                        }
                    }
                }

                // Also show overlay values at cursor
                for (int oi = 0; oi < (int)state.overlays.size(); ++oi) {
                    const auto& ov = state.overlays[oi];
                    if (!ov.visible || ov.times.empty()) continue;
                    int idx = nearest_idx(ov.times, mx);
                    if (idx < 0) continue;

                    ImGui::Separator();
                    ImGui::TextDisabled("[%s]", ov.filename.c_str());

                    if (state.show_all_ch) {
                        for (int ch = 0; ch < (int)CNTDigitizer::kNumChannels; ++ch) {
                            if (!ch_visible[ch]) continue;
                            const auto& s = ov.channels[ch];
                            if (idx < (int)s.size())
                                ImGui::TextColored(OverlayColor(oi, ch),
                                    "Ch%d: %.2f nA", ch, s[idx]);
                        }
                    } else {
                        int ch = state.selected_channel;
                        const auto& s = ov.channels[ch];
                        if (idx < (int)s.size())
                            ImGui::TextColored(OverlayColor(oi, ch),
                                "Ch%d: %.2f nA", ch, s[idx]);
                    }
                }

                ImGui::EndTooltip();
            }

            ImPlot::EndPlot();
        }

        // Capture the full right-panel rect (axes + labels + legend) for PNG.
        // Done here, after EndPlot but while the window is still current.
        last_plot_pos  = plot_win_pos;
        last_plot_size = plot_win_size;

        ImGui::End();  // Right panel

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

        glfwSwapBuffers(window);

        // PNG capture happens AFTER swap so the front buffer has the full frame
        if (request_plot_png) {
            request_plot_png = false;
            glFinish();
            if (SavePlotPNG(png_filename, last_plot_pos, last_plot_size, fw, fh))
                status_line = "Plot PNG saved: output/" + png_filename;
            else
                last_error = "Plot PNG save failed.";
        }
    }

    // ------------------------------------------------------------------
    // Save config on exit so all current settings persist next launch
    // ------------------------------------------------------------------
    {
        AppConfig exit_cfg;
        exit_cfg.port             = state.port;
        exit_cfg.use_wifi         = state.use_wifi;
        exit_cfg.wifi_ip          = std::string(state.wifi_teensy_ip.data());
        exit_cfg.wifi_listen      = (int)state.wifi_listen_port;
        exit_cfg.wifi_teensy      = (int)state.wifi_teensy_port;
        exit_cfg.bias_mv          = param_bias_mv;
        exit_cfg.gate_mv          = param_gate_mv;
        exit_cfg.bsd_ms           = param_bsd_ms;
        exit_cfg.md_ms            = param_md_ms;
        exit_cfg.auto_scroll      = state.auto_scroll;
        exit_cfg.scroll_window    = state.scroll_window_sec;
        exit_cfg.show_all_ch      = state.show_all_ch;
        exit_cfg.ema_enabled      = state.ema_enabled;
        exit_cfg.ema_alpha        = state.ema_alpha;
        exit_cfg.max_points       = state.max_points;
        exit_cfg.selected_channel = state.selected_channel;
        exit_cfg.fullscreen       = is_fullscreen;
        exit_cfg.save_all_channels   = save_all_channels;
        exit_cfg.auto_clear_on_start = auto_clear_on_start;
        // Save windowed size/pos for next windowed launch
        if (!is_fullscreen) {
            glfwGetWindowPos(window, &exit_cfg.win_x, &exit_cfg.win_y);
            glfwGetWindowSize(window, &exit_cfg.win_w, &exit_cfg.win_h);
        } else {
            exit_cfg.win_x = restore.x; exit_cfg.win_y = restore.y;
            exit_cfg.win_w = restore.w; exit_cfg.win_h = restore.h;
        }
        SaveConfig("config.ini", exit_cfg);
        logMessage("Config saved to config.ini");
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
