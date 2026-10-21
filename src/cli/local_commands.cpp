// Upstream v2026.10.5 local commands (see include/openclaw/cli/local_commands.hpp).
#include "openclaw/cli/local_commands.hpp"
#include "openclaw/gateway/subsystems.hpp"
#include "openclaw/cli/tui_screen.hpp"

#include "openclaw/config/config.hpp"

#include <curl/curl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace openclaw::cli {
namespace {

// ---------- argv ----------
struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;  // "--flag" -> value ("" for booleans present)
    std::vector<std::string> multi_section;
    bool has(const std::string& k) const { return opt.count(k) > 0; }
    std::string get(const std::string& k, const std::string& d = "") const { auto it = opt.find(k); return it == opt.end() ? d : it->second; }
};
// Parse argv[from..] against an allowed flag list ("--x=" takes a value). Unknown flag → commander-style error.
std::optional<Args> Parse(int argc, char** argv, int from, const std::vector<std::string>& allowed, const std::string& cmd,
                          const std::map<std::string, std::string>& shorts = {}) {
    Args a;
    for (int i = from; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--") { for (++i; i < argc; ++i) a.pos.push_back(argv[i]); break; }
        if (auto it = shorts.find(s); it != shorts.end()) s = it->second;
        if (s.size() > 2 && s.rfind("--", 0) == 0) {
            std::string val; bool inl = false;
            if (auto eq = s.find('='); eq != std::string::npos) { val = s.substr(eq + 1); s = s.substr(0, eq); inl = true; }
            const bool takes = std::find(allowed.begin(), allowed.end(), s + "=") != allowed.end();
            const bool flag = std::find(allowed.begin(), allowed.end(), s) != allowed.end();
            if (!takes && !flag) { std::cerr << "error: unknown option '" << s << "'\n(run `openclaw-cpp " << cmd << " --help`)\n"; return std::nullopt; }
            if (takes && !inl) {
                if (i + 1 >= argc) { std::cerr << "error: option '" << s << " <value>' argument missing\n"; return std::nullopt; }
                val = argv[++i];
            }
            if (s == "--section") a.multi_section.push_back(val);
            a.opt[s] = val;
        } else {
            a.pos.push_back(s);
        }
    }
    return a;
}

// ---------- prompts (stdin lines; secrets without echo on a tty) ----------
bool g_eof = false;
std::string ReadLine(bool secret = false) {
    std::string line;
    termios old{};
    const bool tty = secret && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) { termios t = old; t.c_lflag &= ~static_cast<tcflag_t>(ECHO); tcsetattr(STDIN_FILENO, TCSANOW, &t); }
    if (!std::getline(std::cin, line)) g_eof = true;
    if (tty) { tcsetattr(STDIN_FILENO, TCSANOW, &old); std::cerr << '\n'; }
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}
std::string Ask(const std::string& q, const std::string& def = "", bool secret = false) {
    std::cerr << "◆ " << q << (def.empty() || secret ? "" : " [" + def + "]") << ": " << std::flush;
    auto v = ReadLine(secret);
    return v.empty() ? def : v;
}
bool Confirm(const std::string& q, bool def) {
    auto v = Ask(q + (def ? " (Y/n)" : " (y/N)"));
    if (v.empty()) return def;
    return v[0] == 'y' || v[0] == 'Y';
}
std::string Choose(const std::string& q, const std::vector<std::pair<std::string, std::string>>& opts, const std::string& def) {
    std::cerr << "◆ " << q << '\n';
    for (size_t i = 0; i < opts.size(); ++i) std::cerr << "  " << (i + 1) << ") " << opts[i].first << (opts[i].second.empty() ? "" : " — " + opts[i].second) << '\n';
    for (int tries = 0; tries < 5 && !g_eof; ++tries) {
        auto v = Ask("choose", def);
        for (size_t i = 0; i < opts.size(); ++i) if (v == opts[i].first || v == std::to_string(i + 1)) return opts[i].first;
        std::cerr << "  invalid choice: " << v << '\n';
    }
    return def;
}

// ---------- config / paths ----------
fs::path Home() { const char* h = std::getenv("HOME"); return h ? fs::path(h) : fs::path("/tmp"); }
fs::path StateDir() {
    if (const char* s = std::getenv("OPENCLAW_STATE_DIR"); s && *s) return s;
    return Home() / ".openclaw";
}
std::string Short(const fs::path& p) {
    auto s = p.string(), h = Home().string();
    return s.rfind(h + "/", 0) == 0 ? "~" + s.substr(h.size()) : s;
}
struct Cfg { fs::path path; bool exists = false; bool valid = true; std::string error; json raw = json::object(); };
Cfg LoadCfg() {
    Cfg c; c.path = config::DefaultPath();
    std::error_code ec;
    c.exists = fs::exists(c.path, ec);
    if (!c.exists) return c;
    auto r = config::Load(c.path);
    if (!r.error.empty()) { c.valid = false; c.error = r.error; return c; }
    std::ifstream in(c.path); std::stringstream ss; ss << in.rdbuf();
    try { c.raw = config::ParseJson5(ss.str()); } catch (const std::exception& e) { c.valid = false; c.error = e.what(); }
    if (!c.raw.is_object()) c.raw = json::object();
    return c;
}
void SaveCfg(const Cfg& c) {
    std::error_code ec; fs::create_directories(c.path.parent_path(), ec);
    config::SaveRaw(c.path, c.raw);
}
json Get(const json& j, const std::string& p) { auto v = config::GetPath(j, p); return v ? *v : json(); }
void Set(json& j, const std::string& p, const json& v) { config::SetPath(j, p, v); }
std::string DefaultWorkspace() { return (StateDir() / "workspace").string(); }
std::string ExpandHome(std::string p) { if (p.rfind("~/", 0) == 0) p = (Home() / p.substr(2)).string(); return p; }
std::string RandomToken() {
    std::random_device rd; std::string t; const char* hex = "0123456789abcdef";
    for (int i = 0; i < 48; ++i) t.push_back(hex[rd() % 16]);
    return t;
}
std::string Stamp() {
    auto t = std::time(nullptr); char b[32]; std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", std::localtime(&t)); return b;
}

// Workspace bootstrap files (upstream templates). BOOTSTRAP.md only for a brand-new workspace.
const std::vector<std::pair<std::string, std::string>>& Templates() {
    static const std::vector<std::pair<std::string, std::string>> k = {
#include "workspace_templates.inc"
    };
    return k;
}
json EnsureWorkspace(const fs::path& ws, bool skip_bootstrap) {
    std::error_code ec;
    const bool brand_new = !fs::exists(ws, ec) || fs::is_empty(ws, ec);
    fs::create_directories(ws / "memory", ec);
    json created = json::array();
    if (!skip_bootstrap)
        for (auto& [name, body] : Templates()) {
            if (name == "BOOTSTRAP.md" && !brand_new) continue;
            if (fs::exists(ws / name, ec)) continue;
            std::ofstream(ws / name) << body;
            created.push_back(name);
        }
    return {{"path", ws.string()}, {"brandNew", brand_new}, {"created", created}};
}
void EnsureSessions() { std::error_code ec; fs::create_directories(StateDir() / "agents" / "main" / "sessions", ec); }

// ---------- HTTP helpers ----------
std::size_t Sink(char* p, std::size_t s, std::size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }
long HttpGet(const std::string& url, std::string* body, long timeout_s = 3) {
    CURL* h = curl_easy_init(); if (!h) return 0;
    std::string b;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, Sink); curl_easy_setopt(h, CURLOPT_WRITEDATA, &b);
    long code = 0;
    if (curl_easy_perform(h) == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(h);
    if (body) *body = b;
    return code;
}
std::string GatewayBase(const json& raw, const std::string& url_flag = "") {
    if (!url_flag.empty()) { std::string u = url_flag; if (u.rfind("ws://", 0) == 0) u = "http://" + u.substr(5); if (u.rfind("wss://", 0) == 0) u = "https://" + u.substr(6); while (!u.empty() && u.back() == '/') u.pop_back(); return u; }
    if (const char* e = std::getenv("OPENCLAW_GATEWAY_URL"); e && *e) return e;
    if (auto r = Get(raw, "gateway.remote.url"); r.is_string() && Get(raw, "gateway.mode") == "remote") return GatewayBase(raw, r.get<std::string>());
    int port = 18789;
    if (const char* p = std::getenv("OPENCLAW_GATEWAY_PORT")) port = std::atoi(p);
    else if (auto p2 = Get(raw, "gateway.port"); p2.is_number_integer()) port = p2.get<int>();
    return "http://127.0.0.1:" + std::to_string(port);
}
json Rpc(const std::string& base, const std::string& token, const std::string& method, const json& params, long timeout_s, std::string* err) {
    CURL* h = curl_easy_init(); if (!h) { *err = "curl init"; return nullptr; }
    json req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
    std::string body = req.dump(), out, url = base + "/v1/rpc", auth;
    curl_slist* hdr = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!token.empty()) { auth = "Authorization: Bearer " + token; hdr = curl_slist_append(hdr, auth.c_str()); }
    curl_easy_setopt(h, CURLOPT_URL, url.c_str()); curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str()); curl_easy_setopt(h, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, Sink); curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
    auto rc = curl_easy_perform(h);
    curl_slist_free_all(hdr); curl_easy_cleanup(h);
    if (rc != CURLE_OK) { *err = std::string("gateway not reachable at ") + base + " (" + curl_easy_strerror(rc) + ")"; return nullptr; }
    json j; try { j = json::parse(out); } catch (...) { *err = "bad response: " + out.substr(0, 200); return nullptr; }
    if (j.contains("error") && !j["error"].is_null()) { *err = j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump(); return nullptr; }
    if (j.contains("ok") && j["ok"].is_boolean() && !j["ok"].get<bool>()) { *err = "request failed"; return nullptr; }
    return j.contains("payload") ? j["payload"] : j.contains("result") ? j["result"] : j;
}

// ---------- shared sections (onboard + configure) ----------
const std::vector<std::pair<std::string, std::string>> kProviders = {
    {"openai", "https://api.openai.com/v1|gpt-4.1"}, {"anthropic", "https://api.anthropic.com|claude-sonnet-4-5"},
    {"deepseek", "https://api.deepseek.com|deepseek-chat"}, {"openrouter", "https://openrouter.ai/api/v1|openai/gpt-4.1"},
    {"gemini", "https://generativelanguage.googleapis.com/v1beta|gemini-2.5-pro"}};
