#include "openclaw/tools/registry.hpp"
#include "openclaw/approvals/store.hpp"
#include <curl/curl.h>
#include <cctype>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <stdexcept>

namespace openclaw::tools {

void Registry::Register(ToolSpec spec) { tools_[spec.name] = std::move(spec); }

bool Registry::Has(const std::string& name) const { return tools_.find(name) != tools_.end(); }

const ToolSpec* Registry::Get(const std::string& name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second;
}

std::string Registry::Invoke(const std::string& name, const nlohmann::json& args) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return nlohmann::json{{"error", "unknown tool: " + name}}.dump();
    }
    try {
        return it->second.handler(args);
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

nlohmann::json Registry::OpenAiToolsArray() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [_, spec] : tools_) {
        arr.push_back({
            {"type", "function"},
            {"function",
             {{"name", spec.name},
              {"description", spec.description},
              {"parameters", spec.parameters}}},
        });
    }
    return arr;
}

std::vector<std::string> Registry::Names() const {
    std::vector<std::string> names;
    for (const auto& [n, _] : tools_) names.push_back(n);
    return names;
}

Registry MakeDefaultRegistry(RegistryOptions opts) {
    Registry r;

    ToolSpec echo;
    echo.name = "echo";
    echo.description = "Echo back the provided text.";
    echo.parameters = {
        {"type", "object"},
        {"properties", {{"text", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"text"})},
    };
    echo.handler = [](const nlohmann::json& args) {
        return nlohmann::json{{"echoed", args.value("text", "")}}.dump();
    };
    r.Register(std::move(echo));

    ToolSpec now;
    now.name = "time_now";
    now.description = "Return current UTC time ISO-8601.";
    now.parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    now.handler = [](const nlohmann::json&) {
        using clock = std::chrono::system_clock;
        auto t = clock::to_time_t(clock::now());
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return nlohmann::json{{"utc", buf}}.dump();
    };
    r.Register(std::move(now));

    static nlohmann::json g_mem = nlohmann::json::object();
    ToolSpec mset;
    mset.name = "memory_set";
    mset.description = "Store a short memory note by key.";
    mset.parameters = {
        {"type", "object"},
        {"properties", {{"key", {{"type", "string"}}}, {"value", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"key", "value"})},
    };
    mset.handler = [](const nlohmann::json& args) {
        g_mem[args.at("key").get<std::string>()] = args.at("value").get<std::string>();
        return nlohmann::json{{"ok", true}}.dump();
    };
    r.Register(std::move(mset));

    ToolSpec mget;
    mget.name = "memory_get";
    mget.description = "Retrieve a memory note by key.";
    mget.parameters = {
        {"type", "object"},
        {"properties", {{"key", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"key"})},
    };
    mget.handler = [](const nlohmann::json& args) {
        auto key = args.at("key").get<std::string>();
        if (!g_mem.contains(key)) return nlohmann::json{{"found", false}}.dump();
        return nlohmann::json{{"found", true}, {"value", g_mem[key]}}.dump();
    };
    r.Register(std::move(mget));

    if (opts.enable_shell) {
        if (const char* env = std::getenv("OPENCLAW_SHELL_REQUIRE_APPROVAL"); env && std::string(env) == "0") {
            opts.shell_requires_approval = false;
        }
        ToolSpec shell;
        shell.name = "shell";
        shell.description =
            "Mock shell tool (records command; does not exec by default). Requires approval when gated.";
        shell.requires_approval = opts.shell_requires_approval;
        shell.parameters = {
            {"type", "object"},
            {"properties",
             {{"command", {{"type", "string"}}},
              {"approval_id", {{"type", "string"}, {"description", "Approved approval id"}}}}},
            {"required", nlohmann::json::array({"command"})},
        };
        auto* appr = opts.approvals;
        bool need = opts.shell_requires_approval;
        shell.handler = [appr, need](const nlohmann::json& args) -> std::string {
            const std::string cmd = args.value("command", "");
            if (need) {
                if (!appr) {
                    return nlohmann::json{{"error", "approvals store required for shell"},
                                          {"needs_approval", true}}.dump();
                }
                const std::string approval_id = args.value("approval_id", "");
                if (approval_id.empty()) {
                    auto id = appr->Create("exec", "shell: " + cmd);
                    return nlohmann::json{{"needs_approval", true},
                                          {"approval_id", id},
                                          {"status", "pending"},
                                          {"command", cmd}}.dump();
                }
                auto info = appr->GetJson(approval_id);
                if (info.value("status", "") != "approved") {
                    return nlohmann::json{{"error", "approval not approved"},
                                          {"approval", info}}.dump();
                }
            }
            // Mock execution only (safe default).
            return nlohmann::json{{"ok", true},
                                 {"mocked", true},
                                 {"command", cmd},
                                 {"stdout", "mock: executed (no real shell)"}}.dump();
        };
        r.Register(std::move(shell));
    }


    // Real HTTP GET via libcurl (SSRF-conscious: blocks localhost/private by default unless allow_private)
    {
        ToolSpec http;
        http.name = "http_get";
        http.description = "Fetch a public HTTP(S) URL and return status + body text (truncated).";
        http.parameters = {
            {"type", "object"},
            {"properties",
             {{"url", {{"type", "string"}}},
              {"max_bytes", {{"type", "integer"}}},
              {"allow_private", {{"type", "boolean"}}}}},
            {"required", nlohmann::json::array({"url"})},
        };
        http.handler = [](const nlohmann::json& args) -> std::string {
            const std::string url = args.value("url", "");
            if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
                return nlohmann::json{{"error", "only http/https URLs allowed"}}.dump();
            }
            bool allow_private = args.value("allow_private", false);
            auto lower = url;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!allow_private) {
                if (lower.find("localhost") != std::string::npos || lower.find("127.0.0.1") != std::string::npos ||
                    lower.find("0.0.0.0") != std::string::npos || lower.find("::1") != std::string::npos ||
                    lower.find("169.254.") != std::string::npos || lower.find("10.") != std::string::npos ||
                    lower.find("192.168.") != std::string::npos) {
                    return nlohmann::json{{"error", "private/localhost URLs blocked (set allow_private)"}}.dump();
                }
            }
            size_t max_b = static_cast<size_t>(args.value("max_bytes", 65536));
            if (max_b > 1 << 20) max_b = 1 << 20;
            CURL* curl = curl_easy_init();
            if (!curl) return nlohmann::json{{"error", "curl init failed"}}.dump();
            std::string body;
            auto write = [](char* p, size_t s, size_t n, void* u) -> size_t {
                auto* o = static_cast<std::string*>(u);
                o->append(p, s * n);
                return s * n;
            };
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +write);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "openclaw-cpp-http_get/1.1");
            CURLcode rc = curl_easy_perform(curl);
            long http = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
            curl_easy_cleanup(curl);
            if (rc != CURLE_OK) {
                return nlohmann::json{{"ok", false}, {"error", curl_easy_strerror(rc)}, {"url", url}}.dump();
            }
            bool truncated = body.size() > max_b;
            if (truncated) body.resize(max_b);
            return nlohmann::json{{"ok", true},
                                  {"impl", "real"},
                                  {"url", url},
                                  {"http", http},
                                  {"bytes", body.size()},
                                  {"truncated", truncated},
                                  {"body", body}}.dump();
        };
        r.Register(std::move(http));
    }

    // Browser: honest not-configured stub (no headless chrome bundled)
    {
        ToolSpec br;
        br.name = "browser";
        br.description = "Browser automation — not configured in openclaw-cpp (use http_get for simple fetch).";
        br.parameters = {
            {"type", "object"},
            {"properties",
             {{"action", {{"type", "string"}}},
              {"url", {{"type", "string"}}}}},
        };
        br.handler = [](const nlohmann::json& args) {
            auto action = args.value("action", "");
            if (action == "open_url" || action == "open") {
                return nlohmann::json{{"ok", false},
                                      {"error", "use tool browser_open_url"},
                                      {"hint", "browser_open_url for curl+title; no Playwright"}}.dump();
            }
            return nlohmann::json{{"ok", false},
                                  {"configured", false},
                                  {"impl", "stub"},
                                  {"error", "full browser not configured"},
                                  {"action", action},
                                  {"hint", "use browser_open_url or http_get; no Playwright/Chromium"}}.dump();
        };
        r.Register(std::move(br));
    }

    {
        ToolSpec bo;
        bo.name = "browser_open_url";
        bo.description = "Lightweight URL open: curl fetch, extract <title>, store page text (not Playwright).";
        bo.parameters = {
            {"type", "object"},
            {"properties", {{"url", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"url"})},
        };
        // Actual sim is on gateway; tool does its own curl for agent use without ServerDeps
        bo.handler = [](const nlohmann::json& args) -> std::string {
            const std::string url = args.value("url", "");
            if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
                return nlohmann::json{{"error", "only http/https"}}.dump();
            }
            CURL* curl = curl_easy_init();
            if (!curl) return nlohmann::json{{"error", "curl init"}}.dump();
            std::string body;
            auto write = [](char* p, size_t s, size_t n, void* u) -> size_t {
                static_cast<std::string*>(u)->append(p, s * n); return s * n;
            };
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +write);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "openclaw-cpp-browser_open_url/1.4");
            CURLcode rc = curl_easy_perform(curl);
            long http = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
            curl_easy_cleanup(curl);
            if (rc != CURLE_OK) return nlohmann::json{{"ok", false}, {"error", curl_easy_strerror(rc)}}.dump();
            std::string title;
            auto p1 = body.find("<title");
            if (p1 != std::string::npos) {
                p1 = body.find('>', p1);
                auto p2 = body.find("</title>", p1);
                if (p1 != std::string::npos && p2 != std::string::npos) title = body.substr(p1 + 1, p2 - p1 - 1);
            }
            if (body.size() > 32768) body.resize(32768);
            return nlohmann::json{{"ok", true},
                                  {"mode", "lightweight"},
                                  {"url", url},
                                  {"title", title},
                                  {"http", http},
                                  {"text_bytes", body.size()},
                                  {"text_prefix", body.substr(0, 500)}}.dump();
        };
        r.Register(std::move(bo));
    }


    return r;
}

}  // namespace openclaw::tools
