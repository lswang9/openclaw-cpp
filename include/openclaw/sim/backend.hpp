#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace openclaw::sim {

// Deterministic local simulation for computer/canvas/desktop RPCs.
// NOT a real OS/desktop/browser automation backend.
// Responses carry impl:"real-sim" via gateway; this class tags mode=simulation.
class SimulationBackend {
public:
    explicit SimulationBackend(std::filesystem::path data_dir);

    nlohmann::json Status() const;
    nlohmann::json EventLog(std::size_t limit = 50) const;

    // computer.*
    nlohmann::json ComputerStatus();
    nlohmann::json ComputerInvoke(const nlohmann::json& params);
    nlohmann::json Screenshot(const nlohmann::json& params = {});

    // canvas.*
    nlohmann::json CanvasView(const nlohmann::json& params = {});
    nlohmann::json CanvasPreview(const nlohmann::json& params = {});
    nlohmann::json CanvasPut(const nlohmann::json& params);

    // desktop.*
    nlohmann::json DesktopLaunch(const nlohmann::json& params);
    nlohmann::json DesktopObserve(const nlohmann::json& params = {});
    nlohmann::json DesktopRelease(const nlohmann::json& params = {});

    // browser lightweight (curl title + body text store)
    nlohmann::json BrowserOpenUrl(const std::string& url);
    nlohmann::json BrowserStatus() const;

private:
    std::filesystem::path root_;
    std::vector<nlohmann::json> events_;
    nlohmann::json canvas_ = {{"title", "sim-canvas"}, {"widgets", nlohmann::json::array()}};
    nlohmann::json desktop_ = {{"running", false}, {"app", ""}};
    std::string last_url_;
    std::string last_title_;
    std::string last_page_text_;
    int seq_ = 1;

    void Record(const std::string& kind, const nlohmann::json& detail);
    static std::string MinimalPngBytes();
    nlohmann::json HttpGet(const std::string& url) const;
};

}  // namespace openclaw::sim
