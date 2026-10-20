#pragma once
#include "openclaw/browser/engine.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace openclaw::sim {

class SimulationBackend {
public:
    explicit SimulationBackend(std::filesystem::path data_dir);

    nlohmann::json Status() const;
    nlohmann::json EventLog(std::size_t limit = 50) const;

    nlohmann::json ComputerStatus();
    nlohmann::json ComputerInvoke(const nlohmann::json& params);
    nlohmann::json Screenshot(const nlohmann::json& params = {});

    nlohmann::json CanvasView(const nlohmann::json& params = {});
    nlohmann::json CanvasPreview(const nlohmann::json& params = {});
    nlohmann::json CanvasPut(const nlohmann::json& params);

    nlohmann::json DesktopLaunch(const nlohmann::json& params);
    nlohmann::json DesktopObserve(const nlohmann::json& params = {});
    nlohmann::json DesktopRelease(const nlohmann::json& params = {});

    nlohmann::json BrowserOpenUrl(const std::string& url);
    nlohmann::json BrowserNavigate(const std::string& url);
    nlohmann::json BrowserEvaluate(const std::string& expression);
    nlohmann::json BrowserScreenshot(const nlohmann::json& params = {});
    nlohmann::json BrowserStatus() const;
    browser::Engine& Browser();
    const browser::Engine& Browser() const;

private:
    std::filesystem::path root_;
    std::vector<nlohmann::json> events_;
    nlohmann::json canvas_ = {{"title", "sim-canvas"}, {"widgets", nlohmann::json::array()}};
    nlohmann::json desktop_ = {{"running", false}, {"app", ""}};
    int seq_ = 1;
    std::unique_ptr<browser::Engine> browser_;

    void Record(const std::string& kind, const nlohmann::json& detail);
    static std::string MinimalPngBytes();
};

}  // namespace openclaw::sim
