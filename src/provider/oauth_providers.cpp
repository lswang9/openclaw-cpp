#include "openclaw/provider/oauth_providers.hpp"

#include "openclaw/auth/profiles.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

namespace openclaw::provider::oauth {
using json = nlohmann::json;
namespace {

const char* kCopilotClientId = "Iv1.b507a08c87ecfe98";
const char* kCodexClientId = "app_EMoamEEZ73f0CkXaXp7hrann";

std::string Env(const char* k, const char* def = "") { const char* v = std::getenv(k); return v && *v ? v : def; }
std::string NoSlash(std::string s) { while (!s.empty() && s.back() == '/') s.pop_back(); return s; }
long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string GithubBase() { return NoSlash(Env("OPENCLAW_GITHUB_BASE", "https://github.com")); }
std::string GithubApiBase() { return NoSlash(Env("OPENCLAW_GITHUB_API_BASE", "https://api.github.com")); }
std::string OpenAiAuthBase() { return NoSlash(Env("OPENCLAW_OPENAI_AUTH_BASE", "https://auth.openai.com")); }

size_t Sink(char* p, size_t s, size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }
struct Res { long status = 0; std::string body; json j; };
Res Http(const std::string& method, const std::string& url, const std::vector<std::string>& headers, const std::string& body) {
    Res r; CURL* c = curl_easy_init(); if (!c) return r;
    curl_slist* h = nullptr; for (auto& x : headers) h = curl_slist_append(h, x.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str()); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Sink); curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L); curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (method == "POST") { curl_easy_setopt(c, CURLOPT_POST, 1L); curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str()); curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size()); }
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(h); curl_easy_cleanup(c);
    r.j = json::parse(r.body, nullptr, false);
    return r;
}
std::string Enc(const std::string& s) {
    std::string o; char b[4];
    for (unsigned char ch : s) { if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += (char)ch; else { std::snprintf(b, sizeof b, "%%%02X", ch); o += b; } }
    return o;
}
std::string Form(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string o; for (auto& [k, v] : kv) { if (!o.empty()) o += "&"; o += Enc(k) + "=" + Enc(v); } return o;
}
void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, ms))); }

std::mutex g_mu;
struct CopilotCache { std::string base; long long at = 0; };
std::map<std::string, CopilotCache> g_copilot;  // github token → resolved CAPI base

bool TrustedCopilotHost(const std::string& url) {
    if (!Env("OPENCLAW_GITHUB_API_BASE").empty()) return url.rfind("http://127.0.0.1", 0) == 0 || url.rfind("http://localhost", 0) == 0 || url.rfind("https://", 0) == 0;
    if (url.rfind("https://", 0) != 0) return false;
    auto host = url.substr(8); host = host.substr(0, host.find('/'));
    if (host.find('@') != std::string::npos || host.find('?') != std::string::npos) return false;
    const std::string suf = ".githubcopilot.com";
    return host == "copilot-proxy.githubusercontent.com" || (host.size() > suf.size() && host.compare(host.size() - suf.size(), suf.size(), suf) == 0);
}

std::string ProfileToken(const std::string& provider, json* cred_out = nullptr, std::string* pid_out = nullptr) {
    auth::ProfileStore store(auth::DefaultProfilesPath("main"));
    auto p = store.ForProvider(provider);
    if (!p) return {};
    if (cred_out) *cred_out = p->second;
    if (pid_out) *pid_out = p->first;
    const auto& c = p->second;
    return c.value("access", c.value("token", c.value("key", std::string())));
}

bool ApplyCopilot(const json& cfg, ProviderSpec* s, std::string* why) {
    (void)cfg;
    std::string gh = s->api_key;
    if (gh.empty()) for (auto k : {"COPILOT_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN"}) if (gh = Env(k); !gh.empty()) break;
    if (gh.empty()) gh = ProfileToken("github-copilot");
    if (gh.empty()) { if (why) *why = "no GitHub token for github-copilot (run `openclaw-cpp models auth login --provider github-copilot` or set COPILOT_GITHUB_TOKEN)"; return false; }
    std::string base;
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_copilot.find(gh);
        if (it != g_copilot.end() && NowMs() - it->second.at < 10 * 60 * 1000) base = it->second.base;
    }
    if (base.empty()) {
        auto r = Http("GET", GithubApiBase() + "/copilot_internal/user", {"Accept: application/json", "Authorization: Bearer " + gh}, "");
        if (r.status != 200 || !r.j.is_object()) { if (why) *why = "GitHub Copilot user check failed (HTTP " + std::to_string(r.status) + ")"; return false; }
        auto api = r.j.contains("endpoints") && r.j["endpoints"].is_object() ? r.j["endpoints"].value("api", std::string()) : std::string();
        if (api.empty()) api = "https://api.individual.githubcopilot.com";
        if (!TrustedCopilotHost(api)) { if (why) *why = "GitHub Copilot user response has an untrusted endpoints.api URL"; return false; }
        base = NoSlash(api);
        std::lock_guard<std::mutex> l(g_mu); g_copilot[gh] = {base, NowMs()};
    }
    // Only take the default CAPI base when the provider config did not pin one.
    if (s->base_url.empty() || s->base_url == "https://api.individual.githubcopilot.com") s->base_url = base;
    s->api_key = gh;
    json h = s->headers.is_object() ? s->headers : json::object();
    std::string integ = "copilot-developer-cli";
    for (auto it = h.begin(); it != h.end();) { std::string k = it.key(); for (auto& c : k) c = (char)std::tolower((unsigned char)c);
        if (k == "copilot-integration-id") { if (it->is_string()) integ = it->get<std::string>(); it = h.erase(it); } else ++it; }
    h["Editor-Version"] = "vscode/1.99.3"; h["Editor-Plugin-Version"] = "copilot-chat/0.26.7"; h["User-Agent"] = "GitHubCopilotChat/0.26.7";
    h["Openai-Organization"] = "github-copilot"; h["Copilot-Integration-Id"] = integ;
    s->headers = h;
    return true;
}

