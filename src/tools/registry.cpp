#include "openclaw/workspace/workspace.hpp"
#include <memory>
#include <filesystem>
#include <sstream>
#include "openclaw/tools/registry.hpp"
#include "openclaw/approvals/store.hpp"
#include "openclaw/browser/engine.hpp"
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

std::size_t Registry::MergeFrom(const Registry& other) {
    std::size_t n = 0;
    for (const auto& name : other.Names()) {
        if (Has(name)) continue;
        if (const auto* spec = other.Get(name)) {
            Register(*spec);
            ++n;
        }
    }
    return n;
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

    // Workspace file + Markdown memory tools (real, sandboxed to the workspace root).
    auto ws = std::make_shared<workspace::Workspace>(opts.workspace_root.empty()
                                                          ? workspace::Workspace::DefaultRoot()
                                                          : std::filesystem::path(opts.workspace_root));
    auto str = [](const char* d) { return nlohmann::json{{"type", "string"}, {"description", d}}; };
    auto num = [](const char* d) { return nlohmann::json{{"type", "integer"}, {"description", d}}; };
    r.Register({"read", "Read a text file in the agent workspace (line-numbered window).",
                {{"type", "object"}, {"properties", {{"path", str("workspace-relative path")},
                                                     {"offset", num("1-based start line")}, {"limit", num("max lines")}}},
                 {"required", nlohmann::json::array({"path"})}},
                [ws](const nlohmann::json& a) {
                    return ws->Read(a.value("path", ""), a.value("offset", 1), a.value("limit", 2000)).dump(); }});
    r.Register({"write", "Create or overwrite a file in the agent workspace.",
                {{"type", "object"}, {"properties", {{"path", str("workspace-relative path")}, {"content", str("full file content")}}},
                 {"required", nlohmann::json::array({"path", "content"})}},
                [ws](const nlohmann::json& a) { return ws->Write(a.value("path", ""), a.value("content", "")).dump(); }});
    r.Register({"edit", "Replace an exact, unique string in a workspace file (or all matches with replace_all).",
                {{"type", "object"}, {"properties", {{"path", str("workspace-relative path")}, {"old_string", str("exact text to replace")},
                                                     {"new_string", str("replacement")}, {"replace_all", {{"type", "boolean"}}}}},
                 {"required", nlohmann::json::array({"path", "old_string", "new_string"})}},
                [ws](const nlohmann::json& a) {
                    return ws->Edit(a.value("path", ""), a.value("old_string", ""), a.value("new_string", ""),
                                    a.value("replace_all", false)).dump(); }});
    r.Register({"memory_search", "Search MEMORY.md and memory/*.md (BM25 over line-ranged chunks). Returns path, lines, score, snippet.",
                {{"type", "object"}, {"properties", {{"query", str("what to recall")}, {"maxResults", num("default 6")},
                                                     {"minScore", {{"type", "number"}}}}},
                 {"required", nlohmann::json::array({"query"})}},
                [ws](const nlohmann::json& a) {
                    return ws->MemorySearch(a.value("query", ""), a.value("maxResults", 6), a.value("minScore", 0.0)).dump(); }});
    r.Register({"memory_get", "Read lines from a memory file (MEMORY.md or memory/**/*.md). Legacy: {key} finds the latest '- ... key: value' note.",
                {{"type", "object"}, {"properties", {{"path", str("memory file path")}, {"from", num("1-based start line")},
                                                     {"lines", num("line count")}, {"key", str("legacy note key")}}}},
                [ws](const nlohmann::json& a) {
                    if (a.contains("path")) return ws->MemoryGet(a.value("path", ""), a.value("from", 1), a.value("lines", 0)).dump();
                    const auto key = a.value("key", "");
                    if (key.empty()) return nlohmann::json{{"ok", false}, {"error", "path or key required"}}.dump();
                    auto hits = ws->MemorySearch(key, 50);
                    const std::string needle = " " + key + ": ";
                    for (auto& h : hits["results"]) {
                        auto snip = h.value("snippet", "");
                        auto lines_v = snip; std::string found;
                        std::istringstream is(lines_v); std::string l;
                        while (std::getline(is, l)) if (auto p = l.find(needle); p != std::string::npos) found = l.substr(p + needle.size());
                        if (!found.empty()) return nlohmann::json{{"found", true}, {"value", found}, {"path", h["path"]}}.dump();
                    }
                    return nlohmann::json{{"found", false}}.dump(); }});
    r.Register({"memory_set", "Append a durable note '- key: value' to today's memory/YYYY-MM-DD.md.",
                {{"type", "object"}, {"properties", {{"key", str("note key")}, {"value", str("note text")}}},
                 {"required", nlohmann::json::array({"key", "value"})}},
                [ws](const nlohmann::json& a) {
                    return ws->MemoryAppend(a.value("key", "") + ": " + a.value("value", "")).dump(); }});

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

    // Browser: CDP when available/mock; else curl-session fallback
    {
        static browser::Engine eng;
        ToolSpec br;
        br.name = "browser";
        br.description =
            "Browser automation via CDP (Chrome --remote-debugging-port) or curl-session fallback. "
            "Actions: status, open/navigate, evaluate, screenshot, title.";
        br.parameters = {
            {"type", "object"},
            {"properties",
             {{"action", {{"type", "string"},
                          {"description", "status|open|navigate|evaluate|screenshot|title"}}},
              {"url", {{"type", "string"}}},
              {"expression", {{"type", "string"}}},
              {"path", {{"type", "string"}}}}},
        };
        br.handler = [](const nlohmann::json& args) {
            auto action = args.value("action", "status");
            if (action == "status") return eng.Status().dump();
            if (action == "open_url" || action == "open" || action == "navigate") {
                return eng.OpenUrl(args.value("url", "")).dump();
            }
            if (action == "evaluate" || action == "eval") {
                return eng.Evaluate(args.value("expression", args.value("js", ""))).dump();
            }
            if (action == "screenshot" || action == "capture") {
                nlohmann::json p;
                if (args.contains("path")) p["path"] = args["path"];
                return eng.Screenshot(p).dump();
            }
            if (action == "title" || action == "url") return eng.TitleAndUrl().dump();
            return nlohmann::json{{"ok", false},
                                  {"error", "unknown action"},
                                  {"action", action},
                                  {"hint", "status|open|navigate|evaluate|screenshot|title"}}.dump();
        };
        r.Register(std::move(br));
    }

    {
        static browser::Engine eng_open;
        ToolSpec bo;
        bo.name = "browser_open_url";
        bo.description =
            "Open URL: CDP navigate when Chrome debugging is up, else curl+cookies+links/forms "
            "(not Playwright). Env OPENCLAW_BROWSER_CDP_URL.";
        bo.parameters = {
            {"type", "object"},
            {"properties", {{"url", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"url"})},
        };
        bo.handler = [](const nlohmann::json& args) -> std::string {
            return eng_open.OpenUrl(args.value("url", "")).dump();
        };
        r.Register(std::move(bo));
    }

    {
        static browser::Engine eng_eval;
        ToolSpec be;
        be.name = "browser_evaluate";
        be.description = "Evaluate JS in the CDP page (requires Chrome remote debugging; mock via OPENCLAW_BROWSER_CDP_MOCK=1).";
        be.parameters = {
            {"type", "object"},
            {"properties", {{"expression", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"expression"})},
        };
        be.handler = [](const nlohmann::json& args) -> std::string {
            return eng_eval.Evaluate(args.value("expression", "")).dump();
        };
        r.Register(std::move(be));
    }

    {
        static browser::Engine eng_shot;
        ToolSpec bs;
        bs.name = "browser_screenshot";
        bs.description = "Capture PNG via CDP Page.captureScreenshot (requires Chrome CDP).";
        bs.parameters = {
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
        };
        bs.handler = [](const nlohmann::json& args) -> std::string {
            nlohmann::json p;
            if (args.contains("path")) p["path"] = args["path"];
            return eng_shot.Screenshot(p).dump();
        };
        r.Register(std::move(bs));
    }




    return r;
}

}  // namespace openclaw::tools