std::pair<std::string, std::string> ProviderDefaults(const std::string& id) {
    for (auto& [k, v] : kProviders) if (k == id) return {v.substr(0, v.find('|')), v.substr(v.find('|') + 1)};
    return {"", ""};
}
void ApplyModel(json& raw, const std::string& provider, const std::string& key, const std::string& model, const std::string& base_url) {
    auto [def_base, def_model] = ProviderDefaults(provider);
    const std::string m = model.empty() ? def_model : model;
    json p = Get(raw, "models.providers." + provider); if (!p.is_object()) p = json::object();
    if (!key.empty()) p["apiKey"] = key;
    p["baseUrl"] = base_url.empty() ? (p.contains("baseUrl") ? p["baseUrl"] : json(def_base)) : json(base_url);
    if (provider != "openai" && provider != "anthropic" && provider != "gemini") p["api"] = p.value("api", std::string("openai-completions"));
    if (!m.empty()) {
        json models = p.value("models", json::array()); bool have = false;
        for (auto& x : models) if (x.is_object() && x.value("id", "") == m) have = true;
        if (!have) models.push_back({{"id", m}, {"name", m}});
        p["models"] = models;
        Set(raw, "agents.defaults.model.primary", provider + "/" + m);
    }
    Set(raw, "models.providers." + provider, p);
}
void SectionWorkspace(json& raw) {
    auto cur = Get(raw, "agents.defaults.workspace");
    auto ws = ExpandHome(Ask("Workspace directory", cur.is_string() ? cur.get<std::string>() : DefaultWorkspace()));
    Set(raw, "agents.defaults.workspace", ws);
    EnsureWorkspace(ws, Get(raw, "agents.defaults.skipBootstrap") == true);
    EnsureSessions();
    std::cerr << "  workspace OK: " << Short(ws) << '\n';
}
void SectionModel(json& raw) {
    std::vector<std::pair<std::string, std::string>> o;
    for (auto& [k, v] : kProviders) o.push_back({k, v.substr(v.find('|') + 1)});
    o.push_back({"custom", "any OpenAI-compatible endpoint"}); o.push_back({"skip", "configure later"});
    auto prov = Choose("Model provider", o, "openai");
    if (prov == "skip") return;
    if (prov == "custom") {
        auto base = Ask("Base URL", "http://127.0.0.1:11434/v1");
        auto id = Ask("Provider id", "custom");
        auto model = Ask("Model id", "llama3.1");
        auto key = Ask("API key (optional)", "", true);
        ApplyModel(raw, id, key, model, base);
        return;
    }
    auto key = Ask(prov + " API key", "", true);
    auto model = Ask("Model", ProviderDefaults(prov).second);
    ApplyModel(raw, prov, key, model, "");
}
void SectionWeb(json& raw) {
    auto p = Choose("Web search provider", {{"brave", "Brave Search API"}, {"perplexity", "Perplexity"}, {"off", "disable web search"}}, "brave");
    if (p == "off") { Set(raw, "tools.web.search.enabled", false); return; }
    Set(raw, "tools.web.search.enabled", true);
    Set(raw, "tools.web.search.provider", p);
    auto key = Ask(p + " API key", "", true);
    if (!key.empty()) Set(raw, "tools.web.search.apiKey", key);
    Set(raw, "tools.web.fetch.enabled", Confirm("Enable web_fetch", true));
}
void SectionGateway(json& raw) {
    auto port = Ask("Gateway port", Get(raw, "gateway.port").is_number() ? std::to_string(Get(raw, "gateway.port").get<int>()) : "18789");
    Set(raw, "gateway.port", std::atoi(port.c_str()));
    Set(raw, "gateway.bind", Choose("Bind", {{"loopback", "127.0.0.1 only"}, {"lan", "0.0.0.0"}, {"tailnet", "Tailscale address"}, {"auto", ""}}, Get(raw, "gateway.bind").is_string() ? Get(raw, "gateway.bind").get<std::string>() : "loopback"));
    auto mode = Choose("Gateway auth", {{"token", "bearer token (recommended)"}, {"password", "shared password"}}, "token");
    Set(raw, "gateway.auth.mode", mode);
    if (mode == "token") {
        auto t = Ask("Gateway token (blank = generate)", "", true);
        Set(raw, "gateway.auth.token", t.empty() ? RandomToken() : t);
    } else {
        Set(raw, "gateway.auth.password", Ask("Gateway password", "", true));
    }
    if (Get(raw, "gateway.mode").is_null()) Set(raw, "gateway.mode", "local");
}
void SectionDaemon(json&) {
    std::cerr << "  openclaw-cpp does not install a background service; run `openclaw-cpp gateway` under systemd/launchd/your supervisor.\n";
}
void SectionChannels(json& raw) {
    for (int guard = 0; guard < 8 && !g_eof; ++guard) {
        auto ch = Choose("Channel to configure", {{"telegram", "bot token"}, {"discord", "bot token"}, {"slack", "bot + app token"}, {"feishu", "app id + secret"}, {"whatsapp", "Cloud API token"}, {"done", "finish"}}, "done");
        if (ch == "done") break;
        if (ch == "telegram") Set(raw, "channels.telegram.botToken", Ask("Telegram bot token", "", true));
        if (ch == "discord") Set(raw, "channels.discord.token", Ask("Discord bot token", "", true));
        if (ch == "slack") { Set(raw, "channels.slack.botToken", Ask("Slack bot token (xoxb-…)", "", true)); Set(raw, "channels.slack.appToken", Ask("Slack app token (xapp-…)", "", true)); }
        if (ch == "feishu") { Set(raw, "channels.feishu.appId", Ask("Feishu app id")); Set(raw, "channels.feishu.appSecret", Ask("Feishu app secret", "", true)); }
        if (ch == "whatsapp") { Set(raw, "channels.whatsapp.accessToken", Ask("WhatsApp Cloud API token", "", true)); Set(raw, "channels.whatsapp.phoneNumberId", Ask("Phone number id")); }
        Set(raw, "channels." + ch + ".enabled", true);
        Set(raw, "channels." + ch + ".dmPolicy", Choose("DM policy", {{"pairing", "unknown senders get a pairing code"}, {"allowlist", "allowFrom only"}, {"open", "anyone"}, {"disabled", ""}}, "pairing"));
    }
}
void SectionSkills(json&) {
    std::cerr << "  skills are loaded from <workspace>/skills; manage them with `openclaw-cpp skills`.\n";
}
json SectionHealth(const json& raw) {
    std::string body; auto code = HttpGet(GatewayBase(raw) + "/health", &body, 2);
    json r{{"url", GatewayBase(raw) + "/health"}, {"ok", code == 200}};
    if (code != 200) r["hint"] = "gateway not running; start it with `openclaw-cpp gateway`";
    std::cerr << (code == 200 ? "  gateway healthy\n" : "  gateway not reachable (start it with `openclaw-cpp gateway`)\n");
    return r;
}
void SectionHealthFn(json& r) { SectionHealth(r); }
using SectionFn = void (*)(json&);
const std::vector<std::pair<std::string, SectionFn>>& Sections() {
    static const std::vector<std::pair<std::string, SectionFn>> k = {
        {"workspace", SectionWorkspace}, {"model", SectionModel}, {"web", SectionWeb}, {"gateway", SectionGateway},
        {"daemon", SectionDaemon}, {"channels", SectionChannels}, {"skills", SectionSkills}, {"health", SectionHealthFn}};
    return k;
}

// ---------- completion ----------
const std::vector<std::string>& NativeCommands() {
    static const std::vector<std::string> k = {"acp", "approvals", "auth", "channels", "chat", "config", "context", "cron", "dashboard", "discord",
                                               "gateway", "mcp", "memory", "methods", "models", "plugins", "self-test", "skills", "status",
                                               "tokenizer", "tools", "voice", "completion", "setup", "onboard", "configure", "migrate", "tui"};
    return k;
}
using TreeT = std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>>;  // cmd -> (subs, root flags)
TreeT Tree();
std::string Join(const std::vector<std::string>& v, const char* sep = " ") { std::string s; for (auto& x : v) { if (!s.empty()) s += sep; s += x; } return s; }

int CmdCompletion(int argc, char** argv) {
    auto a = Parse(argc, argv, 2, {"--shell=", "--install", "--write-state", "--yes"}, "completion", {{"-s", "--shell"}, {"-i", "--install"}, {"-y", "--yes"}});
    if (!a) return 1;
    std::string detected = "bash";
    if (const char* sh = std::getenv("SHELL")) { std::string s = fs::path(sh).filename(); if (s == "zsh" || s == "fish" || s == "bash") detected = s; else if (s == "pwsh") detected = "powershell"; }
    const std::string shell = a->get("--shell", detected);
    static const std::set<std::string> kShells = {"zsh", "bash", "powershell", "fish"};
    if (!kShells.count(shell)) { std::cerr << "error: option '-s, --shell <shell>' argument '" << shell << "' is invalid. Allowed choices are zsh, bash, powershell, fish.\n"; return 1; }
    const std::string bin = "openclaw-cpp";
    auto ext = [](const std::string& s) { return s == "powershell" ? std::string("ps1") : s; };
    const fs::path dir = StateDir() / "completions";
    if (a->has("--write-state") || a->has("--install")) {
        std::error_code ec; fs::create_directories(dir, ec);
        std::vector<std::string> shells = a->has("--shell") || a->has("--install") ? std::vector<std::string>{shell} : std::vector<std::string>{"zsh", "bash", "powershell", "fish"};
        for (auto& s : shells) std::ofstream(dir / (bin + "." + ext(s))) << CompletionScript(s, bin) << '\n';
        if (a->has("--write-state") && !a->has("--install")) { std::cerr << "Wrote completion scripts to " << Short(dir) << '\n'; return 0; }
    }
    if (a->has("--install")) {
        fs::path profile = shell == "zsh" ? Home() / ".zshrc" : shell == "fish" ? Home() / ".config/fish/config.fish"
                         : shell == "powershell" ? Home() / ".config/powershell/Microsoft.PowerShell_profile.ps1" : Home() / ".bashrc";
        const fs::path script = dir / (bin + "." + ext(shell));
        const std::string marker = "# OpenClaw Completion";
        std::string cur; { std::ifstream in(profile); std::stringstream ss; ss << in.rdbuf(); cur = ss.str(); }
        if (cur.find(marker) != std::string::npos) { std::cerr << "Completion already installed in " << Short(profile) << '\n'; return 0; }
        if (!a->has("--yes") && !Confirm("Add completion to " + Short(profile) + "?", true)) { std::cerr << "Cancelled.\n"; return 1; }
        std::error_code ec; fs::create_directories(profile.parent_path(), ec);
        std::ofstream(profile, std::ios::app) << "\n" << marker << "\n" << (shell == "powershell" ? ". \"" : "source \"") << script.string() << "\"\n";
        std::cerr << "Completion installed in " << Short(profile) << ". Restart your shell or source it.\n";
        return 0;
    }
    std::cout << CompletionScript(shell, bin) << '\n';
    return 0;
}

