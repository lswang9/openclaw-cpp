#include "openclaw/sim/capture.hpp"
#include "openclaw/util.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace openclaw::sim {
namespace {

bool EnvTruthy(const char* key) {
    const char* v = std::getenv(key);
    return v && (std::string(v) == "1" || std::string(v) == "true" || std::string(v) == "yes");
}

bool CommandExists(const std::string& name) {
    // Use `command -v` via shell; avoid depending on PATH walk in C++.
    std::string cmd = "command -v " + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

int Run(const std::string& cmd) {
    return std::system(cmd.c_str());
}

std::uintmax_t FileSize(const std::filesystem::path& p) {
    std::error_code ec;
    auto n = std::filesystem::file_size(p, ec);
    return ec ? 0 : n;
}

std::string ShellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string MinimalPngBytes() {
    static const unsigned char k[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
        0x89,0x00,0x00,0x00,0x0A,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x00,0x01,0x00,0x00,
        0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,
        0x42,0x60,0x82};
    return std::string(reinterpret_cast<const char*>(k), sizeof(k));
}

nlohmann::json Placeholder(const std::filesystem::path& out_path, const std::string& reason,
                           const CaptureEnv& env) {
    auto png = MinimalPngBytes();
    {
        std::ofstream out(out_path, std::ios::binary);
        out.write(png.data(), static_cast<std::streamsize>(png.size()));
    }
    return {{"ok", true},
            {"impl", "real-sim"},
            {"engine", "placeholder"},
            {"mode", "simulation"},
            {"path", out_path.string()},
            {"bytes", png.size()},
            {"content_type", "image/png"},
            {"display", env.display},
            {"wayland", env.wayland},
            {"tools_available", env.tools_available},
            {"note", reason}};
}

}  // namespace

CaptureEnv ProbeCaptureEnv() {
    CaptureEnv e;
    e.force_sim = EnvTruthy("OPENCLAW_SCREENSHOT_FORCE_SIM") || EnvTruthy("OPENCLAW_COMPUTER_FORCE_SIM");
    if (const char* d = std::getenv("DISPLAY"); d && *d) {
        e.display = d;
        e.has_display = true;
    }
    if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w) {
        e.wayland = w;
        e.has_display = true;
    }
    for (const char* tool : {"scrot", "import", "gnome-screenshot", "ffmpeg", "grim"}) {
        if (CommandExists(tool)) e.tools_available.push_back(tool);
    }
    return e;
}

nlohmann::json CaptureScreenshotPng(const std::filesystem::path& out_path,
                                    const nlohmann::json& params) {
    (void)params;
    std::filesystem::create_directories(out_path.parent_path());
    auto env = ProbeCaptureEnv();
    if (env.force_sim) {
        return Placeholder(out_path, "OPENCLAW_SCREENSHOT_FORCE_SIM=1 — placeholder PNG", env);
    }
    if (!env.has_display) {
        return Placeholder(out_path,
                           "no DISPLAY/WAYLAND_DISPLAY — placeholder PNG (not a real capture)", env);
    }

    const std::string outq = ShellQuote(out_path.string());
    std::vector<std::pair<std::string, std::string>> attempts;

    // scrot
    if (CommandExists("scrot")) {
        attempts.push_back({"scrot", "scrot -o " + outq + " >/dev/null 2>&1"});
    }
    // ImageMagick import
    if (CommandExists("import")) {
        attempts.push_back({"import", "import -window root " + outq + " >/dev/null 2>&1"});
    }
    // gnome-screenshot
    if (CommandExists("gnome-screenshot")) {
        attempts.push_back(
            {"gnome-screenshot", "gnome-screenshot -f " + outq + " >/dev/null 2>&1"});
    }
    // grim (Wayland)
    if (!env.wayland.empty() && CommandExists("grim")) {
        attempts.push_back({"grim", "grim " + outq + " >/dev/null 2>&1"});
    }
    // ffmpeg x11grab (X11)
    if (!env.display.empty() && CommandExists("ffmpeg")) {
        std::string disp = env.display;
        if (disp.find('.') == std::string::npos) disp += ".0";
        // Grab full screen; -update 1 writes a single PNG.
        std::ostringstream oss;
        oss << "ffmpeg -y -loglevel error -f x11grab -i " << ShellQuote(disp)
            << " -frames:v 1 -update 1 " << outq << " >/dev/null 2>&1";
        attempts.push_back({"ffmpeg-x11grab", oss.str()});
    }

    if (attempts.empty()) {
        return Placeholder(out_path,
                           "display present but no capture tool (scrot/import/ffmpeg/grim) — placeholder",
                           env);
    }

    std::vector<std::string> errors;
    for (const auto& [engine, cmd] : attempts) {
        std::error_code ec;
        std::filesystem::remove(out_path, ec);
        int rc = Run(cmd);
        auto bytes = FileSize(out_path);
        if (rc == 0 && bytes > 100) {  // real PNG >> 1x1 placeholder (~68B)
            return {{"ok", true},
                    {"impl", "real"},
                    {"engine", engine},
                    {"mode", "live"},
                    {"path", out_path.string()},
                    {"bytes", bytes},
                    {"content_type", "image/png"},
                    {"display", env.display},
                    {"wayland", env.wayland},
                    {"tools_available", env.tools_available},
                    {"note", "live display capture via " + engine}};
        }
        errors.push_back(engine + " rc=" + std::to_string(rc) + " bytes=" + std::to_string(bytes));
    }

    auto ph = Placeholder(out_path, "all capture backends failed — placeholder PNG", env);
    ph["attempts"] = errors;
    return ph;
}

}  // namespace openclaw::sim