json CodexRefresh(const json& cred, std::string* why) {
    const auto rt = cred.value("refresh", std::string());
    if (rt.empty()) { if (why) *why = "openai-codex credential has no refresh token"; return nullptr; }
    auto r = Http("POST", OpenAiAuthBase() + "/oauth/token", {"Content-Type: application/x-www-form-urlencoded", "Accept: application/json"},
                  Form({{"grant_type", "refresh_token"}, {"refresh_token", rt}, {"client_id", kCodexClientId}}));
    if (r.status != 200 || !r.j.is_object() || !r.j.contains("access_token")) {
        if (why) *why = "openai-codex token refresh failed (HTTP " + std::to_string(r.status) + (r.j.is_object() ? " " + r.j.value("error", std::string()) : std::string()) + ")"; return nullptr; }
    json n = cred;
    n["access"] = r.j["access_token"];
    if (r.j.contains("refresh_token")) n["refresh"] = r.j["refresh_token"];
    n["expires"] = NowMs() + r.j.value("expires_in", 3600) * 1000LL;
    if (auto acc = CodexAccountIdFromJwt(r.j.value("id_token", n["access"].get<std::string>())); !acc.empty()) n["accountId"] = acc;
    return n;
}

bool ApplyCodex(const json& cfg, ProviderSpec* s, std::string* why) {
    (void)cfg;
    std::string token = s->api_key, account;
    if (token.empty()) {
        json cred; std::string pid;
        std::lock_guard<std::mutex> l(g_mu);  // serialize refreshes (refresh tokens are single-use)
        token = ProfileToken("openai-codex", &cred, &pid);
        if (token.empty()) { if (why) *why = "no OpenAI Codex login (run `openclaw-cpp models auth login --provider openai-codex`)"; return false; }
        if (auth::OAuthNeedsRefresh(cred)) {
            auto n = CodexRefresh(cred, why);
            if (n.is_null()) return false;
            auth::ProfileStore(auth::DefaultProfilesPath("main")).Upsert(pid, n);
            cred = n; token = n.value("access", std::string());
        }
        account = cred.value("accountId", std::string());
    }
    if (account.empty()) account = CodexAccountIdFromJwt(token);
    json h = s->headers.is_object() ? s->headers : json::object();
    if (!account.empty()) h["chatgpt-account-id"] = account;
    h["originator"] = "openclaw"; h["OpenAI-Beta"] = "responses=experimental";
    s->headers = h; s->api_key = token;
    return true;
}

std::string B64UrlDecode(std::string in) {
    for (auto& c : in) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    while (in.size() % 4) in += '=';
    std::string out(in.size(), '\0');
    int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()), reinterpret_cast<const unsigned char*>(in.data()), (int)in.size());
    if (n < 0) return {};
    size_t pad = 0; for (size_t i = in.size(); i > 0 && in[i - 1] == '='; --i) ++pad;
    out.resize((size_t)n - std::min<size_t>(pad, (size_t)n));
    return out;
}

}  // namespace

std::string CodexAccountIdFromJwt(const std::string& jwt) {
    auto a = jwt.find('.'); if (a == std::string::npos) return {};
    auto b = jwt.find('.', a + 1); if (b == std::string::npos) return {};
    auto j = json::parse(B64UrlDecode(jwt.substr(a + 1, b - a - 1)), nullptr, false);
    if (!j.is_object()) return {};
    if (j.contains("https://api.openai.com/auth") && j["https://api.openai.com/auth"].is_object())
        return j["https://api.openai.com/auth"].value("chatgpt_account_id", std::string());
    return j.value("chatgpt_account_id", std::string());
}

bool ApplyRuntimeAuth(const json& cfg, ProviderSpec* s, std::string* why) {
    if (s->auth == "github-copilot") return ApplyCopilot(cfg, s, why);
    if (s->auth == "openai-codex") return ApplyCodex(cfg, s, why);
    return true;
}