// ---------- setup / onboard ----------
int Reset(const std::string& scope, Cfg& c, bool json_out) {
    static const std::set<std::string> ok = {"config", "config+creds+sessions", "full"};
    if (!ok.count(scope)) { std::cerr << "Invalid --reset-scope. Use config, config+creds+sessions, or full.\n"; return 1; }
    json removed = json::array(); std::error_code ec;
    const fs::path trash = StateDir() / "trash" / ("reset-" + Stamp());
    auto move = [&](const fs::path& p) { if (!fs::exists(p, ec)) return; fs::create_directories(trash, ec); fs::rename(p, trash / p.filename(), ec); if (!ec) removed.push_back(Short(p)); };
    auto ws = Get(c.raw, "agents.defaults.workspace");
    move(c.path);
    if (scope != "config") { move(StateDir() / "credentials"); move(StateDir() / "agents"); }
    if (scope == "full" && ws.is_string()) move(ExpandHome(ws.get<std::string>()));
    if (!json_out) std::cerr << "Reset (" << scope << "): moved " << removed.size() << " item(s) to " << Short(trash) << '\n';
    c = LoadCfg();
    return 0;
}

int CmdOnboard(int argc, char** argv, int from) {
    static const std::vector<std::string> kFlags = {
        "--workspace=", "--reset", "--reset-scope=", "--non-interactive", "--accept-risk", "--flow=", "--mode=", "--auth-choice=",
        "--token=", "--token-provider=", "--openai-api-key=", "--anthropic-api-key=", "--deepseek-api-key=", "--openrouter-api-key=",
        "--gemini-api-key=", "--custom-base-url=", "--custom-api-key=", "--custom-model-id=", "--custom-provider-id=",
        "--custom-compatibility=", "--gateway-port=", "--gateway-bind=", "--gateway-auth=", "--gateway-token=", "--gateway-password=",
        "--remote-url=", "--remote-token=", "--remote-password=", "--tailscale=", "--install-daemon", "--no-install-daemon",
        "--skip-daemon", "--daemon-runtime=", "--skip-channels", "--skip-skills", "--skip-bootstrap", "--skip-search", "--skip-health",
        "--skip-ui", "--skip-hooks", "--agent-name=", "--team", "--json", "--classic", "--tui", "--modern", "--suppress-gateway-token-output",
        "--node-manager=", "--import-from=", "--import-source=", "--import-secrets", "--secret-input-mode=", "--wizard", "--yes", "--baseline",
        "--message="};
    auto a = Parse(argc, argv, from, kFlags, "onboard", {{"-m", "--message"}});
    if (!a) return 1;
    const bool ni = a->has("--non-interactive"), jo = a->has("--json");
    if (ni && !a->has("--accept-risk")) {
        std::cerr << "Non-interactive onboarding requires explicit risk acknowledgement.\n"
                     "Agents are powerful and full system access is risky. Re-run with: openclaw-cpp onboard --non-interactive --accept-risk ...\n";
        return 1;
    }
    Cfg c = LoadCfg();
    if (c.exists && !c.valid) { std::cerr << "Config invalid at " << Short(c.path) << ": " << c.error << "\nFix it (`openclaw-cpp config validate`), then re-run onboarding.\n"; return 1; }
    if (a->has("--reset")) { if (int rc = Reset(a->get("--reset-scope", "config+creds+sessions"), c, jo); rc) return rc; }
    json skipped = json::array();
    if (!ni) std::cerr << "┌ OpenClaw onboarding (openclaw-cpp)\n│ Agents can run tools on this machine; only continue if you trust the models you configure.\n";
    if (!ni && !a->has("--accept-risk") && !Confirm("I understand the risks. Continue?", true)) { std::cerr << "└ Onboarding cancelled.\n"; return 1; }
    const std::string flow = a->get("--flow", ni ? "quickstart" : Choose("Onboarding flow", {{"quickstart", "sensible defaults"}, {"advanced", "configure every section"}, {"manual", "same as advanced"}}, "quickstart"));
    if (!std::set<std::string>{"quickstart", "advanced", "manual", "import"}.count(flow)) { std::cerr << "Invalid --flow (use quickstart|advanced|manual|import).\n"; return 1; }
    const std::string mode = a->get("--mode", ni || flow == "quickstart" ? "local" : Choose("Where will the Gateway run?", {{"local", "this machine"}, {"remote", "connect to an existing Gateway"}}, "local"));
    if (mode != "local" && mode != "remote") { std::cerr << "Invalid --mode (use local|remote).\n"; return 1; }
    json& raw = c.raw;
    Set(raw, "gateway.mode", mode);
    std::string ws = a->get("--workspace");
    if (ws.empty()) { auto cur = Get(raw, "agents.defaults.workspace"); ws = cur.is_string() ? cur.get<std::string>() : DefaultWorkspace(); if (!ni && flow != "quickstart") ws = Ask("Workspace directory", ws); }
    ws = ExpandHome(ws);
    Set(raw, "agents.defaults.workspace", ws);
    if (a->has("--skip-bootstrap")) Set(raw, "agents.defaults.skipBootstrap", true);
    if (mode == "remote") {
        std::string url = a->get("--remote-url"); if (url.empty() && !ni) url = Ask("Remote Gateway URL", "ws://127.0.0.1:18789");
        if (url.empty()) { std::cerr << "--remote-url is required for --mode remote.\n"; return 1; }
        Set(raw, "gateway.remote.url", url);
        std::string t = a->get("--remote-token"); if (t.empty() && !ni) t = Ask("Remote Gateway token (optional)", "", true);
        if (!t.empty()) Set(raw, "gateway.remote.token", t);
        if (a->has("--remote-password")) Set(raw, "gateway.remote.password", a->get("--remote-password"));
    } else {
        std::string choice = a->get("--auth-choice"), key, prov;
        for (auto& [p, _] : kProviders) if (a->has("--" + p + "-api-key")) { prov = p; key = a->get("--" + p + "-api-key"); }
        if (a->has("--custom-base-url")) prov = "custom";
        if (!choice.empty() && prov.empty()) {
            if (choice == "skip") prov = "skip";
            else if (choice == "custom-api-key") prov = "custom";
            else if (choice == "token") { prov = a->get("--token-provider", "anthropic"); key = a->get("--token"); }
            else { auto p = choice.substr(0, choice.find('-')); if (ProviderDefaults(p).first.empty()) { std::cerr << "Unknown --auth-choice " << choice << '\n'; return 1; } prov = p; }
        }
        if (prov == "custom") {
            if (!a->has("--custom-base-url") || !a->has("--custom-model-id")) { std::cerr << "--auth-choice custom-api-key needs --custom-base-url and --custom-model-id.\n"; return 1; }
            std::string id = a->get("--custom-provider-id");
            if (id.empty()) { std::string u = a->get("--custom-base-url"); auto s = u.find("://"); id = "custom-" + u.substr(s == std::string::npos ? 0 : s + 3); for (auto& ch : id) if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '-'; while (!id.empty() && id.back() == '-') id.pop_back(); }
            ApplyModel(raw, id, a->get("--custom-api-key"), a->get("--custom-model-id"), a->get("--custom-base-url"));
            if (a->get("--custom-compatibility") == "anthropic") Set(raw, "models.providers." + id + ".api", "anthropic-messages");
        } else if (!prov.empty() && prov != "skip") {
            if (key.empty() && ni) { std::cerr << "Missing API key for " << prov << " (pass --" << prov << "-api-key).\n"; return 1; }
            if (key.empty()) key = Ask(prov + " API key", "", true);
            ApplyModel(raw, prov, key, "", "");
        } else if (prov.empty() && !ni) {
            SectionModel(raw);
        } else {
            skipped.push_back("model");
        }
        if (a->has("--gateway-port")) { int p = std::atoi(a->get("--gateway-port").c_str()); if (p < 1 || p > 65535) { std::cerr << "Invalid --gateway-port\n"; return 1; } Set(raw, "gateway.port", p); }
        else if (Get(raw, "gateway.port").is_null()) Set(raw, "gateway.port", 18789);
        std::string bind = a->get("--gateway-bind", Get(raw, "gateway.bind").is_string() ? Get(raw, "gateway.bind").get<std::string>() : "loopback");
        if (!std::set<std::string>{"loopback", "tailnet", "lan", "auto", "custom"}.count(bind)) { std::cerr << "Invalid --gateway-bind\n"; return 1; }
        Set(raw, "gateway.bind", bind);
        std::string am = a->get("--gateway-auth", Get(raw, "gateway.auth.mode").is_string() ? Get(raw, "gateway.auth.mode").get<std::string>() : "token");
        if (am != "token" && am != "password") { std::cerr << "Invalid --gateway-auth (token|password)\n"; return 1; }
        if (!ni && flow != "quickstart" && !a->has("--gateway-port")) SectionGateway(raw);
        else {
            Set(raw, "gateway.auth.mode", am);
            if (am == "token") { auto t = a->get("--gateway-token"); if (t.empty()) { auto cur = Get(raw, "gateway.auth.token"); t = cur.is_string() ? cur.get<std::string>() : RandomToken(); } Set(raw, "gateway.auth.token", t); }
            else { if (!a->has("--gateway-password")) { std::cerr << "--gateway-auth password needs --gateway-password\n"; return 1; } Set(raw, "gateway.auth.password", a->get("--gateway-password")); }
        }
        if (a->has("--tailscale")) Set(raw, "gateway.tailscale.mode", a->get("--tailscale"));
        if (!a->has("--skip-channels") && !ni && flow != "quickstart") SectionChannels(raw); else skipped.push_back("channels");
        if (!a->has("--skip-search") && !ni && flow != "quickstart") SectionWeb(raw); else skipped.push_back("search");
        if (a->has("--install-daemon")) std::cerr << "note: --install-daemon is not supported by openclaw-cpp; run `openclaw-cpp gateway` under a supervisor.\n";
        skipped.push_back("daemon");
    }
    if (a->has("--agent-name")) {
        std::string name = a->get("--agent-name"), id;
        for (char ch : name) id.push_back(std::isalnum(static_cast<unsigned char>(ch)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch))) : '-');
        json list = json::array({{{"id", id.empty() ? "main" : id}, {"name", name}, {"default", true}, {"workspace", ws}}});
        if (a->has("--team"))
            for (const char* r : {"researcher", "writer", "reviewer"}) list.push_back({{"id", std::string(r)}, {"name", std::string(r)}, {"workspace", (fs::path(ws) / "team" / r).string()}});
        Set(raw, "agents.list", list);
    }
    auto wsr = EnsureWorkspace(ws, a->has("--skip-bootstrap") || Get(raw, "agents.defaults.skipBootstrap") == true);
    EnsureSessions();
    SaveCfg(c);
    json health = nullptr;
    if (!a->has("--skip-health") && mode == "local") health = SectionHealth(raw); else skipped.push_back("health");
    json summary{{"ok", true}, {"configPath", c.path.string()}, {"mode", mode}, {"flow", flow}, {"workspace", wsr},
                 {"model", Get(raw, "agents.defaults.model.primary")}, {"gateway", {{"port", Get(raw, "gateway.port")}, {"bind", Get(raw, "gateway.bind")}, {"auth", Get(raw, "gateway.auth.mode")}}},
                 {"skipped", skipped}, {"health", health}};
    if (mode == "remote") summary["gateway"] = {{"remoteUrl", Get(raw, "gateway.remote.url")}};
    if (jo) { std::cout << summary.dump(2) << '\n'; return 0; }
    std::cerr << "└ Onboarding complete. Wrote " << Short(c.path) << "\n  workspace: " << Short(ws) << '\n';
    if (mode == "local" && Get(raw, "gateway.auth.mode") == "token" && !a->has("--suppress-gateway-token-output"))
        std::cerr << "  gateway token: " << Get(raw, "gateway.auth.token").get<std::string>() << "\n  next: openclaw-cpp gateway   then   openclaw-cpp tui\n";
    return 0;
}

