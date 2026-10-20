#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace openclaw::sim {

// Linux screen capture helpers for computer.screenshot.
// Prefer external tools when DISPLAY/Wayland is usable; else placeholder PNG.

struct CaptureEnv {
    bool force_sim = false;       // OPENCLAW_SCREENSHOT_FORCE_SIM=1
    bool has_display = false;     // DISPLAY or WAYLAND_DISPLAY
    std::string display;          // e.g. ":7"
    std::string wayland;
    std::vector<std::string> tools_available;  // scrot, import, ffmpeg, …
};

CaptureEnv ProbeCaptureEnv();

// Write PNG to out_path. Returns json with ok, impl (real|real-sim), engine, path, bytes, …
nlohmann::json CaptureScreenshotPng(const std::filesystem::path& out_path,
                                    const nlohmann::json& params = {});

}  // namespace openclaw::sim
