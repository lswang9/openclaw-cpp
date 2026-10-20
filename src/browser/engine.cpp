#include "openclaw/browser/engine.hpp"

#include <curl/curl.h>

#include <fstream>
#include <regex>

namespace openclaw::browser {
namespace {
size_t WriteCb(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n);
    return s * n;
}
}  // namespace

Engine::Engine(std::filesystem::path data_dir)
    : root_(data_dir.empty() ? std::filesystem::temp_directory_path() / "openclaw-browser"
                             : std::move(data_dir)),
      cdp_(CdpClient::FromEnv()) {
    std::filesystem::create_directories(root_);
    cookie_jar_ = (root_ / "cookies.txt").string();
}

bool Engine::EnsureCdp() {
    if (cdp_ok_) return true;
    if (cdp_tried_ && !CdpClient::MockEnabled()) return false;
    cdp_tried_ = true;
    auto c = cdp_.Connect();
    cdp_ok_ = c.value("ok", false);
    return cdp_ok_;
}

nlohmann::json Engine::Status() const {
    std::lock_guard lock(mu_);
    auto st = cdp_.Status();
    st["cdp_ok"] = cdp_ok_;
    st["fallback"] = "curl+cookiejar+links";
    st["last_url"] = last_url_;
    st["last_title"] = last_title_;
    st["last_links"] = last_links_.size();
    st["last_forms"] = last_forms_.size();
    st["cookie_jar"] = cookie_jar_;
    if (cdp_ok_ || CdpClient::MockEnabled()) {
        st["mode"] = CdpClient::MockEnabled() ? "cdp-mock" : "cdp";
        st["impl"] = CdpClient::MockEnabled() ? "real-sim" : "real";
    } else {
        st["mode"] = "curl-session";
        st["impl"] = "real-sim";
        st["note"] = "CDP down — curl cookie/redirect/links fallback. Set OPENCLAW_BROWSER_CDP_URL "
                     "and start Chrome with --remote-debugging-port.";
    }
    return st;
}

void Engine::ExtractLinksForms(const std::string& html, const std::string& base_url,
                               std::vector<std::string>& links, std::vector<nlohmann::json>& forms) {
    links.clear();
    forms.clear();
    static const std::regex href_re(R"re(href\s*=\s*["']([^"']+)["'])re", std::regex::icase);
    static const std::regex form_re(R"re(<form\b([^>]*)>([\s\S]*?)</form>)re", std::regex::icase);
    static const std::regex action_re(R"re(action\s*=\s*["']([^"']*)["'])re", std::regex::icase);
    static const std::regex method_re(R"re(method\s*=\s*["']([^"']*)["'])re", std::regex::icase);
    static const std::regex input_re(
        R"re(<input\b[^>]*\bname\s*=\s*["']([^"']+)["'][^>]*>)re", std::regex::icase);

    std::sregex_iterator it(html.begin(), html.end(), href_re);
    const std::sregex_iterator rend;
    for (; it != rend && links.size() < 50; ++it) {
        std::string h = (*it)[1].str();
        if (h.rfind("javascript:", 0) == 0 || h.rfind("#", 0) == 0) continue;
        if (h.rfind("http://", 0) != 0 && h.rfind("https://", 0) != 0) {
            // crude resolve
            if (!h.empty() && h[0] == '/') {
                auto scheme = base_url.find("://");
                if (scheme != std::string::npos) {
                    auto host_end = base_url.find('/', scheme + 3);
                    auto origin = host_end == std::string::npos ? base_url : base_url.substr(0, host_end);
                    h = origin + h;
                }
            }
        }
        links.push_back(h);
    }
    std::sregex_iterator fit(html.begin(), html.end(), form_re);
    for (; fit != rend && forms.size() < 20; ++fit) {
        std::string attrs = (*fit)[1].str();
        std::string body = (*fit)[2].str();
        std::smatch m;
        std::string action = base_url;
        std::string method = "GET";
        if (std::regex_search(attrs, m, action_re)) action = m[1].str();
        if (std::regex_search(attrs, m, method_re)) method = m[1].str();
        nlohmann::json fields = nlohmann::json::array();
        std::sregex_iterator iit(body.begin(), body.end(), input_re);
        for (; iit != rend && fields.size() < 30; ++iit) fields.push_back((*iit)[1].str());
        forms.push_back({{"action", action}, {"method", method}, {"fields", fields}});
    }
}

