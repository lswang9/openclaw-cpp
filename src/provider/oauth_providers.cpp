#include "openclaw/provider/oauth_providers.hpp"

#include "openclaw/auth/profiles.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <random>
#include <atomic>
#include <openssl/sha.h>

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


std::string B64Url(const unsigned char* data, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = data[i] << 16; if (i + 1 < n) v |= data[i + 1] << 8; if (i + 2 < n) v |= data[i + 2];
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63]; o += (i + 1 < n) ? t[(v >> 6) & 63] : '='; o += (i + 2 < n) ? t[v & 63] : '=';
    }
    while (!o.empty() && o.back() == '=') o.pop_back();
    for (char& c : o) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    return o;
}
std::string RandomUrlSafe(size_t n) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::random_device rd; std::mt19937 gen(rd()); std::uniform_int_distribution<int> d(0, 63);
    std::string o; o.resize(n); for (size_t i = 0; i < n; ++i) o[i] = alphabet[d(gen)]; return o;
}
std::string PkceChallenge(const std::string& verifier) {
    unsigned char h[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), h);
    return B64Url(h, SHA256_DIGEST_LENGTH);
}

// Tiny loopback HTTP server: waits for GET /auth/callback?code=&state= (or any path with those query params).
struct LoopbackWait {
    int port = 0; int fd = -1; std::string code, state, err;
    bool Start(int prefer) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) { err = "socket"; return false; }
        int yes = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<uint16_t>(prefer > 0 ? prefer : 0));
        if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { err = "bind"; ::close(fd); fd = -1; return false; }
        socklen_t al = sizeof a; getsockname(fd, reinterpret_cast<sockaddr*>(&a), &al); port = ntohs(a.sin_port);
        if (listen(fd, 1) != 0) { err = "listen"; ::close(fd); fd = -1; return false; }
        return true;
    }
    bool AcceptOnce(int timeout_ms) {
        if (fd < 0) return false;
        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(fd + 1, &rf, nullptr, nullptr, &tv) <= 0) { err = "timeout waiting for OAuth callback"; return false; }
        int c = ::accept(fd, nullptr, nullptr); if (c < 0) { err = "accept"; return false; }
        char buf[4096]; ssize_t n = ::recv(c, buf, sizeof buf - 1, 0); if (n <= 0) { ::close(c); err = "empty request"; return false; }
        buf[n] = 0; std::string req(buf);
        auto qpos = req.find('?'); auto sp = req.find(' ', qpos == std::string::npos ? 0 : qpos);
        std::string query = (qpos != std::string::npos && sp != std::string::npos) ? req.substr(qpos + 1, sp - qpos - 1) : std::string();
        auto get = [&](const char* k) {
            auto p = query.find(std::string(k) + "="); if (p == std::string::npos) return std::string();
            p += std::strlen(k) + 1; auto e = query.find('&', p); auto v = query.substr(p, e == std::string::npos ? std::string::npos : e - p);
            // minimal URL decode for %XX
            std::string o; for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] == '%' && i + 2 < v.size()) { int x = 0; std::sscanf(v.c_str() + i + 1, "%2x", &x); o += (char)x; i += 2; }
                else if (v[i] == '+') o += ' '; else o += v[i];
            }
            return o;
        };
        code = get("code"); state = get("state");
        const char* html = code.empty()
            ? "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<html><body>Missing code.</body></html>"
            : "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<html><body>Authorization received. You can return to OpenClaw.</body></html>";
        ::send(c, html, std::strlen(html), 0); ::close(c);
        if (code.empty()) { err = "callback missing code"; return false; }
        return true;
    }
    ~LoopbackWait() { if (fd >= 0) ::close(fd); }
};