int CmdSetup(int argc, char** argv, const std::string& name) {
    static const std::vector<std::string> kFlags = {"--message=", "--yes", "--json", "--workspace=", "--agent-name=", "--team", "--wizard", "--baseline",
                                                    "--skip-bootstrap", "--reset", "--reset-scope=", "--non-interactive", "--accept-risk"};
    auto a = Parse(argc, argv, 2, kFlags, name, {{"-m", "--message"}});
    if (!a) return 1;
    if (a->has("--wizard") || a->has("--reset") || a->has("--non-interactive") || a->has("--agent-name") || a->has("--team")) return CmdOnboard(argc, argv, 2);
    const bool baseline = a->has("--baseline") || a->has("--workspace") || a->has("--skip-bootstrap");
    if (baseline) {
        for (auto& [k, _] : a->opt) if (k != "--baseline" && k != "--workspace" && k != "--skip-bootstrap" && k != "--json") { std::cerr << "--baseline cannot be combined with: " << k << ".\n"; return 1; }
        Cfg c = LoadCfg();
        if (c.exists && !c.valid) {
            if (a->has("--json")) std::cout << json{{"ok", false}, {"error", "OpenClaw config is invalid: " + Short(c.path)}, {"issues", json::array({c.error})}}.dump(2) << '\n';
            std::cerr << "Config invalid at " << Short(c.path) << ". Run `openclaw-cpp config validate`, fix it, then re-run setup.\n";
            return 1;
        }
        auto cur = Get(c.raw, "agents.defaults.workspace");
        const std::string configured = cur.is_string() ? cur.get<std::string>() : "";
        const std::string want = a->has("--workspace") ? ExpandHome(a->get("--workspace")) : "";
        const std::string ws = !want.empty() ? want : !configured.empty() ? configured : DefaultWorkspace();
        json changed = json::array();
        if (!c.exists || (!want.empty() && configured != ws)) { Set(c.raw, "agents.defaults.workspace", ws); changed.push_back("agents.defaults.workspace"); }
        if (Get(c.raw, "gateway.mode").is_null()) { Set(c.raw, "gateway.mode", "local"); changed.push_back("gateway.mode"); }
        const bool skip = a->has("--skip-bootstrap") || Get(c.raw, "agents.defaults.skipBootstrap") == true;
        if (a->has("--skip-bootstrap") && Get(c.raw, "agents.defaults.skipBootstrap") != true) { Set(c.raw, "agents.defaults.skipBootstrap", true); changed.push_back("agents.defaults.skipBootstrap"); }
        const bool fresh = !c.exists;
        if (!changed.empty()) SaveCfg(c);
        auto wsr = EnsureWorkspace(ws, skip);
        EnsureSessions();
        if (a->has("--json")) { std::cout << json{{"ok", true}, {"configPath", c.path.string()}, {"created", fresh}, {"changed", changed}, {"workspace", wsr}, {"sessions", (StateDir() / "agents/main/sessions").string()}}.dump(2) << '\n'; return 0; }
        std::cout << (changed.empty() ? "Config OK: " : fresh ? "Wrote " : "Updated ") << Short(c.path) << "\nWorkspace OK: " << Short(ws) << "\nSessions OK: " << Short(StateDir() / "agents/main/sessions") << '\n';
        return 0;
    }
    Cfg c = LoadCfg();
    const bool incomplete = !c.exists || Get(c.raw, "agents.defaults.workspace").is_null();
    if (a->has("--message")) return kDelegateChat;
    if (incomplete) { std::cerr << "Setup is incomplete — starting onboarding.\n"; return CmdOnboard(argc, argv, 2); }
    if (a->has("--json")) {
        json chans = json::array();
        if (c.raw.contains("channels") && c.raw["channels"].is_object()) for (auto& [k, _] : c.raw["channels"].items()) chans.push_back(k);
        std::cout << json{{"ok", true}, {"configPath", c.path.string()}, {"workspace", Get(c.raw, "agents.defaults.workspace")}, {"model", Get(c.raw, "agents.defaults.model.primary")},
                          {"gateway", {{"mode", Get(c.raw, "gateway.mode")}, {"port", Get(c.raw, "gateway.port")}}}, {"channels", chans}}.dump(2) << '\n';
        return 0;
    }
    return kDelegateChat;
}

int CmdConfigure(int argc, char** argv) {
    auto a = Parse(argc, argv, 2, {"--section="}, "configure");
    if (!a) return 1;
    Cfg c = LoadCfg();
    if (c.exists && !c.valid) { std::cerr << "Config invalid at " << Short(c.path) << ": " << c.error << '\n'; return 1; }
    std::vector<std::string> secs = a->multi_section;
    for (auto& s : secs) {
        bool known = false; for (auto& [k, _] : Sections()) known |= k == s;
        if (!known) { std::string all; for (auto& [k, _] : Sections()) all += (all.empty() ? "" : ", ") + k; std::cerr << "Invalid --section: " << s << ". Expected one of: " << all << ".\n"; return 1; }
    }
    std::cerr << "┌ OpenClaw configure\n";
    if (secs.empty()) {
        for (int guard = 0; guard < 16 && !g_eof; ++guard) {
            std::vector<std::pair<std::string, std::string>> o; for (auto& [k, _] : Sections()) o.push_back({k, ""});
            o.push_back({"continue", "save and exit"});
            auto s = Choose("Select section to configure", o, "continue");
            if (s == "continue") break;
            secs.push_back(s);
            for (auto& [k, fn] : Sections()) if (k == s) fn(c.raw);
            SaveCfg(c);
        }
    } else {
        for (auto& s : secs) for (auto& [k, fn] : Sections()) if (k == s) fn(c.raw);
        SaveCfg(c);
    }
    std::cerr << "└ Configure complete (" << Join(secs, ", ") << "). Wrote " << Short(c.path) << '\n';
    return 0;
}

