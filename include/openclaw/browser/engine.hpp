#pragma once

#include "openclaw/browser/cdp_client.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace openclaw::browser {

// Unified browser path: prefer CDP when available/mock, else curl session (cookies, redirects, links/forms).
class Engine {
public:
    explicit Engine(std::filesystem::path data_dir = {});

    nlohmann::json Status() const;

    // open / navigate — CDP if up, else curl fallback
    nlohmann::json OpenUrl(const std::string& url);
    nlohmann::json Navigate(const std::string& url);  // alias emphasizing CDP
    nlohmann::json Evaluate(const std::string& expression);
    nlohmann::json Screenshot(const nlohmann::json& params = {});
    nlohmann::json TitleAndUrl();

    // Multi-step curl session helpers (always available as fallback surface)
    nlohmann::json CurlFetch(const std::string& url);
    nlohmann::json SessionState() const;

private:
    std::filesystem::path root_;
    mutable std::mutex mu_;
    CdpClient cdp_;
    bool cdp_tried_ = false;
    bool cdp_ok_ = false;

    // curl session
    std::string cookie_jar_;
    std::string last_url_;
    std::string last_title_;
    std::string last_body_;
    std::vector<std::string> last_links_;
    std::vector<nlohmann::json> last_forms_;
    int seq_ = 1;

    bool EnsureCdp();
    nlohmann::json OpenUrlCurl(const std::string& url);
    static void ExtractLinksForms(const std::string& html, const std::string& base_url,
                                  std::vector<std::string>& links, std::vector<nlohmann::json>& forms);
};

}  // namespace openclaw::browser
