#pragma once
// Real X11 desktop control: launch (policy-gated) / track / release processes, window list/focus/close via xdotool,
// and the AT-SPI accessibility tree + actions (click, set text) via python3-gi Atspi. Falls back (available=false)
// when DISPLAY, xdotool, or Atspi are missing — callers keep the simulation path then.
#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace openclaw::desktop {

class Controller {
public:
    nlohmann::json Status() const;
    bool Available() const;        // DISPLAY + xdotool
    bool A11yAvailable() const;    // python3 + gi Atspi
    // Policy: OPENCLAW_DESKTOP_LAUNCH=1 allows any app; else OPENCLAW_DESKTOP_APPS (comma list of basenames).
    nlohmann::json Launch(const nlohmann::json& params);   // {app, args[], wait_ms}
    nlohmann::json Windows() const;
    nlohmann::json Focus(const nlohmann::json& params);    // {window}
    nlohmann::json Close(const nlohmann::json& params);    // {window}
    nlohmann::json Release(const nlohmann::json& params);  // {pid} or all launched
    nlohmann::json A11yTree(const nlohmann::json& params) const;   // {max_nodes, app}
    nlohmann::json A11yAction(const nlohmann::json& params);       // {path:[..], action:"click"|index, text?}
    nlohmann::json Observe(const nlohmann::json& params) const;    // windows + a11y summary
    ~Controller();

private:
    mutable std::mutex mu_;
    std::vector<int> pids_;
};

Controller& Default();

}  // namespace openclaw::desktop
