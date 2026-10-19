#include "openclaw/sim/backend.hpp"

#include <curl/curl.h>
#include <chrono>
#include <fstream>
#include <regex>

namespace openclaw::sim {
namespace {
size_t WriteCb(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n);
    return s * n;
}
}  // namespace

SimulationBackend::SimulationBackend(std::filesystem::path data_dir)
    : root_(std::move(data_dir) / "sim") {
    std::filesystem::create_directories(root_);
}

void SimulationBackend::Record(const std::string& kind, const nlohmann::json& detail) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    events_.push_back({{"ts_ms", ms}, {"kind", kind}, {"detail", detail}});
    if (events_.size() > 500) events_.erase(events_.begin());
}

std::string SimulationBackend::MinimalPngBytes() {
    // 1x1 transparent PNG
    static const unsigned char k[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
        0x89,0x00,0x00,0x00,0x0A,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x00,0x01,0x00,0x00,
        0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,
        0x42,0x60,0x82};
    return std::string(reinterpret_cast<const char*>(k), sizeof(k));
}

nlohmann::json SimulationBackend::Status() const {
    return {{"mode", "simulation"},
            {"backend", "SimulationBackend"},
            {"events", events_.size()},
            {"desktop", desktop_},
            {"canvas_title", canvas_.value("title", "")},
            {"last_url", last_url_},
            {"note", "NOT production OS/desktop automation — deterministic local sim"}};
}

nlohmann::json SimulationBackend::EventLog(std::size_t limit) const {
    nlohmann::json arr = nlohmann::json::array();
    const std::size_t start = events_.size() > limit ? events_.size() - limit : 0;
    for (std::size_t i = start; i < events_.size(); ++i) arr.push_back(events_[i]);
    return arr;
}

nlohmann::json SimulationBackend::ComputerStatus() {
    return {{"mode", "simulation"},
            {"online", true},
            {"capabilities", nlohmann::json::array({"screenshot", "mouse", "keyboard", "invoke"})},
            {"events", events_.size()}};
}

nlohmann::json SimulationBackend::Screenshot(const nlohmann::json& params) {
    (void)params;
    auto png = MinimalPngBytes();
    auto path = root_ / ("screenshot-" + std::to_string(seq_++) + ".png");
    std::ofstream out(path, std::ios::binary);
    out.write(png.data(), static_cast<std::streamsize>(png.size()));
    Record("screenshot", {{"path", path.string()}, {"bytes", png.size()}});
    return {{"mode", "simulation"},
            {"path", path.string()},
            {"bytes", png.size()},
            {"content_type", "image/png"},
            {"note", "placeholder 1x1 PNG — not a real display capture"}};
}

nlohmann::json SimulationBackend::ComputerInvoke(const nlohmann::json& params) {
    auto action = params.value("action", params.value("op", "noop"));
    nlohmann::json result{{"mode", "simulation"}, {"action", action}, {"ok", true}};
    if (action == "screenshot" || action == "capture") {
        result.merge_patch(Screenshot(params));
    } else if (action == "mouse_move" || action == "move") {
        Record("mouse_move", params);
        result["x"] = params.value("x", 0);
        result["y"] = params.value("y", 0);
    } else if (action == "mouse_click" || action == "click") {
        Record("mouse_click", params);
        result["button"] = params.value("button", "left");
    } else if (action == "key" || action == "type" || action == "keyboard") {
        Record("keyboard", params);
        result["text"] = params.value("text", params.value("keys", ""));
    } else {
        Record("invoke", params);
        result["echo"] = params;
    }
    return result;
}

nlohmann::json SimulationBackend::CanvasView(const nlohmann::json& params) {
    (void)params;
    Record("canvas.view", nlohmann::json::object());
    return {{"mode", "simulation"}, {"document", canvas_}};
}

nlohmann::json SimulationBackend::CanvasPreview(const nlohmann::json& params) {
    (void)params;
    Record("canvas.preview", nlohmann::json::object());
    return {{"mode", "simulation"},
            {"preview", {{"title", canvas_.value("title", "")},
                         {"widget_count", canvas_.value("widgets", nlohmann::json::array()).size()}}},
            {"note", "text preview only — no raster compositor"}};
}

nlohmann::json SimulationBackend::CanvasPut(const nlohmann::json& params) {
    if (params.contains("title")) canvas_["title"] = params["title"];
    if (params.contains("widgets")) canvas_["widgets"] = params["widgets"];
    if (params.contains("document") && params["document"].is_object()) canvas_ = params["document"];
    Record("canvas.put", params);
    return {{"mode", "simulation"}, {"document", canvas_}, {"saved", true}};
}

nlohmann::json SimulationBackend::DesktopLaunch(const nlohmann::json& params) {
    desktop_["running"] = true;
    desktop_["app"] = params.value("app", params.value("name", "sim-app"));
    desktop_["args"] = params.value("args", nlohmann::json::array());
    Record("desktop.launch", desktop_);
    return {{"mode", "simulation"}, {"desktop", desktop_},
            {"note", "no real process spawned"}};
}

nlohmann::json SimulationBackend::DesktopObserve(const nlohmann::json& params) {
    (void)params;
    Record("desktop.observe", desktop_);
    auto shot = Screenshot({});
    return {{"mode", "simulation"},
            {"desktop", desktop_},
            {"screenshot", shot},
            {"events_recent", EventLog(10)}};
}

nlohmann::json SimulationBackend::DesktopRelease(const nlohmann::json& params) {
    (void)params;
    desktop_["running"] = false;
    Record("desktop.release", desktop_);
    return {{"mode", "simulation"}, {"desktop", desktop_}, {"released", true}};
}

nlohmann::json SimulationBackend::HttpGet(const std::string& url) const {
    CURL* curl = curl_easy_init();
    if (!curl) return {{"ok", false}, {"error", "curl init"}};
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "openclaw-cpp-browser.open_url/1.4");
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) return {{"ok", false}, {"error", curl_easy_strerror(rc)}};
    if (body.size() > 512 * 1024) body.resize(512 * 1024);
    return {{"ok", true}, {"http", http}, {"body", body}};
}

nlohmann::json SimulationBackend::BrowserOpenUrl(const std::string& url) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return {{"ok", false}, {"error", "only http/https"}, {"mode", "lightweight"}};
    }
    auto got = HttpGet(url);
    if (!got.value("ok", false)) {
        Record("browser.open_url_fail", {{"url", url}, {"error", got}});
        return got;
    }
    std::string body = got.value("body", "");
    std::string title;
    std::smatch m;
    static const std::regex re("<title[^>]*>([^<]*)</title>", std::regex::icase);
    if (std::regex_search(body, m, re)) title = m[1].str();
    // strip scripts roughly for text store
    std::string text = body;
    if (text.size() > 32 * 1024) text.resize(32 * 1024);
    last_url_ = url;
    last_title_ = title;
    last_page_text_ = text;
    Record("browser.open_url", {{"url", url}, {"title", title}, {"http", got.value("http", 0)}});
    return {{"ok", true},
            {"mode", "lightweight"},
            {"impl_note", "curl fetch + title regex — not Playwright/Chromium"},
            {"url", url},
            {"title", title},
            {"http", got.value("http", 0)},
            {"text_bytes", text.size()}};
}

nlohmann::json SimulationBackend::BrowserStatus() const {
    return {{"configured", true},
            {"mode", "lightweight"},
            {"engine", "curl+title"},
            {"last_url", last_url_},
            {"last_title", last_title_},
            {"last_text_bytes", last_page_text_.size()},
            {"note", "no headless browser; use browser.open_url / http_get"}};
}

}  // namespace openclaw::sim