json CopilotDeviceLogin(const std::function<void(const std::string&, const std::string&)>& show, int poll_override) {
    const std::vector<std::string> hdr{"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"};
    auto r = Http("POST", GithubBase() + "/login/device/code", hdr, Form({{"client_id", kCopilotClientId}, {"scope", "read:user"}}));
    if (r.status != 200 || !r.j.is_object() || !r.j.contains("device_code")) return {{"error", "GitHub device code failed (HTTP " + std::to_string(r.status) + ")"}};
    const auto dc = r.j.value("device_code", std::string());
    show(r.j.value("verification_uri", GithubBase() + "/login/device"), r.j.value("user_code", std::string()));
    int interval = poll_override >= 0 ? poll_override : r.j.value("interval", 5) * 1000;
    const long long deadline = NowMs() + r.j.value("expires_in", 900) * 1000LL;
    while (NowMs() < deadline) {
        Sleep(interval);
        auto t = Http("POST", GithubBase() + "/login/oauth/access_token", hdr,
                      Form({{"client_id", kCopilotClientId}, {"device_code", dc}, {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}));
        if (t.j.is_object() && t.j.contains("access_token")) return {{"type", "token"}, {"provider", "github-copilot"}, {"token", t.j["access_token"]}};
        const auto err = t.j.is_object() ? t.j.value("error", std::string()) : std::string();
        if (err == "authorization_pending") continue;
        if (err == "slow_down") { interval += 5000; continue; }
        return {{"error", "GitHub device token failed: " + (err.empty() ? "HTTP " + std::to_string(t.status) : err)}};
    }
    return {{"error", "GitHub device code expired"}};
}

json CodexDeviceLogin(const std::function<void(const std::string&, const std::string&)>& show, int poll_override) {
    const std::vector<std::string> hj{"Content-Type: application/json", "Accept: application/json"};
    auto r = Http("POST", OpenAiAuthBase() + "/api/accounts/deviceauth/usercode", hj, json{{"client_id", kCodexClientId}}.dump());
    if (r.status == 404) return {{"error", "OpenAI Codex device code login is not enabled for this server. Use ChatGPT OAuth instead."}};
    const auto dai = r.j.is_object() ? r.j.value("device_auth_id", std::string()) : std::string();
    const auto uc = r.j.is_object() ? r.j.value("user_code", r.j.value("usercode", std::string())) : std::string();
    if (r.status != 200 || dai.empty() || uc.empty()) return {{"error", "OpenAI device code request failed (HTTP " + std::to_string(r.status) + ")"}};
    show(OpenAiAuthBase() + "/codex/device", uc);
    int interval = poll_override >= 0 ? poll_override : (r.j["interval"].is_number() ? r.j.value("interval", 5) : 5) * 1000;
    const long long deadline = NowMs() + 15 * 60 * 1000LL;
    while (NowMs() < deadline) {
        Sleep(interval);
        auto t = Http("POST", OpenAiAuthBase() + "/api/accounts/deviceauth/token", hj, json{{"device_auth_id", dai}, {"user_code", uc}}.dump());
        if (t.status == 403 || t.status == 404) continue;
        if (t.status != 200 || !t.j.is_object()) return {{"error", "OpenAI device authorization failed (HTTP " + std::to_string(t.status) + ")"}};
        const auto code = t.j.value("authorization_code", std::string()), ver = t.j.value("code_verifier", std::string());
        if (code.empty() || ver.empty()) return {{"error", "OpenAI device authorization response was missing the exchange code."}};
        auto x = Http("POST", OpenAiAuthBase() + "/oauth/token", {"Content-Type: application/x-www-form-urlencoded", "Accept: application/json"},
                      Form({{"grant_type", "authorization_code"}, {"code", code}, {"code_verifier", ver}, {"client_id", kCodexClientId},
                            {"redirect_uri", OpenAiAuthBase() + "/deviceauth/callback"}}));
        if (x.status != 200 || !x.j.is_object() || !x.j.contains("access_token")) return {{"error", "OpenAI token exchange failed (HTTP " + std::to_string(x.status) + ")"}};
        json cred{{"type", "oauth"}, {"provider", "openai-codex"}, {"access", x.j["access_token"]}, {"refresh", x.j.value("refresh_token", std::string())},
                  {"expires", NowMs() + x.j.value("expires_in", 3600) * 1000LL}};
        if (auto acc = CodexAccountIdFromJwt(x.j.value("id_token", x.j["access_token"].get<std::string>())); !acc.empty()) cred["accountId"] = acc;
        return cred;
    }
    return {{"error", "OpenAI device code expired"}};
}

void ResetCachesForTest() { std::lock_guard<std::mutex> l(g_mu); g_copilot.clear(); }

}  // namespace openclaw::provider::oauth