// ---------- migrate (claude provider; upstream extensions/migrate-claude item mapping) ----------
std::string Slug(const std::string& s) {
    std::string o; for (char ch : s) o.push_back(std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-' ? ch : '-');
    while (!o.empty() && (o.front() == '-' || o.front() == '.' || o.front() == '_')) o.erase(o.begin());
    while (!o.empty() && (o.back() == '-' || o.back() == '.' || o.back() == '_')) o.pop_back();
    return o.substr(0, 72);
}
std::string ReadFile(const fs::path& p) { std::ifstream in(p, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
json ReadJsonObj(const fs::path& p) { std::error_code ec; if (p.empty() || !fs::exists(p, ec)) return json::object(); try { auto j = json::parse(ReadFile(p)); return j.is_object() ? j : json::object(); } catch (...) { return json::object(); } }

struct Item { std::string id, kind, action, source, target, status = "planned", reason; json value; bool sensitive = false; };
struct ClaudeSrc { fs::path home, project; };
ClaudeSrc DiscoverClaude(const std::string& from) {
    ClaudeSrc s;
    const char* cd = std::getenv("CLAUDE_CONFIG_DIR");
    s.home = cd && *cd ? fs::path(cd) : Home() / ".claude";
    s.project = fs::current_path();
    if (!from.empty()) {
        fs::path f = ExpandHome(from);
        std::error_code ec;
        if (f.filename() == ".claude" || fs::exists(f / "settings.json", ec)) s.home = f, s.project = fs::path();
        else s.project = f;
    }
    return s;
}
std::vector<Item> PlanClaude(const ClaudeSrc& src, const fs::path& ws, const json& cfg, bool overwrite, bool secrets) {
    std::vector<Item> items; std::error_code ec;
    auto instr = [&](const std::string& id, const fs::path& p, const std::string& target, const std::string& label) {
        if (p.empty() || !fs::is_regular_file(p, ec)) return;
        Item it{id, "instructions", "append", p.string(), (ws / target).string()};
        it.reason = label;
        if (ReadFile(ws / target).find("<!-- openclaw-migrate:" + id + " -->") != std::string::npos) it.status = "skipped", it.reason = "already imported";
        items.push_back(it);
    };
    if (!src.project.empty()) { instr("workspace:CLAUDE.md", src.project / "CLAUDE.md", "AGENTS.md", "project CLAUDE.md"); instr("workspace:.claude/CLAUDE.md", src.project / ".claude/CLAUDE.md", "AGENTS.md", "project .claude/CLAUDE.md"); }
    instr("memory:user-CLAUDE.md", src.home / "CLAUDE.md", "USER.md", "user ~/.claude/CLAUDE.md");
    auto skills = [&](const fs::path& dir, const std::string& scope) {
        if (!fs::is_directory(dir, ec)) return;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_directory() || e.is_symlink() || !fs::exists(e.path() / "SKILL.md", ec)) continue;
            auto name = Slug(e.path().filename().string()); if (name.empty()) continue;
            Item it{"skill:" + scope + ":" + name, "skill", "copy", e.path().string(), (ws / "skills" / name).string()};
            if (fs::exists(it.target, ec)) { it.status = overwrite ? "planned" : "conflict"; it.reason = overwrite ? "will overwrite after backup" : "target exists (use --overwrite)"; }
            items.push_back(it);
        }
    };
    skills(src.home / "skills", "user");
    if (!src.project.empty()) skills(src.project / ".claude/skills", "project");
    auto commands = [&](const fs::path& dir, const std::string& scope) {
        if (!fs::is_directory(dir, ec)) return;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::none, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file() || it->path().extension() != ".md") continue;
            auto rel = fs::relative(it->path(), dir);
            std::string ns = rel.parent_path().string(); std::replace(ns.begin(), ns.end(), '/', '-');
            auto name = Slug("claude-command" + (ns.empty() ? "" : "-" + Slug(ns)) + "-" + Slug(rel.stem().string()));
            Item x{"command:" + scope + ":" + rel.string(), "skill", "create", it->path().string(), (ws / "skills" / name).string()};
            x.reason = scope + " Claude command " + rel.string();
            if (fs::exists(x.target, ec)) { x.status = overwrite ? "planned" : "conflict"; if (!overwrite) x.reason = "target exists (use --overwrite)"; }
            items.push_back(x);
        }
    };
    commands(src.home / "commands", "user");
    if (!src.project.empty()) commands(src.project / ".claude/commands", "project");
    auto mcp = [&](const fs::path& file, const json& servers) {
        if (!servers.is_object()) return;
        for (auto& [name, spec] : servers.items()) {
            if (!spec.is_object()) continue;
            Item it{"mcp:" + Slug(name), "mcp-server", "config", file.string(), "mcp.servers." + Slug(name)};
            json v = spec;
            if (v.contains("env") && v["env"].is_object() && !v["env"].empty()) {
                it.sensitive = true;
                if (!secrets) { std::string keys; for (auto& [k, _] : v["env"].items()) keys += (keys.empty() ? "" : ",") + k; v.erase("env"); it.reason = "env omitted (" + keys + "); set them in the environment or re-run with --include-secrets"; }
            }
            it.value = v;
            if (auto cur = Get(cfg, it.target); !cur.is_null()) { it.status = cur == v ? "skipped" : overwrite ? "planned" : "conflict"; it.reason = cur == v ? "already configured" : overwrite ? "will overwrite" : "mcp.servers entry exists (use --overwrite)"; }
            items.push_back(it);
        }
    };
    if (!src.project.empty()) { auto pm = ReadJsonObj(src.project / ".mcp.json"); mcp(src.project / ".mcp.json", pm.contains("mcpServers") ? pm["mcpServers"] : pm); }
    const fs::path cjp = src.home.parent_path() / ".claude.json";
    auto cj = ReadJsonObj(cjp);
    mcp(cjp, cj.value("mcpServers", json()));
    if (!src.project.empty() && cj.contains("projects") && cj["projects"].is_object() && cj["projects"].contains(src.project.string()))
        mcp(cjp, cj["projects"][src.project.string()].value("mcpServers", json()));
    for (const auto& p : {src.home / "settings.json", src.project.empty() ? fs::path() : src.project / ".claude/settings.json"}) {
        auto s = ReadJsonObj(p);
        for (const char* k : {"hooks", "permissions"})
            if (s.contains(k)) items.push_back({std::string("manual:") + k + ":" + p.string(), "manual", "review", p.string(), "", "manual", std::string(k) + " are not activated automatically; review them by hand"});
    }
    return items;
}
std::string Backup(const fs::path& out_arg, const fs::path& cfgp, const fs::path& ws, std::string* err) {
    fs::path out = out_arg.empty() ? StateDir() / "backups" : fs::path(ExpandHome(out_arg.string()));
    std::error_code ec;
    if (out.extension() != ".gz") { fs::create_directories(out, ec); out /= "pre-migrate-" + Stamp() + ".tar.gz"; }
    else fs::create_directories(out.parent_path(), ec);
    std::string cmd = "tar -czf '" + out.string() + "'";
    bool any = false;
    for (const auto& p : {cfgp, ws}) if (fs::exists(p, ec)) { cmd += " -C '" + p.parent_path().string() + "' '" + p.filename().string() + "'"; any = true; }
    if (!any) return "(nothing to back up)";
    if (std::system((cmd + " 2>/dev/null").c_str()) != 0) { *err = "backup failed: " + cmd; return ""; }
    return out.string();
}
json ItemJson(const Item& i) {
    json j{{"id", i.id}, {"kind", i.kind}, {"action", i.action}, {"source", i.source}, {"status", i.status}};
    if (!i.target.empty()) j["target"] = i.target;
    if (!i.reason.empty()) j["reason"] = i.reason;
    if (i.sensitive) j["sensitive"] = true;
    return j;
}
void PrintPlan(const std::vector<Item>& items, const std::string& title) {
    std::cout << title << '\n';
    if (items.empty()) std::cout << "  (nothing found to migrate)\n";
    for (auto& i : items) std::cout << "  [" << i.status << "] " << i.kind << "  " << Short(i.source) << (i.target.empty() ? "" : "  ->  " + (i.kind == "mcp-server" ? i.target : Short(i.target))) << (i.reason.empty() ? "" : "  (" + i.reason + ")") << '\n';
}
int CmdMigrate(int argc, char** argv) {
    static const std::vector<std::string> kFlags = {"--from=", "--agent=", "--include-secrets", "--no-auth-credentials", "--overwrite", "--dry-run", "--yes",
                                                    "--backup-output=", "--no-backup", "--force", "--json"};
    auto a = Parse(argc, argv, 2, kFlags, "migrate");
    if (!a) return 1;
    std::string sub = a->pos.empty() ? "" : a->pos[0];
    const json providers = json::array({{{"id", "claude"}, {"label", "Claude Code / Claude Desktop"}, {"supported", true}},
                                        {{"id", "hermes"}, {"label", "Hermes"}, {"supported", false}, {"reason", "not implemented in openclaw-cpp"}}});
    if (sub.empty() || sub == "list") {
        if (a->has("--json")) { std::cout << json{{"providers", providers}}.dump(2) << '\n'; return 0; }
        for (auto& p : providers) std::cout << p["id"].get<std::string>() << "\t" << p["label"].get<std::string>() << (p["supported"].get<bool>() ? "" : "  (not implemented)") << '\n';
        return 0;
    }
    std::string mode = "auto", provider = sub;
    if (sub == "plan" || sub == "apply") { mode = sub; if (a->pos.size() < 2) { std::cerr << "error: missing required argument 'provider'\n"; return 1; } provider = a->pos[1]; }
    if (provider == "hermes") { std::cerr << "migrate hermes: not implemented in openclaw-cpp (only `claude`).\n"; return 1; }
    if (provider != "claude") { std::cerr << "Unknown migration provider: " << provider << " (try `openclaw-cpp migrate list`).\n"; return 1; }
    if (a->has("--no-backup") && !a->has("--force")) { std::cerr << "--no-backup is dangerous; re-run with --force to confirm.\n"; return 1; }
    Cfg c = LoadCfg();
    if (c.exists && !c.valid) { std::cerr << "Config invalid at " << Short(c.path) << ": " << c.error << "\n"; return 1; }
    auto wsj = Get(c.raw, "agents.defaults.workspace");
    const fs::path ws = wsj.is_string() ? fs::path(ExpandHome(wsj.get<std::string>())) : fs::path(DefaultWorkspace());
    auto src = DiscoverClaude(a->get("--from"));
    auto items = PlanClaude(src, ws, c.raw, a->has("--overwrite"), a->has("--include-secrets"));
    const bool dry = mode == "plan" || a->has("--dry-run");
    size_t conflicts = 0, planned = 0; for (auto& i : items) { conflicts += i.status == "conflict"; planned += i.status == "planned"; }
    json out{{"provider", "claude"}, {"source", {{"home", src.home.string()}, {"project", src.project.string()}}}, {"workspace", ws.string()}};
    auto all = [&] { json arr = json::array(); for (auto& i : items) arr.push_back(ItemJson(i)); return arr; };
    if (dry) {
        out["mode"] = "plan"; out["items"] = all(); out["summary"] = {{"planned", planned}, {"conflicts", conflicts}, {"total", items.size()}};
        if (a->has("--json")) std::cout << out.dump(2) << '\n'; else PrintPlan(items, "Migration plan (claude -> " + Short(ws) + "):");
        if (conflicts && !a->has("--json")) std::cout << "Conflicts were found. Re-run with --overwrite to replace conflicting targets after item-level backups.\n";
        return 0;
    }
    if (!a->has("--json")) PrintPlan(items, "Migration plan (claude -> " + Short(ws) + "):");
    if (planned == 0) {
        if (a->has("--json")) { out["mode"] = "apply"; out["items"] = all(); std::cout << out.dump(2) << '\n'; } else std::cout << "Nothing to apply.\n";
        return conflicts ? 1 : 0;
    }
    if (!a->has("--yes") && !Confirm("Apply " + std::to_string(planned) + " item(s)?", false)) { std::cerr << "Migration cancelled.\n"; return 1; }
    std::string err, backup = "skipped (--no-backup)";
    if (!a->has("--no-backup")) { backup = Backup(a->get("--backup-output"), c.path, ws, &err); if (backup.empty()) { std::cerr << err << '\n'; return 1; } }
    std::error_code ec; fs::create_directories(ws, ec);
    bool cfg_changed = false;
    for (auto& i : items) {
        if (i.status != "planned") continue;
        try {
            ec.clear();
            if (i.kind == "instructions") {
                std::ofstream o(i.target, std::ios::app);
                o << "\n\n<!-- openclaw-migrate:" << i.id << " -->\n## Imported from " << i.reason << "\n\n" << ReadFile(i.source) << '\n';
            } else if (i.kind == "skill") {
                if (fs::exists(i.target, ec)) fs::rename(i.target, i.target + ".bak-" + Stamp(), ec);
                fs::create_directories(fs::path(i.target).parent_path(), ec);
                if (i.action == "copy") fs::copy(i.source, i.target, fs::copy_options::recursive | fs::copy_options::skip_symlinks, ec);
                else {
                    fs::create_directories(i.target, ec);
                    auto body = ReadFile(i.source);
                    std::string desc = body.substr(0, body.find('\n')); if (desc.size() > 120) desc.resize(120);
                    for (auto& ch : desc) if (ch == '"') ch = '\'';
                    std::ofstream(fs::path(i.target) / "SKILL.md") << "---\nname: " << fs::path(i.target).filename().string() << "\ndescription: \"" << (desc.empty() ? "Imported Claude command" : desc) << "\"\n---\n\n" << body;
                }
                if (ec) throw std::runtime_error(ec.message());
            } else if (i.kind == "mcp-server") {
                Set(c.raw, i.target, i.value); cfg_changed = true;
            }
            i.status = "applied";
        } catch (const std::exception& e) { i.status = "failed"; i.reason = e.what(); }
    }
    if (cfg_changed) SaveCfg(c);
    size_t applied = 0, failed = 0; for (auto& i : items) { applied += i.status == "applied"; failed += i.status == "failed"; }
    if (a->has("--json")) {
        out["mode"] = "apply"; out["items"] = all(); out["backup"] = backup; out["summary"] = {{"applied", applied}, {"failed", failed}, {"conflicts", conflicts}};
        std::cout << out.dump(2) << '\n';
    } else std::cout << "Applied " << applied << " item(s)" << (failed ? ", " + std::to_string(failed) + " failed" : "") << ". Backup: " << backup << '\n';
    return failed ? 1 : 0;
}