nlohmann::json Engine::OpenUrlCurl(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return {{"ok", false}, {"error", "curl init"}};
    std::string body;
    char errbuf[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "openclaw-cpp-browser/1.6");
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookie_jar_.c_str());
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_jar_.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    char* final_url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final_url);
    std::string effective = final_url ? final_url : url;
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        return {{"ok", false}, {"error", errbuf[0] ? errbuf : curl_easy_strerror(rc)}, {"url", url}};
    }
    if (body.size() > 1024 * 1024) body.resize(1024 * 1024);
    std::string title;
    std::smatch m;
    static const std::regex re("<title[^>]*>([^<]*)</title>", std::regex::icase);
    if (std::regex_search(body, m, re)) title = m[1].str();
    ExtractLinksForms(body, effective, last_links_, last_forms_);
    last_url_ = effective;
    last_title_ = title;
    last_body_ = body.size() > 64 * 1024 ? body.substr(0, 64 * 1024) : body;
    nlohmann::json links = nlohmann::json::array();
    for (auto& l : last_links_) links.push_back(l);
    return {{"ok", true},
            {"impl", "real-sim"},
            {"engine", "curl-session"},
            {"mode", "curl-session"},
            {"url", effective},
            {"title", title},
            {"http", http},
            {"text_bytes", last_body_.size()},
            {"links", links},
            {"forms", last_forms_},
            {"cookie_jar", cookie_jar_},
            {"note", "curl + cookie jar + link/form extract — not full Chromium DOM"}};
}

nlohmann::json Engine::OpenUrl(const std::string& url) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0 && url != "about:blank") {
        return {{"ok", false}, {"error", "only http/https/about:blank"}};
    }
    {
        std::lock_guard lock(mu_);
        if (EnsureCdp()) {
            auto nav = cdp_.Navigate(url == "about:blank" ? "about:blank" : url);
            if (nav.value("ok", false)) {
                last_url_ = nav.value("url", url);
                last_title_ = nav.value("title", "");
                return nav;
            }
            cdp_ok_ = false;
        }
    }
    std::lock_guard lock(mu_);
    return OpenUrlCurl(url);
}

nlohmann::json Engine::Navigate(const std::string& url) { return OpenUrl(url); }

nlohmann::json Engine::Evaluate(const std::string& expression) {
    std::lock_guard lock(mu_);
    if (EnsureCdp()) {
        auto r = cdp_.Evaluate(expression);
        if (r.value("ok", false)) return r;
        cdp_ok_ = false;
    }
    // curl fallback: only support a few expressions against last_body_
    if (expression.find("document.title") != std::string::npos) {
        return {{"ok", true},
                {"impl", "real-sim"},
                {"engine", "curl-session"},
                {"value", last_title_},
                {"note", "from last curl fetch — no live DOM"}};
    }
    if (expression.find("location.href") != std::string::npos) {
        return {{"ok", true},
                {"impl", "real-sim"},
                {"engine", "curl-session"},
                {"value", last_url_},
                {"note", "from last curl fetch — no live DOM"}};
    }
    return {{"ok", false},
            {"error", "evaluate requires CDP"},
            {"hint", "set OPENCLAW_BROWSER_CDP_URL and start Chrome with --remote-debugging-port"},
            {"impl", "missing"}};
}

nlohmann::json Engine::Screenshot(const nlohmann::json& params) {
    std::filesystem::path out;
    if (params.contains("path")) out = params["path"].get<std::string>();
    std::lock_guard lock(mu_);
    if (EnsureCdp()) {
        if (out.empty()) out = root_ / ("shot-" + std::to_string(seq_++) + ".png");
        auto r = cdp_.ScreenshotPng(out);
        if (r.value("ok", false)) return r;
        cdp_ok_ = false;
    }
    return {{"ok", false},
            {"error", "screenshot requires CDP"},
            {"hint", "Chrome --remote-debugging-port + OPENCLAW_BROWSER_CDP_URL"},
            {"impl", "missing"}};
}

nlohmann::json Engine::TitleAndUrl() {
    std::lock_guard lock(mu_);
    if (EnsureCdp()) {
        auto r = cdp_.TitleAndUrl();
        if (r.value("ok", false)) {
            last_url_ = r.value("url", last_url_);
            last_title_ = r.value("title", last_title_);
            return r;
        }
        cdp_ok_ = false;
    }
    return {{"ok", true},
            {"impl", "real-sim"},
            {"engine", "curl-session"},
            {"url", last_url_},
            {"title", last_title_}};
}

nlohmann::json Engine::CurlFetch(const std::string& url) {
    std::lock_guard lock(mu_);
    return OpenUrlCurl(url);
}

nlohmann::json Engine::SessionState() const {
    std::lock_guard lock(mu_);
    nlohmann::json links = nlohmann::json::array();
    for (auto& l : last_links_) links.push_back(l);
    return {{"url", last_url_},
            {"title", last_title_},
            {"text_bytes", last_body_.size()},
            {"links", links},
            {"forms", last_forms_},
            {"cookie_jar", cookie_jar_},
            {"cdp_ok", cdp_ok_}};
}

}  // namespace openclaw::browser
