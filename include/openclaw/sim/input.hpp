#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace openclaw::sim {

// Linux OS input injection for computer.click / type / move.
// Prefer xdotool (X11); ydotool if present. Else real-sim event log only.

struct InputEnv {
    bool force_sim = false;  // OPENCLAW_INPUT_FORCE_SIM or OPENCLAW_COMPUTER_FORCE_SIM
    bool has_display = false;
    std::string display;
    std::vector<std::string> tools_available;  // xdotool, ydotool
};

InputEnv ProbeInputEnv();

nlohmann::json InputMouseMove(int x, int y);
nlohmann::json InputMouseClick(const std::string& button, int x = -1, int y = -1);
nlohmann::json InputTypeText(const std::string& text);
nlohmann::json InputKey(const std::string& keys);  // e.g. "Return", "ctrl+c"

}  // namespace openclaw::sim