// ---------- tui (line mode over the gateway RPC) ----------
int CmdTui(int argc, char** argv) {
    static const std::vector<std::string> kFlags = {"--url=", "--token=", "--password=", "--tls-fingerprint=", "--local", "--session=", "--deliver",
                                                    "--thinking=", "--message=", "--timeout-ms=", "--history-limit=", "--plain", "--fullscreen"};
    auto a = Parse(argc, argv, 2, kFlags, "tui");
    if (!a) return 1;
    if (a->has("--local")) return kDelegateChat;
    Cfg c = LoadCfg();
    const std::string base = GatewayBase(c.raw, a->get("--url"));
    std::string token = a->get("--token", a->get("--password"));
    if (token.empty()) if (const char* t = std::getenv("OPENCLAW_GATEWAY_TOKEN")) token = t;
    if (token.empty()) { auto t = Get(c.raw, a->has("--url") || Get(c.raw, "gateway.mode") == "remote" ? "gateway.remote.token" : "gateway.auth.token"); if (t.is_string()) token = t.get<std::string>(); }
    std::string session = a->get("--session", a->pos.empty() ? "main" : a->pos[0]);
    if (session.find(':') == std::string::npos) session = "agent:main:" + session;
    const long timeout = std::max<long>(5, std::atol(a->get("--timeout-ms", "600000").c_str()) / 1000);
    const int hist = std::atoi(a->get("--history-limit", "200").c_str());
    std::string err;
    auto health = Rpc(base, token, "health", json::object(), 5, &err);
    if (health.is_null()) { std::cerr << "tui: " << err << "; start it with `openclaw-cpp gateway`\n"; return 1; }
    std::cerr << "openclaw tui - " << base << " - session " << session << "  (/help for commands)\n";
    auto text_of = [](const json& m) {
        std::string text;
        if (m.contains("content") && m["content"].is_string()) text = m["content"];
        else if (m.contains("content") && m["content"].is_array()) { for (auto& p : m["content"]) if (p.is_object() && p.value("type", "") == "text") text += p.value("text", ""); }
        else text = m.value("text", "");
        return text;
    };
    auto show_history = [&] {
        std::string e;
        auto h = Rpc(base, token, "chat.history", {{"sessionKey", session}, {"limit", hist}}, 15, &e);
        if (h.is_null()) return;
        for (auto& m : h.value("messages", json::array())) {
            std::string role = m.value("role", "");
            if (role == "user" || role == "assistant") std::cout << (role == "user" ? "you: " : "agent: ") << text_of(m) << '\n';
        }
    };
    auto send = [&](const std::string& text) -> bool {
        json p{{"sessionKey", session}, {"message", text}};
        if (a->has("--thinking")) p["thinking"] = a->get("--thinking");
        if (a->has("--deliver")) p["deliver"] = true;
        std::string e;
        auto r = Rpc(base, token, "chat.send", p, timeout, &e);
        if (r.is_null()) { std::cerr << "error: " << e << '\n'; return false; }
        std::cout << "agent: " << r.value("text", r.dump()) << '\n' << std::flush;
        return true;
    };
    const bool full = a->has("--fullscreen") || (!a->has("--plain") && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
                                                  std::getenv("TERM") && std::string(std::getenv("TERM")) != "dumb");
    if (full) {
        TuiBackend tb;
        tb.rpc = [&](const std::string& m, const json& p, long t, std::string* e) { return Rpc(base, token, m, p, t, e); };
        tb.base = base; tb.session = session; tb.timeout_s = timeout; tb.history_limit = hist;
        if (a->has("--thinking")) tb.send_extra["thinking"] = a->get("--thinking");
        if (a->has("--deliver")) tb.send_extra["deliver"] = true;
        tb.initial_message = a->get("--message");
        const int rc = RunFullscreenTui(tb);
        if (rc >= 0) return rc;  // -1: not a terminal after all → line mode
    }
    show_history();
    if (a->has("--message") && !send(a->get("--message"))) return 1;
    std::string line;
    while (true) {
        if (isatty(STDIN_FILENO)) std::cerr << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line == "/exit" || line == "/quit") break;
        if (line == "/help") { std::cerr << "/session <key>  /history  /new  /abort  /status  /exit\n"; continue; }
        if (line == "/history") { show_history(); continue; }
        if (line == "/status") { std::string e; auto s = Rpc(base, token, "status", json::object(), 10, &e); std::cout << (s.is_null() ? e : s.dump(2)) << '\n'; continue; }
        if (line == "/abort") { std::string e; Rpc(base, token, "chat.abort", {{"sessionKey", session}}, 10, &e); continue; }
        if (line == "/new" || line == "/reset") { std::string e; auto r = Rpc(base, token, "sessions.reset", {{"key", session}}, 10, &e); std::cerr << (r.is_null() ? "reset failed: " + e : "session reset") << '\n'; continue; }
        if (line.rfind("/session ", 0) == 0) { session = line.substr(9); if (session.find(':') == std::string::npos) session = "agent:main:" + session; std::cerr << "session " << session << '\n'; show_history(); continue; }
        send(line);
    }
    return 0;
}

TreeT Tree() {
    TreeT t;
    for (auto& c : NativeCommands()) t[c];
    for (auto& [cmd, subs] : UpstreamCommandTree().items()) {
        if (cmd == "crestodian") continue;
        auto& e = t[cmd];
        for (auto& [s, d] : subs.items()) {
            if (s.empty()) { for (auto& f : d["flags"]) { auto x = f.get<std::string>(); if (!x.empty() && x.back() == '=') x.pop_back(); e.second.push_back(x); } }
            else e.first.push_back(s);
        }
    }
    return t;
}

}  // namespace

const json& UpstreamCommandTree() {
    static const json k = json::parse(
#include "upstream_cli_tree.inc"
    );
    return k;
}

std::string CompletionScript(const std::string& shell, const std::string& bin) {
    auto t = Tree();
    std::vector<std::string> cmds; for (auto& [k, _] : t) cmds.push_back(k);
    std::string fn = "_"; for (char ch : bin) fn.push_back(std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
    auto words = [](const std::pair<std::vector<std::string>, std::vector<std::string>>& v) { auto w = v.first; w.insert(w.end(), v.second.begin(), v.second.end()); return w; };
    std::ostringstream o;
    if (shell == "bash") {
        o << "# bash completion for " << bin << " (generated)\n" << fn << "_completion() {\n  local cur cmd\n  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n  cmd=\"${COMP_WORDS[1]}\"\n"
          << "  if [[ $COMP_CWORD -eq 1 ]]; then COMPREPLY=( $(compgen -W \"" << Join(cmds) << "\" -- \"$cur\") ); return; fi\n  case \"$cmd\" in\n";
        for (auto& [k, v] : t) if (!words(v).empty()) o << "    " << k << ") COMPREPLY=( $(compgen -W \"" << Join(words(v)) << "\" -- \"$cur\") ) ;;\n";
        o << "  esac\n}\ncomplete -F " << fn << "_completion " << bin;
    } else if (shell == "zsh") {
        o << "#compdef " << bin << "\n\n" << fn << "() {\n  local -a cmds\n  if (( CURRENT == 2 )); then\n    cmds=(" << Join(cmds) << ")\n    compadd -a cmds\n    return\n  fi\n  case $words[2] in\n";
        for (auto& [k, v] : t) if (!words(v).empty()) o << "    " << k << ") compadd -- " << Join(words(v)) << " ;;\n";
        o << "  esac\n}\n\nif (( $+functions[compdef] )); then compdef " << fn << " " << bin << "; fi";
    } else if (shell == "fish") {
        o << "# fish completion for " << bin << " (generated)\ncomplete -c " << bin << " -f\n";
        for (auto& c : cmds) o << "complete -c " << bin << " -n '__fish_use_subcommand' -a '" << c << "'\n";
        for (auto& [k, v] : t) {
            for (auto& s : v.first) o << "complete -c " << bin << " -n '__fish_seen_subcommand_from " << k << "' -a '" << s << "'\n";
            for (auto& f : v.second) o << "complete -c " << bin << " -n '__fish_seen_subcommand_from " << k << "' -l '" << f.substr(2) << "'\n";
        }
    } else {
        o << "# PowerShell completion for " << bin << " (generated)\nRegister-ArgumentCompleter -Native -CommandName '" << bin << "' -ScriptBlock {\n  param($wordToComplete, $commandAst, $cursorPosition)\n  $tree = @{\n";
        for (auto& [k, v] : t) { o << "    '" << k << "' = @("; bool f = true; for (auto& s : words(v)) { o << (f ? "" : ",") << "'" << s << "'"; f = false; } o << ")\n"; }
        o << "  }\n  $els = @($commandAst.CommandElements | ForEach-Object { $_.ToString() })\n"
          << "  if ($els.Count -le 1 -or ($els.Count -eq 2 -and $wordToComplete -ne '')) { $cands = $tree.Keys } else { $cands = $tree[$els[1]] }\n"
          << "  $cands | Where-Object { $_ -like \"$wordToComplete*\" } | ForEach-Object { [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_) }\n}";
    }
    return o.str();
}

// ---------------------------------------------------------------- claws add / status / remove (install records)
// Install records live in <data>/claws/state.json "installs" (shared with the gateway's claws.removalJournal).
fs::path ClawsDataDir() { const char* e = std::getenv("OPENCLAW_DATA_DIR"); return e && *e ? fs::path(e) : Home() / ".openclaw-cpp" / "data"; }
std::string ReadAll(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
std::string FileDigest(const std::string& content) { return gateway::ClawDigest(json(content)); }
void WriteClawState(const json& st) {  // in place: the gateway binds journal mutations to the file's physical identity
    const auto p = ClawsDataDir() / "claws" / "state.json";
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc); f << st.dump(2);
}
// Minimal YAML front matter reader for CLAW.md: nested "key: value" maps by indentation (scalars only).
json FrontMatter(const std::string& md, std::string* body) {
    json out = json::object();
    if (md.rfind("---\n", 0) != 0) { *body = md; return out; }
    auto end = md.find("\n---", 4);
    if (end == std::string::npos) { *body = md; return out; }
    std::istringstream in(md.substr(4, end - 4));
    *body = md.substr(md.find('\n', end + 1) == std::string::npos ? md.size() : md.find('\n', end + 1) + 1);
    std::vector<std::pair<int, json*>> stack{{-1, &out}};
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(' ') == std::string::npos || line[line.find_first_not_of(' ')] == '#') continue;
        const int ind = static_cast<int>(line.find_first_not_of(' '));
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(ind, colon - ind), v = line.substr(colon + 1);
        v.erase(0, v.find_first_not_of(' ')); while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
        while (stack.size() > 1 && stack.back().first >= ind) stack.pop_back();
        json& parent = *stack.back().second;
        if (v.empty()) { parent[k] = json::object(); stack.push_back({ind, &parent[k]}); continue; }
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
        if (std::all_of(v.begin(), v.end(), ::isdigit)) parent[k] = std::stoll(v); else parent[k] = v;
    }
    return out;
}
struct ClawPackage { json manifest; fs::path base; std::string name, version, source; std::string instructions; std::string error; };
ClawPackage ReadClaw(const std::string& src) {
    ClawPackage pk; pk.source = fs::absolute(src).lexically_normal().string();
    std::error_code ec;
    fs::path mf;
    if (fs::is_directory(src, ec)) {
        pk.base = fs::absolute(src);
        if (fs::exists(pk.base / "package.json")) {
            try {
                auto pj = json::parse(ReadAll(pk.base / "package.json"));
                pk.name = pj.value("name", ""); pk.version = pj.value("version", "");
                if (pj.contains("openclaw") && pj["openclaw"].is_object()) mf = pk.base / pj["openclaw"].value("claw", "CLAW.md");
            } catch (...) { pk.error = "package.json is not valid JSON"; return pk; }
        }
        if (mf.empty()) mf = fs::exists(pk.base / "CLAW.md") ? pk.base / "CLAW.md" : pk.base / "claw.json";
    } else { mf = fs::absolute(src); pk.base = mf.parent_path(); }
    if (!fs::is_regular_file(mf, ec)) { pk.error = "no Claw manifest at " + mf.string(); return pk; }
    const auto raw = ReadAll(mf);
    if (raw.size() > 256 * 1024) { pk.error = "Claw manifest exceeds 256 KiB"; return pk; }
    if (mf.extension() == ".md") pk.manifest = FrontMatter(raw, &pk.instructions);
    else try { pk.manifest = json::parse(raw); } catch (const std::exception& e) { pk.error = std::string("invalid manifest JSON: ") + e.what(); return pk; }
    if (pk.manifest.value("schemaVersion", 0) != 1) { pk.error = "unsupported Claw schemaVersion (expected 1)"; return pk; }
    if (!pk.manifest.contains("agent") || !pk.manifest["agent"].is_object() || pk.manifest["agent"].value("id", "").empty()) { pk.error = "manifest agent.id is required"; return pk; }
    if (pk.name.empty()) pk.name = pk.manifest["agent"].value("id", "");
    return pk;
}
bool InsideDir(const fs::path& base, const fs::path& p) {
    auto b = fs::weakly_canonical(base).string(), q = fs::weakly_canonical(p).string();
    return q.size() > b.size() && q.rfind(b + "/", 0) == 0;
}
bool AgentConfigured(const json& raw, const std::string& id) {
    auto l = Get(raw, "agents.list");
    if (l.is_array()) for (auto& a : l) if (a.is_object() && a.value("id", "") == id) return true;
    auto e = Get(raw, "agents.entries");
    return e.is_object() && e.contains(id);
}
void PrintPlan(const json& plan, bool as_json) {
    if (as_json) { std::cout << plan.dump(2) << "\n"; return; }
    for (auto& a : plan["actions"]) std::cout << "  - " << a.get<std::string>() << "\n";
    std::cout << "plan integrity: " << plan["planIntegrity"].get<std::string>() << "\n";
}

