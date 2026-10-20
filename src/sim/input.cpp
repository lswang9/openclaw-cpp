#include "openclaw/sim/input.hpp"
#include "openclaw/util.hpp"

#include <cstdlib>
#include <sstream>

namespace openclaw::sim {
namespace {

bool EnvTruthy(const char* key) {
    const char* v = std::getenv(key);
    return v && (std::string(v) == "1" || std::string(v) == "true" || std::string(v) == "yes");
}

bool CommandExists(const std::string& name) {
    std::string cmd = "command -v " + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

int Run(const std::string& cmd) { return std::system(cmd.c_str()); }

std::string ShellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string MapButton(const std::string& button) {
    // xdotool: 1=left 2=middle 3=right
    if (button == "right" || button == "3") return "3";
    if (button == "middle" || button == "2") return "2";
    return "1";
}

nlohmann::json SimResult(const std::string& action, nlohmann::json detail, const std::string& note) {
    detail["ok"] = true;
    detail["impl"] = "real-sim";
    detail["engine"] = "event-log";
    detail["mode"] = "simulation";
    detail["action"] = action;
    detail["note"] = note;
    return detail;
}

}  // namespace

InputEnv ProbeInputEnv() {
    InputEnv e;
    e.force_sim = EnvTruthy("OPENCLAW_INPUT_FORCE_SIM") || EnvTruthy("OPENCLAW_COMPUTER_FORCE_SIM");
    if (const char* d = std::getenv("DISPLAY"); d && *d) {
        e.display = d;
        e.has_display = true;
    }
    // Wayland-only: ydotool may work without DISPLAY
    if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w) e.has_display = true;
    if (CommandExists("xdotool")) e.tools_available.push_back("xdotool");
    if (CommandExists("ydotool")) e.tools_available.push_back("ydotool");
    return e;
}

nlohmann::json InputMouseMove(int x, int y) {
    auto env = ProbeInputEnv();
    nlohmann::json detail{{"x", x}, {"y", y}};
    if (env.force_sim) {
        return SimResult("mouse_move", detail, "OPENCLAW_*_FORCE_SIM — input not injected");
    }
    if (!env.has_display || env.tools_available.empty()) {
        return SimResult("mouse_move", detail, "no DISPLAY/input tool — event logged only");
    }
    if (CommandExists("xdotool") && !env.display.empty()) {
        std::ostringstream oss;
        oss << "DISPLAY=" << ShellQuote(env.display) << " xdotool mousemove --sync " << x << " " << y
            << " >/dev/null 2>&1";
        if (Run(oss.str()) == 0) {
            return {{"ok", true},
                    {"impl", "real"},
                    {"engine", "xdotool"},
                    {"mode", "live"},
                    {"action", "mouse_move"},
                    {"x", x},
                    {"y", y},
                    {"display", env.display},
                    {"note", "xdotool mousemove"}};
        }
    }
    if (CommandExists("ydotool")) {
        std::ostringstream oss;
        oss << "ydotool mousemove --absolute " << x << " " << y << " >/dev/null 2>&1";
        if (Run(oss.str()) == 0) {
            return {{"ok", true},
                    {"impl", "real"},
                    {"engine", "ydotool"},
                    {"mode", "live"},
                    {"action", "mouse_move"},
                    {"x", x},
                    {"y", y},
                    {"note", "ydotool mousemove"}};
        }
    }
    return SimResult("mouse_move", detail, "input tool failed — event logged only");
}

nlohmann::json InputMouseClick(const std::string& button, int x, int y) {
    auto env = ProbeInputEnv();
    nlohmann::json detail{{"button", button.empty() ? "left" : button}, {"x", x}, {"y", y}};
    if (env.force_sim) {
        return SimResult("mouse_click", detail, "OPENCLAW_*_FORCE_SIM — input not injected");
    }
    if (!env.has_display || env.tools_available.empty()) {
        return SimResult("mouse_click", detail, "no DISPLAY/input tool — event logged only");
    }
    const std::string btn = MapButton(button.empty() ? "left" : button);
    if (CommandExists("xdotool") && !env.display.empty()) {
        std::ostringstream oss;
        oss << "DISPLAY=" << ShellQuote(env.display) << " xdotool ";
        if (x >= 0 && y >= 0) oss << "mousemove --sync " << x << " " << y << " ";
        oss << "click " << btn << " >/dev/null 2>&1";
        if (Run(oss.str()) == 0) {
            return {{"ok", true},
                    {"impl", "real"},
                    {"engine", "xdotool"},
                    {"mode", "live"},
                    {"action", "mouse_click"},
                    {"button", button.empty() ? "left" : button},
                    {"x", x},
                    {"y", y},
                    {"display", env.display},
                    {"note", "xdotool click"}};
        }
    }
    if (CommandExists("ydotool")) {
        // ydotool click: 0xC0 left down+up roughly — keep simple: click 0xC0
        std::ostringstream oss;
        if (x >= 0 && y >= 0) oss << "ydotool mousemove --absolute " << x << " " << y << " && ";
        int code = 0xC0;  // BTN_LEFT
        if (btn == "3") code = 0xC1;
        if (btn == "2") code = 0xC2;
        oss << "ydotool click " << code << " >/dev/null 2>&1";
        if (Run(oss.str()) == 0) {
            return {{"ok", true},
                    {"impl", "real"},
                    {"engine", "ydotool"},
                    {"mode", "live"},
                    {"action", "mouse_click"},
                    {"button", button.empty() ? "left" : button},
                    {"x", x},
                    {"y", y},
                    {"note", "ydotool click"}};
        }
    }
    return SimResult("mouse_click", detail, "input tool failed — event logged only");
}

nlohmann::json InputTypeText(const std::string& text) {
    auto env = ProbeInputEnv();
    nlohmann::json detail{{"text", text}};
    if (env.force_sim) {
        return SimResult("type", detail, "OPENCLAW_*_FORCE_SIM — input not injected");
    }
    if (!env.has_display || env.tools_available.empty()) {
        return SimResult("type", detail, "no DISPLAY/input tool — event logged only");
    }
    if (CommandExists("xdotool") && !env.display.empty()) {
        // --clearmodifiers avoids stuck modifiers; type as literal
        std::ostringstream oss;
        oss << "DISPLAY=" << ShellQuote(env.display) << " xdotool type --clearmodifiers --delay 1 -- "
            << ShellQuote(text) << " >/dev/null 2>&1";
        if (Run(oss.str()) == 0) {
            return {{"ok", true},
                    {"impl", "real"},
                    {"engine", "xdotool"},
                    {"mode", "live"},
                    {"action", "type"},
                    {"text", text},
                    {"display", env.display},
                    {"note", "xdotool type"}};
        }
    }
    if (CommandExists("ydotool")) {
        std::ostringstream oss;
        oss << "ydotool type -- " << ShellQuote(text) << " >/dev/null 2>&1";
        if (Run(oss.str()) == 0) {
            return {{"ok", true},
                    {"impl", "real"},
                    {"engine", "ydotool"},
                    {"mode", "live"},
                    {"action", "type"},
                    {"text", text},
                    {"note", "ydotool type"}};
        }
    }
    return SimResult("type", detail, "input tool failed — event logged only");
}

nlohmann::json InputKey(const std::string& keys) {
    auto env = ProbeInputEnv();
    nlohmann::json detail{{"keys", keys}};
    if (env.force_sim || keys.empty()) {
        return SimResult("key", detail,
                         env.force_sim ? "OPENCLAW_*_FORCE_SIM — input not injected" : "empty keys");
    }
    if (!env.has_display || env.tools_available.empty()) {
        return SimResult("key", detail, "no DISPLAY/input tool — event logged only");
    }
    if (CommandExists("xdotool") && !env.display.empty()) {
        // Allow sequences like "ctrl+c" or "Return"
        std::ostringstream oss;
        oss << "DISPLAY=" << ShellQuote(env.display) << " xdotool key --clearmodifiers -- "
            << ShellQuote(keys) << " >/dev/null 2>&1";
        if (Run(oss.str()) == 0) {
            return {{"ok", true},
                    {"impl", "real"},
                    {"engine", "xdotool"},
                    {"mode", "live"},
                    {"action", "key"},
                    {"keys", keys},
                    {"display", env.display},
                    {"note", "xdotool key"}};
        }
    }
    return SimResult("key", detail, "input tool failed — event logged only");
}

}  // namespace openclaw::sim