json CodexBrowserPkceLogin(const std::function<void(const std::string&, const std::string&)>& show) {
    const std::string verifier = RandomUrlSafe(64);
    const std::string challenge = PkceChallenge(verifier);
    const std::string state = RandomUrlSafe(32);
    int prefer = 1455; if (const char* e = std::getenv("OPENCLAW_OAUTH_CALLBACK_PORT"); e && *e) prefer = std::atoi(e);
    LoopbackWait lb;
    if (!lb.Start(prefer > 0 ? prefer : 0)) return {{"error", "cannot bind OAuth callback: " + lb.err}};
    const std::string redirect = "http://127.0.0.1:" + std::to_string(lb.port) + "/auth/callback";
    std::string url = OpenAiAuthBase() + "/oauth/authorize?response_type=code&client_id=" + Enc(kCodexClientId)
        + "&redirect_uri=" + Enc(redirect) + "&scope=" + Enc("openid profile email offline_access")
        + "&code_challenge=" + Enc(challenge) + "&code_challenge_method=S256&state=" + Enc(state)
        + "&id_token_add_organizations=true&codex_cli_simplified_flow=true&originator=openclaw";
    show(url, "");
    std::string code, got_state;
    auto parse_qs = [](const std::string& q, const char* k) {
        auto p = q.find(std::string(k) + "="); if (p == std::string::npos) return std::string();
        p += std::strlen(k) + 1; auto e = q.find('&', p); return q.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };
    if (const char* ec = std::getenv("OPENCLAW_OAUTH_CODE"); ec && *ec) {
        code = ec; got_state = Env("OPENCLAW_OAUTH_STATE", state.c_str());
    } else if (Env("OPENCLAW_OAUTH_FAKE_BROWSER") == "1") {
        // Offline fakes: GET authorize (no follow), read Location ?code=&state=, skip the loopback race.
        CURL* c = curl_easy_init(); if (!c) return {{"error", "curl init"}};
        std::string loc, discard;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Sink); curl_easy_setopt(c, CURLOPT_WRITEDATA, &discard);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, +[](char* p, size_t sz, size_t n, void* u) -> size_t {
            std::string line(p, sz * n); auto* out = static_cast<std::string*>(u);
            if (line.size() > 9 && (line.rfind("Location:", 0) == 0 || line.rfind("location:", 0) == 0)) {
                auto v = line.substr(line.find(':') + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
                *out = v;
            }
            return sz * n;
        });
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &loc);
        curl_easy_perform(c); curl_easy_cleanup(c);
        if (loc.empty()) return {{"error", "fake browser: authorize returned no Location"}};
        auto qpos = loc.find('?'); std::string q = qpos == std::string::npos ? std::string() : loc.substr(qpos + 1);
        code = parse_qs(q, "code"); got_state = parse_qs(q, "state");
    } else {
        if (!lb.AcceptOnce(5 * 60 * 1000)) return {{"error", lb.err.empty() ? "OAuth callback failed" : lb.err}};
        code = lb.code; got_state = lb.state;
    }
    if (got_state != state) return {{"error", "OAuth state mismatch"}};
    if (code.empty()) return {{"error", "OAuth callback missing code"}};
    auto x = Http("POST", OpenAiAuthBase() + "/oauth/token", {"Content-Type: application/x-www-form-urlencoded", "Accept: application/json"},
                  Form({{"grant_type", "authorization_code"}, {"code", code}, {"code_verifier", verifier}, {"client_id", kCodexClientId},
                        {"redirect_uri", redirect}}));
    if (x.status != 200 || !x.j.is_object() || !x.j.contains("access_token"))
        return {{"error", "OpenAI PKCE token exchange failed (HTTP " + std::to_string(x.status) + ")"}};
    json cred{{"type", "oauth"}, {"provider", "openai-codex"}, {"access", x.j["access_token"]}, {"refresh", x.j.value("refresh_token", std::string())},
              {"expires", NowMs() + x.j.value("expires_in", 3600) * 1000LL}, {"authFlow", "oauth-pkce"}};
    if (auto acc = CodexAccountIdFromJwt(x.j.value("id_token", x.j["access_token"].get<std::string>())); !acc.empty()) cred["accountId"] = acc;
    return cred;
}

void ResetCachesForTest() { std::lock_guard<std::mutex> l(g_mu); g_copilot.clear(); }

}  // namespace openclaw::provider::oauth