int CmdClawsAdd(int argc, char** argv) {
    std::string src, agent_override, ws_override, integrity; bool dry = false, yes = false, as_json = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--dry-run") dry = true; else if (a == "--yes") yes = true; else if (a == "--json") as_json = true;
        else if (a == "--plan-integrity") integrity = next(); else if (a == "--agent-id") agent_override = next();
        else if (a == "--workspace") ws_override = next();
        else if (a.rfind("--", 0) == 0) { std::cerr << "unknown option '" << a << "'\n"; return 2; }
        else if (src.empty()) src = a; else { std::cerr << "unexpected argument '" << a << "'\n"; return 2; }
    }
    if (src.empty()) { std::cerr << "usage: openclaw-cpp claws add <source> [--dry-run] [--yes] [--plan-integrity D] [--agent-id ID] [--workspace PATH] [--json]\n"; return 2; }
    auto pk = ReadClaw(src);
    if (!pk.error.empty()) { std::cerr << "claws add: " << pk.error << "\n"; return 1; }
    const std::string id = agent_override.empty() ? pk.manifest["agent"].value("id", "") : agent_override;
    static const std::regex id_re("^[a-z0-9][a-z0-9_-]{0,63}$");
    if (!std::regex_match(id, id_re) || id == "main") { std::cerr << "claws add: invalid or reserved agent id '" << id << "'\n"; return 1; }
    auto c = LoadCfg();
    if (!c.valid) { std::cerr << "claws add: config invalid: " << c.error << "\n"; return 1; }
    auto st = gateway::ClawStateRead(ClawsDataDir());
    if (AgentConfigured(c.raw, id) || st["installs"].contains(id)) { std::cerr << "claws add: agent '" << id << "' already exists; pass --agent-id\n"; return 1; }
    const fs::path ws = ws_override.empty() ? StateDir() / ("workspace-" + id) : fs::absolute(ExpandHome(ws_override));
    std::error_code ec;
    if (fs::exists(ws, ec) && !fs::is_empty(ws, ec)) { std::cerr << "claws add: workspace " << ws.string() << " already exists and is not empty\n"; return 1; }
    // Files the Claw owns in the new workspace.
    json files = json::object();
    std::vector<std::string> errors;
    auto add_file = [&](const std::string& rel_src, const std::string& dest) {
        const auto sp = pk.base / rel_src;
        if (!InsideDir(pk.base, sp) || !fs::is_regular_file(sp, ec)) { errors.push_back("missing or escaping source " + rel_src); return; }
        if (fs::path(dest).is_absolute() || dest.find("..") != std::string::npos) { errors.push_back("invalid workspace path " + dest); return; }
        files[dest] = {{"source", rel_src}, {"digest", FileDigest(ReadAll(sp))}};
    };
    const auto& wsm = pk.manifest.contains("workspace") && pk.manifest["workspace"].is_object() ? pk.manifest["workspace"] : json::object();
    if (wsm.contains("bootstrapFiles") && wsm["bootstrapFiles"].is_object())
        for (auto& [name, v] : wsm["bootstrapFiles"].items()) add_file(v.value("source", ""), name);
    if (wsm.contains("files") && wsm["files"].is_array())
        for (auto& f : wsm["files"]) add_file(f.value("source", ""), f.value("path", ""));
    if (!pk.instructions.empty() && !files.contains("AGENTS.md")) files["AGENTS.md"] = {{"inline", true}, {"digest", FileDigest(pk.instructions)}};
    if (!errors.empty()) { for (auto& e : errors) std::cerr << "claws add: " << e << "\n"; return 1; }
    json entry{{"id", id}, {"workspace", ws.string()}};
    if (auto n = pk.manifest["agent"].value("name", ""); !n.empty()) entry["name"] = n;
    if (pk.manifest["agent"].contains("identity")) entry["identity"] = pk.manifest["agent"]["identity"];
    json plan{{"kind", "claws.add"}, {"agentId", id}, {"package", {{"name", pk.name}, {"version", pk.version}, {"source", pk.source}, {"manifestDigest", gateway::ClawDigest(pk.manifest)}}},
              {"workspace", ws.string()}, {"files", files}, {"configEntry", entry}};
    json actions = json::array({"create agent '" + id + "' in agents.list", "create workspace " + ws.string()});
    for (auto& [k, v] : files.items()) actions.push_back("write " + k);
    plan["planIntegrity"] = gateway::ClawDigest(plan);
    plan["actions"] = actions;
    if (dry || !yes) {
        plan["applied"] = false;
        PrintPlan(plan, as_json);
        if (!yes && !dry && !as_json) std::cout << "re-run with --yes --plan-integrity " << plan["planIntegrity"].get<std::string>() << " to apply\n";
        return 0;
    }
    if (!integrity.empty() && integrity != plan["planIntegrity"].get<std::string>()) { std::cerr << "claws add: plan changed since preview (plan integrity mismatch)\n"; return 1; }
    fs::create_directories(ws, ec);
    for (auto& [dest, v] : files.items()) {
        const auto out = ws / dest;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        f << (v.value("inline", false) ? pk.instructions : ReadAll(pk.base / v["source"].get<std::string>()));
    }
    auto list = Get(c.raw, "agents.list");
    if (!list.is_array()) list = json::array();
    list.push_back(entry);
    Set(c.raw, "agents.list", list);
    SaveCfg(c);
    st["installs"][id] = {{"agentId", id}, {"package", plan["package"]}, {"workspace", ws.string()}, {"files", files},
                          {"configEntryDigest", gateway::ClawDigest(entry)}, {"planIntegrity", plan["planIntegrity"]},
                          {"createdAtMs", static_cast<long long>(std::time(nullptr)) * 1000}};
    WriteClawState(st);
    plan["applied"] = true;
    if (as_json) std::cout << plan.dump(2) << "\n"; else std::cout << "added Claw agent '" << id << "' (workspace " << Short(ws) << ")\n";
    return 0;
}

json ClawDrift(const json& raw, const json& inst) {
    json drift = json::array();
    const fs::path ws = inst.value("workspace", "");
    std::error_code ec;
    if (!fs::exists(ws, ec)) drift.push_back({{"kind", "workspace_missing"}, {"path", ws.string()}});
    for (auto& [rel, v] : inst["files"].items()) {
        const auto p = ws / rel;
        if (!fs::exists(p, ec)) drift.push_back({{"kind", "file_missing"}, {"path", rel}});
        else if (FileDigest(ReadAll(p)) != v.value("digest", "")) drift.push_back({{"kind", "file_modified"}, {"path", rel}});
    }
    if (!AgentConfigured(raw, inst.value("agentId", ""))) drift.push_back({{"kind", "config_entry_missing"}});
    return drift;
}
std::string ResolveInstallTarget(const json& st, const std::string& target) {
    if (st["installs"].contains(target)) return target;
    for (auto& [id, v] : st["installs"].items()) if (v["package"].value("name", "") == target) return id;
    return {};
}

int CmdClawsStatus(int argc, char** argv) {
    std::string target; bool as_json = false;
    for (int i = 3; i < argc; ++i) { std::string a = argv[i]; if (a == "--json") as_json = true; else if (a.rfind("--", 0) == 0) { std::cerr << "unknown option '" << a << "'\n"; return 2; } else target = a; }
    const auto c = LoadCfg();
    auto st = gateway::ClawStateRead(ClawsDataDir());
    json out = json::array();
    for (auto& [id, inst] : st["installs"].items()) {
        if (!target.empty() && id != target && inst["package"].value("name", "") != target) continue;
        json j{{"agentId", id}, {"package", inst["package"]}, {"workspace", inst["workspace"]}, {"drift", ClawDrift(c.raw, inst)}};
        if (st["journals"].contains(id)) j["deletionJournal"] = st["journals"][id];
        out.push_back(j);
    }
    if (!target.empty() && out.empty()) { std::cerr << "claws status: no installed Claw '" << target << "'\n"; return 1; }
    if (as_json) { std::cout << json{{"installs", out}}.dump(2) << "\n"; return 0; }
    if (out.empty()) std::cout << "no Claw agents installed\n";
    for (auto& j : out) {
        std::cout << j["agentId"].get<std::string>() << "  " << j["package"].value("name", "") << " " << j["package"].value("version", "")
                  << "  " << (j["drift"].empty() ? "in sync" : std::to_string(j["drift"].size()) + " drift item(s)") << "\n";
        for (auto& d : j["drift"]) std::cout << "    " << d["kind"].get<std::string>() << (d.contains("path") ? " " + d["path"].get<std::string>() : "") << "\n";
    }
    return 0;
}

