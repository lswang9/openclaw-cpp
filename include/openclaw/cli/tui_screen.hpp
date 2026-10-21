#pragma once
// Full-screen terminal UI for `openclaw-cpp tui` (ANSI alternate screen, raw termios, no curses dependency):
// header (gateway / session / state), word-wrapped scrollable transcript, editable input line with history,
// background sends with a spinner, resize handling; live streaming + tool/thinking blocks from gateway events,
// model/agent pickers, and a queue for messages typed while a turn runs. Line mode remains for pipes and --plain.
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace openclaw::cli {

struct TuiBackend {
    // rpc(method, params, timeout_s, &err) → payload or null on error
    std::function<nlohmann::json(const std::string&, const nlohmann::json&, long, std::string*)> rpc;
    std::string base;
    std::string token;  // gateway token for the live event WebSocket (streaming, tool blocks)
    std::string session;
    long timeout_s = 600;
    int history_limit = 200;
    nlohmann::json send_extra = nlohmann::json::object();  // thinking / deliver
    std::string initial_message;
};

int RunFullscreenTui(TuiBackend& b);

}  // namespace openclaw::cli