int CmdClawsRemove(int argc, char** argv) {
    std::string target, integrity, url; bool dry = false, yes = false, as_json = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--dry-run") dry = true; else if (a == "--yes") yes = true; else if (a == "--json") as_json = true;
        else if (a == "--plan-integrity") integrity = next(); else if (a == "--url") url = next();
        else if (a == "--remove-unused" || a == "--force-referenced") {}  // Claws here introduce no shared references
        else if (a == "--remove-referenced") next();
        else if (a.rfind("--", 0) == 0) { std::cerr << "unknown option '" << a << "'\n"; return 2; }
        else target = a;
    }
    if (target.empty()) { std::cerr << "usage: openclaw-cpp claws remove <claw-or-agent> [--dry-run] [--yes] [--plan-integrity D] [--json]\n"; return 2; }
    auto c = LoadCfg();
    const auto data = ClawsDataDir();
    auto st = gateway::ClawStateRead(data);
    const auto id = ResolveInstallTarget(st, target);
    if (id.empty()) { std::cerr << "claws remove: '" << target << "' is not a Claw-created agent (no install record)\n"; return 1; }
    const auto inst = st["installs"][id];
    const auto drift = ClawDrift(c.raw, inst);
    json retained = json::array(), remove_files = json::array();
    for (auto& [rel, v] : inst["files"].items()) {
        bool modified = false;
        for (auto& d : drift) if (d.value("path", "") == rel && d["kind"] == "file_modified") modified = true;
        (modified ? retained : remove_files).push_back(rel);
    }
    json plan{{"kind", "claws.remove"}, {"agentId", id}, {"installDigest", gateway::ClawDigest(inst)}, {"removeFiles", remove_files},
              {"retainedModifiedFiles", retained}, {"removeConfigEntry", AgentConfigured(c.raw, id)}, {"workspace", inst["workspace"]}};
    json actions = json::array();
    if (plan["removeConfigEntry"].get<bool>()) actions.push_back("remove agent '" + id + "' from config");
    for (auto& f : remove_files) actions.push_back("delete " + f.get<std::string>());
    for (auto& f : retained) actions.push_back("keep modified " + f.get<std::string>());
    actions.push_back("delete agent state (sessions, memory) for '" + id + "'");
    plan["planIntegrity"] = gateway::ClawDigest(plan);
    plan["actions"] = actions;
    if (dry || !yes) { plan["applied"] = false; PrintPlan(plan, as_json); return 0; }
    if (!integrity.empty() && integrity != plan["planIntegrity"].get<std::string>()) { std::cerr << "claws remove: plan changed since preview (plan integrity mismatch)\n"; return 1; }
    // 1. Gateway journal: begin (lease + digest guarded). The journal's cleanupPaths are what we may delete.
    const std::string op = "rm-" + RandomToken().substr(0, 16), owner = "cli-" + std::to_string(::getpid());
    const json journal_prev = st["journals"].contains(id) ? st["journals"][id] : json(nullptr);
    json req{{"phase", "begin"}, {"agentId", id}, {"operationId", op}, {"binding", gateway::ClawRemovalBinding(data)},
             {"sourceIdentity", gateway::ClawStateIdentity(data)}, {"lease", {{"scope", "core:agent-deletion"}, {"key", id}, {"owner", owner}}},
             {"expectedInstallDigest", gateway::ClawDigest(inst)}, {"expectedJournalDigest", gateway::ClawDigest(journal_prev)},
             {"configDigest", gateway::ClawDigest(c.raw)}};
    std::string err;
    const char* tok = std::getenv("OPENCLAW_GATEWAY_TOKEN");
    { std::string ignore; Rpc(GatewayBase(c.raw, url), tok ? tok : "", "config.reload", json::object(), 30, &ignore); }  // bind to the config on disk
    auto r = Rpc(GatewayBase(c.raw, url), tok ? tok : "", "claws.removalJournal", req, 600, &err);
    if (!err.empty()) { std::cerr << "claws remove: the Gateway must accept the deletion journal first: " << err << "\n"; return 1; }
    const auto journal = r.value("journal", json());
    if (!journal.is_object() || journal.value("operationId", "") != op) { std::cerr << "claws remove: Claw journal outcome is unknown; inspect claws status before retrying\n"; return 1; }
    // 2. Cleanup, bounded by the journal: owned workspace files, then agent-private directories.
    std::error_code ec;
    const fs::path ws = inst.value("workspace", "");
    for (auto& f : remove_files) fs::remove(ws / f.get<std::string>(), ec);
    for (auto it = fs::recursive_directory_iterator(ws, ec); !ec && it != fs::recursive_directory_iterator(); ++it) {}
    // prune now-empty directories bottom-up
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(ws, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) if (it->is_directory()) dirs.push_back(it->path());
    std::sort(dirs.begin(), dirs.end(), [](auto& a, auto& b) { return a.string().size() > b.string().size(); });
    for (auto& d : dirs) if (fs::is_empty(d, ec)) fs::remove(d, ec);
    if (fs::exists(ws, ec) && fs::is_empty(ws, ec)) fs::remove(ws, ec);
    json deleted = json::array();
    const std::set<std::string> protected_roots{fs::weakly_canonical(data).string(), fs::weakly_canonical(Home()).string(), fs::weakly_canonical(StateDir()).string(), "/"};
    for (auto& cp : journal["cleanupPaths"]) {
        const fs::path p = cp.value("path", "");
        if (p == ws || p.empty()) continue;  // workspace handled per owned file above
        bool owned = false;
        for (auto& part : p) if (part == id) owned = true;
        if (!owned || protected_roots.count(fs::weakly_canonical(p).string())) continue;
        if (fs::exists(p, ec)) { fs::remove_all(p, ec); deleted.push_back(p.string()); }
    }
    // 3. Config entry, then close the journal (cleanupCompleted) and drop the install record.
    c = LoadCfg();
    for (const char* path : {"agents.list"}) {
        auto l = Get(c.raw, path);
        if (!l.is_array()) continue;
        json kept = json::array();
        for (auto& a : l) if (!(a.is_object() && a.value("id", "") == id)) kept.push_back(a);
        Set(c.raw, path, kept);
    }
    if (auto e = Get(c.raw, "agents.entries"); e.is_object() && e.contains(id)) { e.erase(id); Set(c.raw, "agents.entries", e); }
    SaveCfg(c);
    st = gateway::ClawStateRead(data);
    st["journals"][id]["cleanupCompleted"] = true;
    st["journals"][id]["completedAtMs"] = static_cast<long long>(std::time(nullptr)) * 1000;
    st["installs"].erase(id);
    st["leases"].erase(id);
    WriteClawState(st);
    plan["applied"] = true; plan["deletedPaths"] = deleted; plan["operationId"] = op;
    if (as_json) std::cout << plan.dump(2) << "\n";
    else std::cout << "removed Claw agent '" << id << "'" << (retained.empty() ? "" : " (kept " + std::to_string(retained.size()) + " modified file(s))") << "\n";
    return 0;
}

// `claws removal-journal begin|rollback`: the CLI side of upstream's clawRemovalJournalGateway. It computes the
// gateway binding, physical state identity, config digest and expected install/journal digests locally, then
// calls claws.removalJournal. --stale-journal deliberately sends an outdated journal digest (test hook).
int CmdClawsRemovalJournal(int argc, char** argv) {
    if (argc < 4 || std::string(argv[2]) != "removal-journal") { std::cerr << "usage: openclaw-cpp claws removal-journal begin|rollback --agent ID --operation ID --owner ID [--json]\n"; return 2; }
    const std::string phase = argv[3];
    if (phase != "begin" && phase != "rollback") { std::cerr << "phase must be begin or rollback\n"; return 2; }
    std::string agent, op, owner, url; bool as_json = false, stale = false;
    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--agent") agent = next(); else if (a == "--operation") op = next(); else if (a == "--owner") owner = next();
        else if (a == "--url") url = next(); else if (a == "--json") as_json = true; else if (a == "--stale-journal") stale = true;
        else { std::cerr << "unknown option '" << a << "'\n"; return 2; }
    }
    if (agent.empty() || op.empty() || owner.empty()) { std::cerr << "--agent, --operation and --owner are required\n"; return 2; }
    const auto c = LoadCfg();
    const fs::path data = [] { const char* e = std::getenv("OPENCLAW_DATA_DIR"); return e && *e ? fs::path(e) : Home() / ".openclaw-cpp" / "data"; }();
    auto st = gateway::ClawStateRead(data);
    const json install = st["installs"].contains(agent) ? st["installs"][agent] : json(nullptr);
    json journal = st["journals"].contains(agent) ? st["journals"][agent] : json(nullptr);
    if (stale) journal = json{{"agentId", agent}, {"operationId", "stale"}};
    json req{{"phase", phase}, {"agentId", agent}, {"operationId", op}, {"binding", gateway::ClawRemovalBinding(data)},
             {"sourceIdentity", gateway::ClawStateIdentity(data)}, {"lease", {{"scope", "core:agent-deletion"}, {"key", agent}, {"owner", owner}}},
             {"expectedInstallDigest", gateway::ClawDigest(install)}, {"expectedJournalDigest", gateway::ClawDigest(journal)},
             {"configDigest", gateway::ClawDigest(c.raw)}};
    std::string err;
    const char* tok = std::getenv("OPENCLAW_GATEWAY_TOKEN");
    auto r = Rpc(GatewayBase(c.raw, url), tok ? tok : "", "claws.removalJournal", req, 600, &err);
    if (!err.empty()) { std::cerr << "claws.removalJournal: " << err << "\n"; return 1; }
    if (as_json) std::cout << r.dump() << "\n";
    else if (r.value("journal", json()).is_null()) std::cout << "deletion journal for " << agent << " rolled back\n";
    else std::cout << "deletion journal begun for " << agent << " (operation " << op << ")\n";
    return 0;
}

int MaybeLocalCommand(int argc, char** argv) {
    if (argc < 2) return -1;
    const std::string cmd = argv[1];
    if (cmd == "completion") return CmdCompletion(argc, argv);
    if (cmd == "claws" && argc >= 3 && std::string(argv[2]) == "removal-journal") return CmdClawsRemovalJournal(argc, argv);
    if (cmd == "claws" && argc >= 3 && std::string(argv[2]) == "add") return CmdClawsAdd(argc, argv);
    if (cmd == "claws" && argc >= 3 && std::string(argv[2]) == "status") return CmdClawsStatus(argc, argv);
    if (cmd == "claws" && argc >= 3 && std::string(argv[2]) == "remove") return CmdClawsRemove(argc, argv);
    if (cmd == "onboard") return CmdOnboard(argc, argv, 2);
    if (cmd == "configure") return CmdConfigure(argc, argv);
    if (cmd == "migrate") return CmdMigrate(argc, argv);
    if (cmd == "tui") return CmdTui(argc, argv);
    if (cmd == "setup") return CmdSetup(argc, argv, "setup");
    if (cmd == "crestodian") { std::cerr << "`crestodian` is deprecated: use `openclaw-cpp setup`.\n"; return CmdSetup(argc, argv, "crestodian"); }
    return -1;
}

}  // namespace openclaw::cli
