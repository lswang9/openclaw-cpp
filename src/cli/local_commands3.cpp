// Upstream v2026.10.5 local commands, batch 3: directory (channel contact/group lookups), proxy (explicit debug proxy with
// capture store + validator), database (SQLite schema preflight + shared-state ownership), update (wizard, cleanup,
// recover, migration-plan, admit, adopt-immutable).
#include "openclaw/cli/local_commands.hpp"
#include "openclaw/config/config.hpp"
#include "openclaw/channels/outbound.hpp"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;

namespace openclaw::cli {
namespace {

struct A3 {
    std::vector<std::string> pos;
    std::map<std::string, std::vector<std::string>> opt;
    bool has(const std::string& k) const { return opt.count(k) > 0; }
    std::string get(const std::string& k, const std::string& d = "") const { auto it = opt.find(k); return it == opt.end() || it->second.empty() || it->second.back().empty() ? d : it->second.back(); }
    std::vector<std::string> all(const std::string& k) const { auto it = opt.find(k); return it == opt.end() ? std::vector<std::string>{} : it->second; }
};
std::optional<A3> P3(int argc, char** argv, int from, const std::vector<std::string>& allowed, const std::string& cmd) {
    A3 a;
    for (int i = from; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--") { for (++i; i < argc; ++i) a.pos.push_back(argv[i]); break; }
        if (s == "--help" || s == "-h") {
            std::cout << "Usage: openclaw-cpp " << cmd << " [args] [options]\n\nOptions:\n";
            for (auto& o : allowed) std::cout << "  " << (o.back() == '=' ? o.substr(0, o.size() - 1) + " <value>" : o) << "\n";
            std::cout << "  -h, --help\n"; std::exit(0);
        }
        if (s.size() > 2 && s.rfind("--", 0) == 0) {
            std::string val; bool inl = false;
            if (auto eq = s.find('='); eq != std::string::npos) { val = s.substr(eq + 1); s = s.substr(0, eq); inl = true; }
            const bool takes = std::find(allowed.begin(), allowed.end(), s + "=") != allowed.end();
            const bool flag = std::find(allowed.begin(), allowed.end(), s) != allowed.end();
            if (!takes && !flag) { std::cerr << "error: unknown option '" << s << "'\n(run `openclaw-cpp " << cmd << " --help`)\n"; return std::nullopt; }
            if (takes && !inl) { if (i + 1 >= argc) { std::cerr << "error: option '" << s << " <value>' argument missing\n"; return std::nullopt; } val = argv[++i]; }
            a.opt[s].push_back(val);
        } else a.pos.push_back(s);
    }
    return a;
}
fs::path HomeDir() { const char* h = std::getenv("HOME"); return h ? fs::path(h) : fs::path("/tmp"); }
fs::path StateRoot() { if (const char* s = std::getenv("OPENCLAW_STATE_DIR"); s && *s) return s; return HomeDir() / ".openclaw"; }
fs::path DataRoot() { const char* e = std::getenv("OPENCLAW_DATA_DIR"); return e && *e ? fs::path(e) : HomeDir() / ".openclaw-cpp" / "data"; }
json LoadRaw() {
    const auto p = config::DefaultPath(); std::error_code ec;
    if (!fs::exists(p, ec)) return json::object();
    std::ifstream in(p); std::stringstream ss; ss << in.rdbuf();
    try { auto j = config::ParseJson5(ss.str()); return j.is_object() ? j : json::object(); } catch (...) { return json::object(); }
}
void SaveRaw(const json& raw) { const auto p = config::DefaultPath(); std::error_code ec; fs::create_directories(p.parent_path(), ec); config::SaveRaw(p, raw); }
json ReadJ(const fs::path& p, json d = json::object()) { std::ifstream f(p); if (!f) return d; try { return json::parse(f); } catch (...) { return d; } }
void WriteJ(const fs::path& p, const json& j) { std::error_code ec; fs::create_directories(p.parent_path(), ec); auto t = p; t += ".tmp"; { std::ofstream f(t); f << j.dump(2); } fs::rename(t, p, ec); }
long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string IsoNow() { auto t = std::time(nullptr); char b[32]; std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t)); return b; }
std::string RandHex(int n) { std::vector<unsigned char> b(n); RAND_bytes(b.data(), n); static const char* x = "0123456789abcdef"; std::string o; for (auto c : b) { o += x[c >> 4]; o += x[c & 15]; } return o; }
std::string Sha256(const std::string& s) { unsigned char h[32]; SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), h); static const char* x = "0123456789abcdef"; std::string o; for (auto c : h) { o += x[c >> 4]; o += x[c & 15]; } return o; }
void Emit(bool js, const json& j, const std::function<void()>& human) { if (js) std::cout << j.dump(2) << "\n"; else human(); }
std::size_t SinkS(char* p, std::size_t s, std::size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }
struct Resp { long status = 0; std::string body, error; };
Resp Http(const std::string& url, const std::vector<std::string>& headers, long timeout_ms = 15000, const std::string& proxy = "", const std::string& cafile = "") {
    Resp r; CURL* c = curl_easy_init(); if (!c) { r.error = "curl init"; return r; }
    curl_slist* h = nullptr; for (auto& x : headers) h = curl_slist_append(h, x.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str()); if (h) curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms); curl_easy_setopt(c, CURLOPT_USERAGENT, "openclaw-cpp");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, SinkS); curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (!proxy.empty()) { curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str()); curl_easy_setopt(c, CURLOPT_NOPROXY, ""); }
    else curl_easy_setopt(c, CURLOPT_PROXY, "");
    if (!cafile.empty()) curl_easy_setopt(c, CURLOPT_PROXY_CAINFO, cafile.c_str());
    auto rc = curl_easy_perform(c); if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(h); curl_easy_cleanup(c); return r;
}
std::string UrlEnc(const std::string& s) { std::string o; char b[4]; for (unsigned char ch : s) { if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += static_cast<char>(ch); else { std::snprintf(b, sizeof b, "%%%02X", ch); o += b; } } return o; }
std::string Trim(std::string s) { while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back(); size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; return s.substr(i); }
std::string Lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string NoSlash(std::string s) { while (!s.empty() && s.back() == '/') s.pop_back(); return s; }

// ============================================================ directory (Discord / Slack / Telegram REST lookups)
int CmdDirectory(int argc, char** argv) {
    std::string sub = argc >= 3 ? argv[2] : "", leaf; int from = 3;
    if (sub == "peers" || sub == "groups") { leaf = argc >= 4 ? argv[3] : ""; from = 4;
        if (leaf != "list" && !(sub == "groups" && leaf == "members")) { std::cerr << "error: missing subcommand for 'directory " << sub << "' (" << (sub == "groups" ? "list|members" : "list") << ")\n"; return 1; } }
    std::vector<std::string> allowed{"--channel=", "--account=", "--json"};
    if (leaf == "list") { allowed.push_back("--query="); allowed.push_back("--limit="); }
    if (leaf == "members") { allowed.push_back("--group-id="); allowed.push_back("--limit="); }
    auto a = P3(argc, argv, from, allowed, "directory " + sub + (leaf.empty() ? "" : " " + leaf)); if (!a) return 2;
    const bool js = a->has("--json");
    int limit = 0;
    if (a->has("--limit")) { try { limit = std::stoi(a->get("--limit")); } catch (...) { limit = -1; } if (limit <= 0) { std::cerr << "--limit must be a positive integer.\n"; return 1; } }
    if (leaf == "members" && a->get("--group-id").empty()) { std::cerr << "Missing --group-id\n"; return 1; }
    const auto raw = LoadRaw();
    const auto dtok = channels::ChannelCred(raw, "discord", "token", "DISCORD_BOT_TOKEN");
    const auto stok = channels::ChannelCred(raw, "slack", "botToken", "SLACK_BOT_TOKEN");
    const auto ttok = channels::ChannelCred(raw, "telegram", "botToken", "TELEGRAM_BOT_TOKEN");
    std::string ch = Lower(a->get("--channel"));
    auto fail = [&](const std::string& m) { if (js) std::cout << json{{"error", m}}.dump() << "\n"; else std::cerr << m << "\n"; return 1; };
    if (ch.empty()) {
        std::vector<std::string> conf; if (!dtok.empty()) conf.push_back("discord"); if (!stok.empty()) conf.push_back("slack"); if (!ttok.empty()) conf.push_back("telegram");
        if (conf.empty()) return fail("No directory-capable channel is configured (discord, slack, telegram).");
        if (conf.size() > 1) return fail("Multiple channels configured (" + conf[0] + ", " + conf[1] + (conf.size() > 2 ? ", ..." : "") + "); pass --channel.");
        ch = conf[0];
    }
    const std::string qraw = a->get("--query"), q = Lower(qraw), gid = a->get("--group-id"), acct = a->get("--account", "default");
    auto match = [&](const json& e) { return q.empty() || Lower(e.value("name", "")).find(q) != std::string::npos || Lower(e.value("handle", "")).find(q) != std::string::npos || e.value("id", "") == qraw; };
    json out = json::array(), self;
    auto getj = [&](const Resp& r, const std::string& what) -> json {
        if (r.status < 200 || r.status >= 300) throw std::runtime_error(what + " failed (HTTP " + std::to_string(r.status) + (r.error.empty() ? "" : ", " + r.error) + ")");
        try { return json::parse(r.body); } catch (...) { throw std::runtime_error(what + ": invalid JSON"); } };
    auto str = [](const json& j, const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    try {
        if (ch == "discord") {
            if (dtok.empty()) return fail("discord is not configured (channels.discord.token / DISCORD_BOT_TOKEN)");
            auto base = NoSlash(channels::ChannelCred(raw, "discord", "apiBase", "DISCORD_API_BASE")); if (base.empty()) base = "https://discord.com/api/v10";
            const std::vector<std::string> H{"Authorization: Bot " + dtok};
            auto dg = [&](const std::string& p) { return getj(Http(base + p, H), "discord " + p.substr(0, p.find('?'))); };
            auto uname = [&](const json& u, const json& m) { auto n = str(m, "nick"); if (n.empty()) n = str(u, "global_name"); if (n.empty()) n = str(u, "username"); return n; };
            if (sub == "self") { auto u = dg("/users/@me"); self = {{"channel", "discord"}, {"accountId", acct}, {"id", str(u, "id")}, {"name", uname(u, json::object())}, {"handle", str(u, "username")}, {"bot", u.value("bot", false)}}; }
            else if (sub == "groups" && leaf == "list") {
                for (auto& g : dg("/users/@me/guilds")) for (auto& c : dg("/guilds/" + str(g, "id") + "/channels")) {
                    const int t = c.value("type", -1); if (t != 0 && t != 5 && t != 15) continue;
                    json e{{"kind", "group"}, {"id", str(c, "id")}, {"name", "#" + str(c, "name")}, {"guildId", str(g, "id")}, {"guild", str(g, "name")}}; if (match(e)) out.push_back(e); }
            } else {
                std::vector<std::string> guilds;
                if (leaf == "members") { auto c = dg("/channels/" + UrlEnc(gid)); guilds.push_back(str(c, "guild_id").empty() ? gid : str(c, "guild_id")); }
                else for (auto& g : dg("/users/@me/guilds")) guilds.push_back(str(g, "id"));
                std::set<std::string> seen; const auto lim = std::to_string(limit > 0 ? std::min(limit, 1000) : 1000);
                for (auto& g : guilds) {
                    auto ms = !q.empty() && leaf == "list" ? dg("/guilds/" + g + "/members/search?query=" + UrlEnc(qraw) + "&limit=" + lim) : dg("/guilds/" + g + "/members?limit=" + lim);
                    for (auto& m : ms) { auto u = m.value("user", json::object()); if (!seen.insert(str(u, "id")).second) continue;
                        json e{{"kind", "user"}, {"id", str(u, "id")}, {"name", uname(u, m)}, {"handle", str(u, "username")}};
                        if (u.value("bot", false)) e["bot"] = true;
                        if (leaf == "members" || match(e)) out.push_back(e); }
                }
            }
        } else if (ch == "slack") {
            if (stok.empty()) return fail("slack is not configured (channels.slack.botToken / SLACK_BOT_TOKEN)");
            auto base = NoSlash(channels::ChannelCred(raw, "slack", "apiBase", "SLACK_API_BASE")); if (base.empty()) base = "https://slack.com/api";
            const std::vector<std::string> H{"Authorization: Bearer " + stok};
            auto sg = [&](const std::string& m) { auto j = getj(Http(base + "/" + m, H), "slack " + m.substr(0, m.find('?')));
                if (!j.value("ok", false)) throw std::runtime_error("slack " + m.substr(0, m.find('?')) + ": " + str(j, "error")); return j; };
            auto paged = [&](const std::string& m, const std::string& key) { json all = json::array(); std::string cur;
                for (int i = 0; i < 50; ++i) { auto j = sg(m + (m.find('?') == std::string::npos ? "?" : "&") + "limit=200" + (cur.empty() ? "" : "&cursor=" + UrlEnc(cur)));
                    for (auto& x : j.value(key, json::array())) all.push_back(x);
                    cur = str(j.value("response_metadata", json::object()), "next_cursor"); if (cur.empty()) break; }
                return all; };
            auto rname = [&](const json& u) { auto n = str(u, "real_name"); return n.empty() ? str(u, "name") : n; };
            if (sub == "self") { auto j = sg("auth.test"); self = {{"channel", "slack"}, {"accountId", acct}, {"id", str(j, "user_id")}, {"name", str(j, "user")}, {"team", str(j, "team")}, {"teamId", str(j, "team_id")}}; }
            else if (sub == "peers") { for (auto& u : paged("users.list", "members")) { if (u.value("deleted", false)) continue;
                    json e{{"kind", "user"}, {"id", str(u, "id")}, {"name", rname(u)}, {"handle", str(u, "name")}}; if (u.value("is_bot", false)) e["bot"] = true; if (match(e)) out.push_back(e); } }
            else if (leaf == "list") { for (auto& c : paged("conversations.list?types=public_channel,private_channel&exclude_archived=true", "channels")) {
                    json e{{"kind", "group"}, {"id", str(c, "id")}, {"name", "#" + str(c, "name")}, {"private", c.value("is_private", false)}}; if (match(e)) out.push_back(e); } }
            else { std::map<std::string, json> names; for (auto& u : paged("users.list", "members")) names[str(u, "id")] = u;
                for (auto& id : paged("conversations.members?channel=" + UrlEnc(gid), "members")) { const auto s = id.get<std::string>(); auto u = names.count(s) ? names[s] : json::object();
                    out.push_back({{"kind", "user"}, {"id", s}, {"name", rname(u)}, {"handle", str(u, "name")}}); } }
        } else if (ch == "telegram") {
            if (ttok.empty()) return fail("telegram is not configured (channels.telegram.botToken / TELEGRAM_BOT_TOKEN)");
            if (sub != "self") return fail(std::string("Channel telegram does not support ") + (sub == "peers" ? "peer listing" : leaf == "members" ? "group member listing" : "group listing") + " (the Bot API has no directory)");
            auto base = NoSlash(channels::ChannelCred(raw, "telegram", "apiBase", "TELEGRAM_API_BASE")); if (base.empty()) base = "https://api.telegram.org";
            auto j = getj(Http(base + "/bot" + ttok + "/getMe", {}), "telegram getMe"); auto u = j.value("result", json::object());
            self = {{"channel", "telegram"}, {"accountId", acct}, {"id", std::to_string(u.value("id", 0LL))}, {"name", str(u, "first_name")}, {"handle", str(u, "username")}, {"bot", true}};
        } else return fail("Unsupported channel: " + ch);
    } catch (const std::exception& e) { return fail(e.what()); }
    if (sub == "self") { Emit(js, self, [&] { std::cout << self.value("name", "") << "  (" << self.value("id", "") << ")" << (self.value("handle", "").empty() ? "" : "  @" + self.value("handle", "")) << "\n"; }); return 0; }
    if (limit > 0 && out.size() > static_cast<size_t>(limit)) out.erase(out.begin() + limit, out.end());
    Emit(js, out, [&] {
        const std::string title = sub == "peers" ? "Peers" : leaf == "members" ? "Group members" : "Groups";
        if (out.empty()) { std::cout << "No " << Lower(title) << " found for " << (leaf == "members" ? "group \"" + gid + "\", " : "") << ch << " (account " << acct << ").\n"; return; }
        std::cout << title << " (" << ch << ", " << out.size() << ")\n";
        for (auto& e : out) std::cout << "  " << e.value("id", "") << "  " << e.value("name", "") << (e.value("handle", "").empty() ? "" : "  @" + e.value("handle", "")) << "\n";
    });
    return 0;
}

// ============================================================ proxy: explicit HTTP/1.1 debug proxy + capture store + validator
fs::path CaptureDir() { return StateRoot() / "debug-proxy"; }
std::mutex g_cap_mu;
void CapAppend(const std::string& file, const json& j) { std::lock_guard<std::mutex> l(g_cap_mu); std::error_code ec; fs::create_directories(CaptureDir(), ec); std::ofstream f(CaptureDir() / file, std::ios::app); f << j.dump() << "\n"; }
std::vector<json> CapRead(const std::string& file) { std::vector<json> v; std::ifstream f(CaptureDir() / file); std::string line; while (std::getline(f, line)) { try { v.push_back(json::parse(line)); } catch (...) {} } return v; }
std::string CapBlob(const std::string& data) {
    if (data.empty()) return {};
    const auto id = "blob-" + Sha256(data).substr(0, 24); std::error_code ec; fs::create_directories(CaptureDir() / "blobs", ec);
    const auto p = CaptureDir() / "blobs" / id; if (!fs::exists(p, ec)) { std::ofstream f(p, std::ios::binary); f << data; }
    return id;
}
int Dial(const std::string& host, const std::string& port) {
    addrinfo hints{}, *res = nullptr; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (auto* p = res; p; p = p->ai_next) { fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol); if (fd < 0) continue; if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break; close(fd); fd = -1; }
    freeaddrinfo(res); return fd;
}
bool SendAll(int fd, const std::string& s) { size_t o = 0; while (o < s.size()) { auto n = send(fd, s.data() + o, s.size() - o, MSG_NOSIGNAL); if (n <= 0) return false; o += static_cast<size_t>(n); } return true; }
std::pair<size_t, size_t> Relay(int a, int b, std::string* capture_ba, size_t cap) {
    size_t ab = 0, ba = 0; char buf[16384]; pollfd p[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
    for (;;) {
        if (poll(p, 2, 120000) <= 0) break;
        bool done = false;
        for (int i = 0; i < 2 && !done; ++i) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            auto n = recv(p[i].fd, buf, sizeof buf, 0);
            if (n <= 0 || !SendAll(p[1 - i].fd, std::string(buf, static_cast<size_t>(n)))) { done = true; break; }
            if (i == 0) ab += static_cast<size_t>(n);
            else { ba += static_cast<size_t>(n); if (capture_ba && capture_ba->size() < cap) capture_ba->append(buf, std::min(static_cast<size_t>(n), cap - capture_ba->size())); }
        }
        if (done) break;
    }
    return {ab, ba};
}
struct DebugProxy {
    std::string host = "127.0.0.1"; int port = 0; int lfd = -1; std::string session;
    std::atomic<bool> stop{false}; std::thread th; std::atomic<int> seq{0}; std::atomic<int> active{0};
    bool Start() {
        addrinfo hints{}, *res = nullptr; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return false;
        lfd = socket(res->ai_family, SOCK_STREAM, 0); int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(lfd, res->ai_addr, res->ai_addrlen) != 0 || listen(lfd, 64) != 0) { freeaddrinfo(res); close(lfd); lfd = -1; return false; }
        freeaddrinfo(res);
        sockaddr_storage ss{}; socklen_t sl = sizeof ss; getsockname(lfd, reinterpret_cast<sockaddr*>(&ss), &sl);
        port = ntohs(ss.ss_family == AF_INET ? reinterpret_cast<sockaddr_in*>(&ss)->sin_port : reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
        session = "cap-" + std::to_string(NowMs()) + "-" + RandHex(3);
        CapAppend("sessions.jsonl", {{"id", session}, {"event", "start"}, {"host", host}, {"port", port}, {"startedAt", IsoNow()}, {"pid", getpid()}});
        th = std::thread([this] { Loop(); });
        return true;
    }
    void Stop() {
        if (stop.exchange(true)) return;
        shutdown(lfd, SHUT_RDWR); close(lfd); if (th.joinable()) th.join();
        for (int i = 0; i < 50 && active.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CapAppend("sessions.jsonl", {{"id", session}, {"event", "end"}, {"endedAt", IsoNow()}, {"requests", seq.load()}});
    }
    void Loop() {
        while (!stop) { pollfd p{lfd, POLLIN, 0}; if (poll(&p, 1, 200) <= 0) continue; int c = accept(lfd, nullptr, nullptr); if (c < 0) continue;
            ++active; std::thread([this, c] { Handle(c); close(c); --active; }).detach(); }
    }
    void Handle(int c) {
        std::string req; char buf[8192];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) { auto n = recv(c, buf, sizeof buf, 0); if (n <= 0) return; req.append(buf, static_cast<size_t>(n)); }
        const auto he = req.find("\r\n\r\n"); if (he == std::string::npos) return;
        std::istringstream rl(req.substr(0, req.find("\r\n"))); std::string method, target, ver; rl >> method >> target >> ver;
        const auto t0 = NowMs(); const int n = ++seq;
        json ev{{"sessionId", session}, {"seq", n}, {"ts", t0}, {"method", method}};
        if (method == "CONNECT") {
            const auto colon = target.rfind(':'); const auto h = target.substr(0, colon), pt = colon == std::string::npos ? "443" : target.substr(colon + 1);
            ev["kind"] = "connect"; ev["url"] = "https://" + target; ev["host"] = h;
            int u = Dial(h, pt);
            if (u < 0) { SendAll(c, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n"); ev["status"] = 502; ev["error"] = "connect failed"; CapAppend("events.jsonl", ev); return; }
            SendAll(c, "HTTP/1.1 200 Connection Established\r\n\r\n");
            const auto extra = req.substr(he + 4); if (!extra.empty()) SendAll(u, extra);
            auto io = Relay(c, u, nullptr, 0); close(u);
            ev["status"] = 200; ev["bytesOut"] = io.first + extra.size(); ev["bytesIn"] = io.second; ev["durationMs"] = NowMs() - t0; CapAppend("events.jsonl", ev);
            return;
        }
        std::smatch m;
        if (!std::regex_match(target, m, std::regex(R"(^http://([^/:]+)(?::(\d+))?(/.*)?$)"))) {
            SendAll(c, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            ev["status"] = 400; ev["url"] = target; ev["error"] = "not an absolute http:// URI (explicit proxy)"; CapAppend("events.jsonl", ev); return;
        }
        const std::string h = m[1], pt = m[2].matched ? m[2].str() : "80", path = m[3].matched ? m[3].str() : "/";
        ev["kind"] = "http"; ev["url"] = target; ev["host"] = h;
        std::string head = req.substr(0, he), rest = req.substr(he + 4), out = method + " " + path + " " + (ver.empty() ? "HTTP/1.1" : ver) + "\r\n"; size_t clen = 0;
        std::istringstream hs(head); std::string line; std::getline(hs, line);
        while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto k = Lower(line.substr(0, line.find(':')));
            if (k == "proxy-connection" || k == "proxy-authorization" || k == "connection" || k == "keep-alive") continue;
            if (k == "content-length") clen = std::strtoul(line.substr(line.find(':') + 1).c_str(), nullptr, 10);
            out += line + "\r\n";
        }
        out += "Connection: close\r\n\r\n";
        while (rest.size() < clen) { auto k = recv(c, buf, sizeof buf, 0); if (k <= 0) break; rest.append(buf, static_cast<size_t>(k)); }
        ev["requestBlob"] = CapBlob(rest); ev["requestBytes"] = rest.size(); ev["requestSha256"] = Sha256(method + " " + target + "\n" + rest);
        int u = Dial(h, pt);
        if (u < 0) { SendAll(c, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); ev["status"] = 502; ev["error"] = "upstream connect failed"; ev["durationMs"] = NowMs() - t0; CapAppend("events.jsonl", ev); return; }
        SendAll(u, out + rest);
        std::string resp; Relay(c, u, &resp, 4u << 20); close(u);
        int status = 0; if (resp.rfind("HTTP/", 0) == 0 && resp.find(' ') != std::string::npos) status = std::atoi(resp.substr(resp.find(' ') + 1, 3).c_str());
        const auto rb = resp.find("\r\n\r\n");
        ev["status"] = status; ev["responseBlob"] = CapBlob(rb == std::string::npos ? resp : resp.substr(rb + 4)); ev["responseBytes"] = resp.size(); ev["durationMs"] = NowMs() - t0;
        if (status == 0) ev["error"] = "no response";
        CapAppend("events.jsonl", ev);
    }
};
std::atomic<bool> g_proxy_sig{false};
void OnSig(int) { g_proxy_sig = true; }
json ProxyQuery(const std::string& preset, const std::string& sid) {
    std::vector<json> ev; for (auto& e : CapRead("events.jsonl")) if (sid.empty() || e.value("sessionId", "") == sid) ev.push_back(e);
    json rows = json::array();
    auto window = [&](const std::function<std::string(const json&)>& key, long long ms, size_t min) {
        std::map<std::string, std::vector<json>> by; for (auto& e : ev) { auto k = key(e); if (!k.empty()) by[k].push_back(e); }
        for (auto& [k, v] : by) for (size_t i = 0; i < v.size(); ++i) {
            size_t j = i; while (j + 1 < v.size() && v[j + 1].value("ts", 0LL) - v[i].value("ts", 0LL) <= ms) ++j;
            if (j - i + 1 >= min) { json seqs = json::array(); for (size_t x = i; x <= j; ++x) seqs.push_back(v[x]["seq"]);
                rows.push_back({{"preset", preset}, {"key", k}, {"count", j - i + 1}, {"sessionId", v[i].value("sessionId", "")}, {"seqs", seqs}, {"url", v[i].value("url", "")}}); i = j; }
        }
    };
    if (preset == "double-sends") window([](const json& e) { return e.value("kind", "") == "http" ? e.value("requestSha256", "") : std::string(); }, 2000, 2);
    else if (preset == "retry-storms") window([](const json& e) { return e.value("method", "") + " " + e.value("url", ""); }, 10000, 3);
    else if (preset == "cache-busting") {
        std::map<std::string, std::set<std::string>> by;
        for (auto& e : ev) { auto u = e.value("url", ""); auto qp = u.find('?'); if (qp == std::string::npos) continue; by[e.value("method", "") + " " + u.substr(0, qp)].insert(u.substr(qp)); }
        for (auto& [k, v] : by) if (v.size() >= 3) rows.push_back({{"preset", preset}, {"key", k}, {"distinctQueries", v.size()}});
    } else if (preset == "missing-ack") { for (auto& e : ev) if (e.value("status", 0) == 0 || e.contains("error")) rows.push_back({{"preset", preset}, {"seq", e["seq"]}, {"url", e.value("url", "")}, {"error", e.value("error", "")}, {"sessionId", e.value("sessionId", "")}}); }
    else if (preset == "error-bursts") window([](const json& e) { const int s = e.value("status", 0); return s >= 500 || s == 0 ? e.value("host", "") : std::string(); }, 10000, 3);
    else if (preset != "ws-duplicate-frames") throw std::runtime_error("unknown preset " + preset + " (double-sends, retry-storms, cache-busting, ws-duplicate-frames, missing-ack, error-bursts)");
    json out{{"preset", preset}, {"sessionId", sid.empty() ? json(nullptr) : json(sid)}, {"events", ev.size()}, {"rows", rows}};
    if (preset == "ws-duplicate-frames") out["note"] = "WebSocket frames are relayed opaquely; frame-level capture is not available in openclaw-cpp";
    return out;
}
int CmdProxy(int argc, char** argv) {
    const std::string sub = argc >= 3 ? argv[2] : "";
    if (sub == "start" || sub == "run") {
        std::string host = "127.0.0.1"; int port = 0; std::vector<std::string> cmd; int i = 3;
        for (; i < argc; ++i) {
            std::string s = argv[i];
            if (s == "--") { ++i; break; }
            const bool hf = s == "--host" || s.rfind("--host=", 0) == 0, pf = s == "--port" || s.rfind("--port=", 0) == 0;
            if (hf || pf) {
                std::string v; if (s.find('=') != std::string::npos) v = s.substr(s.find('=') + 1); else if (i + 1 < argc) v = argv[++i]; else { std::cerr << "error: option '" << s << " <value>' argument missing\n"; return 2; }
                if (hf) host = v;
                else { try { size_t pos = 0; port = std::stoi(v, &pos); if (pos != v.size()) throw std::invalid_argument("x"); } catch (...) { std::cerr << "--port must be an integer.\n"; return 2; }
                       if (port < 0 || port > 65535) { std::cerr << "--port must be between 0 and 65535.\n"; return 2; } }
                continue;
            }
            if (sub == "start") { std::cerr << "error: unknown option '" << s << "'\n"; return 2; }
            break;
        }
        for (; i < argc; ++i) cmd.push_back(argv[i]);
        if (sub == "run" && cmd.empty()) { std::cerr << "proxy run: pass the command to run after --\n"; return 2; }
        DebugProxy px; px.host = host; px.port = port;
        if (!px.Start()) { std::cerr << "debug proxy: could not bind " << host << ":" << port << "\n"; return 1; }
        const auto url = "http://" + (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(px.port);
        if (sub == "start") {
            std::cout << "OpenClaw debug proxy listening on " << url << " (session " << px.session << ")\nCapture: " << CaptureDir().string()
                      << "\nUse: HTTP_PROXY=" << url << " HTTPS_PROXY=" << url << " <command>   (Ctrl-C to stop)" << std::endl;
            signal(SIGINT, OnSig); signal(SIGTERM, OnSig);
            while (!g_proxy_sig) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            px.Stop(); std::cout << "debug proxy stopped (" << px.seq.load() << " requests captured)" << std::endl; return 0;
        }
        std::cout.flush();
        pid_t pid = fork();
        if (pid == 0) {
            for (auto k : {"HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy", "ALL_PROXY", "all_proxy"}) setenv(k, url.c_str(), 1);
            unsetenv("NO_PROXY"); unsetenv("no_proxy");
            setenv("OPENCLAW_DEBUG_PROXY_URL", url.c_str(), 1); setenv("OPENCLAW_DEBUG_PROXY_SESSION", px.session.c_str(), 1);
            std::vector<char*> av; for (auto& s : cmd) av.push_back(const_cast<char*>(s.c_str())); av.push_back(nullptr);
            execvp(av[0], av.data()); std::perror("proxy run: exec"); _exit(127);
        }
        int st = 0; waitpid(pid, &st, 0); px.Stop();
        std::cerr << "[debug-proxy] session " << px.session << ": " << px.seq.load() << " requests captured\n";
        return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }
    if (sub == "coverage") {
        auto a = P3(argc, argv, 3, {"--json"}, "proxy coverage"); if (!a) return 2;
        json out{{"transports", json::array({{{"transport", "http"}, {"coverage", "full"}, {"detail", "absolute-form HTTP/1.1: method, URL, status, timing, request/response bodies as blobs"}},
                                             {{"transport", "https"}, {"coverage", "metadata"}, {"detail", "CONNECT tunnels: host, port, byte counts, duration (no TLS interception)"}},
                                             {{"transport", "websocket"}, {"coverage", "metadata"}, {"detail", "ws:// upgrades and wss:// tunnels are relayed; frames are not decoded"}}})},
                 {"gaps", json::array({"TLS payload capture (needs a MITM CA; deliberately not implemented)", "WebSocket frame-level capture", "HTTP/2 and HTTP/3", "clients that ignore HTTP(S)_PROXY"})},
                 {"captureDir", CaptureDir().string()}};
        Emit(a->has("--json"), out, [&] {
            for (auto& t : out["transports"]) std::cout << t["transport"].get<std::string>() << ": " << t["coverage"].get<std::string>() << " - " << t["detail"].get<std::string>() << "\n";
            std::cout << "gaps:\n"; for (auto& g : out["gaps"]) std::cout << "  - " << g.get<std::string>() << "\n"; });
        return 0;
    }
    if (sub == "sessions") {
        auto a = P3(argc, argv, 3, {"--json", "--limit="}, "proxy sessions"); if (!a) return 2;
        int lim = 20; if (a->has("--limit")) { try { lim = std::stoi(a->get("--limit")); } catch (...) { lim = 0; } if (lim <= 0) { std::cerr << "--limit must be a positive integer.\n"; return 2; } }
        std::map<std::string, json> by; std::vector<std::string> order;
        for (auto& e : CapRead("sessions.jsonl")) {
            auto id = e.value("id", ""); if (!by.count(id)) { order.push_back(id); by[id] = {{"id", id}, {"requests", 0}}; }
            if (e.value("event", "") == "start") { by[id]["startedAt"] = e["startedAt"]; by[id]["host"] = e["host"]; by[id]["port"] = e["port"]; } else by[id]["endedAt"] = e.value("endedAt", "");
        }
        for (auto& e : CapRead("events.jsonl")) { auto s = e.value("sessionId", ""); if (by.count(s)) by[s]["requests"] = by[s]["requests"].get<int>() + 1; }
        json out = json::array(); for (auto it = order.rbegin(); it != order.rend() && static_cast<int>(out.size()) < lim; ++it) out.push_back(by[*it]);
        Emit(a->has("--json"), out, [&] { if (out.empty()) { std::cout << "No capture sessions.\n"; return; }
            for (auto& s : out) std::cout << s["id"].get<std::string>() << "  " << s.value("startedAt", "") << "  " << s["requests"] << " requests" << (s.contains("endedAt") ? "" : "  (running)") << "\n"; });
        return 0;
    }
    if (sub == "query") {
        auto a = P3(argc, argv, 3, {"--json", "--preset=", "--session="}, "proxy query"); if (!a) return 2;
        if (a->get("--preset").empty()) { std::cerr << "error: required option '--preset <name>' not specified\n"; return 2; }
        try {
            auto out = ProxyQuery(a->get("--preset"), a->get("--session"));
            Emit(a->has("--json"), out, [&] { std::cout << out["preset"].get<std::string>() << ": " << out["rows"].size() << " finding(s) over " << out["events"] << " event(s)\n";
                for (auto& r : out["rows"]) std::cout << "  " << r.dump() << "\n"; if (out.contains("note")) std::cout << out["note"].get<std::string>() << "\n"; });
        } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 2; }
        return 0;
    }
    if (sub == "blob") {
        auto a = P3(argc, argv, 3, {"--id="}, "proxy blob"); if (!a) return 2;
        const auto id = a->get("--id"); if (id.empty()) { std::cerr << "error: required option '--id <blobId>' not specified\n"; return 2; }
        if (!std::regex_match(id, std::regex("^blob-[0-9a-f]{24}$"))) { std::cerr << "invalid blob id: " << id << "\n"; return 1; }
        std::ifstream f(CaptureDir() / "blobs" / id, std::ios::binary); if (!f) { std::cerr << "blob not found: " << id << "\n"; return 1; }
        std::cout << f.rdbuf(); return 0;
    }
    if (sub == "purge") {
        auto a = P3(argc, argv, 3, {}, "proxy purge"); if (!a) return 2;
        std::error_code ec; const auto n = fs::exists(CaptureDir(), ec) ? fs::remove_all(CaptureDir(), ec) : 0;
        std::cout << "Purged debug proxy capture (" << n << " entries) at " << CaptureDir().string() << "\n"; return 0;
    }
    if (sub == "validate") {
        auto a = P3(argc, argv, 3, {"--json", "--proxy-url=", "--proxy-ca-file=", "--allowed-url=", "--denied-url=", "--apns-reachable", "--apns-authority=", "--timeout-ms="}, "proxy validate"); if (!a) return 2;
        long tmo = 10000; if (a->has("--timeout-ms")) { try { tmo = std::stol(a->get("--timeout-ms")); } catch (...) { tmo = 0; } if (tmo <= 0) { std::cerr << "--timeout-ms must be a positive integer.\n"; return 2; } }
        const auto raw = LoadRaw(); std::string purl = a->get("--proxy-url"), source = "flag";
        if (purl.empty()) for (auto p : {"network.proxy.url", "proxy.url", "gateway.proxy.url"}) if (auto v = config::GetPath(raw, p); v && v->is_string()) { purl = v->get<std::string>(); source = std::string("config ") + p; break; }
        if (purl.empty()) for (auto k : {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy", "ALL_PROXY"}) if (const char* e = std::getenv(k); e && *e) { purl = e; source = std::string("env ") + k; break; }
        if (purl.empty()) { json out{{"ok", false}, {"error", "no proxy configured (pass --proxy-url, set network.proxy.url, or HTTPS_PROXY)"}};
            Emit(a->has("--json"), out, [&] { std::cerr << out["error"].get<std::string>() << "\n"; }); return 1; }
        std::smatch m;
        if (!std::regex_match(purl, m, std::regex(R"(^(https?)://(?:[^@/]+@)?([^/:\[]+|\[[^\]]+\])(?::(\d+))?/?$)"))) { std::cerr << "invalid proxy URL: " << purl << "\n"; return 1; }
        json checks = json::array(); bool ok = true;
        { const auto h = m[2].str(); const auto pt = m[3].matched ? m[3].str() : (m[1] == "https" ? "443" : "80");
          int fd = Dial(h[0] == '[' ? h.substr(1, h.size() - 2) : h, pt);
          checks.push_back({{"check", "proxy-reachable"}, {"target", h + ":" + pt}, {"ok", fd >= 0}}); if (fd >= 0) close(fd); else ok = false; }
        auto res = [&](const char* kind, const std::string& u, bool good, const Resp& r) { checks.push_back({{"check", kind}, {"url", u}, {"ok", good}, {"status", r.status}, {"error", r.error.empty() ? json(nullptr) : json(r.error)}}); ok = ok && good; };
        for (auto& u : a->all("--allowed-url")) { auto r = Http(u, {}, tmo, purl, a->get("--proxy-ca-file")); res("allowed", u, r.error.empty() && r.status > 0 && r.status != 403 && r.status != 407 && r.status < 500, r); }
        for (auto& u : a->all("--denied-url")) { auto r = Http(u, {}, tmo, purl, a->get("--proxy-ca-file")); res("denied", u, !r.error.empty() || r.status == 403 || r.status == 407 || r.status == 0 || r.status >= 500, r); }
        if (a->has("--apns-reachable")) { const auto au = a->get("--apns-authority", "https://api.sandbox.push.apple.com"); auto r = Http(au, {}, tmo, purl, a->get("--proxy-ca-file")); res("apns-reachable", au, r.status > 0, r); }
        json out{{"ok", ok}, {"proxyUrl", std::regex_replace(purl, std::regex(R"(//[^@/]+@)"), "//***@")}, {"source", source}, {"checks", checks}};
        Emit(a->has("--json"), out, [&] { std::cout << "Proxy " << out["proxyUrl"].get<std::string>() << " (" << source << "): " << (ok ? "OK" : "FAILED") << "\n";
            for (auto& c : checks) std::cout << "  [" << (c["ok"].get<bool>() ? "ok" : "FAIL") << "] " << c["check"].get<std::string>() << " " << (c.contains("url") ? c["url"].get<std::string>() : c["target"].get<std::string>())
                                             << (c.contains("status") ? " (HTTP " + std::to_string(c["status"].get<long>()) + ")" : "") << "\n"; });
        return ok ? 0 : 1;
    }
    std::cerr << "usage: openclaw-cpp proxy start|run|validate|coverage|sessions|query|blob|purge\n"; return 1;
}

// ============================================================ database: SQLite preflight (pure C++ reader) + shared-state ownership
constexpr int kStateSchemaVersion = 20, kAgentSchemaVersion = 25;
struct Sqlite {
    std::string d; uint32_t page = 0, usable = 0;
    uint32_t U32(size_t o) const { if (o + 4 > d.size()) throw std::runtime_error("truncated file"); return (uint32_t(uint8_t(d[o])) << 24) | (uint32_t(uint8_t(d[o + 1])) << 16) | (uint32_t(uint8_t(d[o + 2])) << 8) | uint8_t(d[o + 3]); }
    uint16_t U16(size_t o) const { if (o + 2 > d.size()) throw std::runtime_error("truncated file"); return uint16_t((uint8_t(d[o]) << 8) | uint8_t(d[o + 1])); }
    uint64_t Var(size_t& o) const { uint64_t v = 0; for (int i = 0; i < 9; ++i) { uint8_t b = uint8_t(d.at(o++)); if (i == 8) return (v << 8) | b; v = (v << 7) | (b & 0x7f); if (!(b & 0x80)) break; } return v; }
    size_t PageOff(uint32_t n) const { return size_t(n - 1) * page; }
    std::string Payload(uint64_t total, size_t local_off) const {
        const uint64_t X = usable - 35; if (total <= X) return d.substr(local_off, total);
        const uint64_t M = ((uint64_t(usable) - 12) * 32 / 255) - 23; const uint64_t K = M + ((total - M) % (usable - 4)); const uint64_t local = K <= X ? K : M;
        std::string out = d.substr(local_off, local); uint32_t ov = U32(local_off + local); int guard = 0;
        while (ov && out.size() < total && ++guard < 1000000) { const auto o = PageOff(ov); out += d.substr(o + 4, std::min<uint64_t>(usable - 4, total - out.size())); ov = U32(o); }
        return out;
    }
    void Walk(uint32_t pg, std::vector<std::string>& rows, int depth = 0) const {
        if (depth > 32 || pg == 0 || PageOff(pg) + page > d.size()) throw std::runtime_error("corrupt b-tree page " + std::to_string(pg));
        const size_t base = PageOff(pg), hdr = pg == 1 ? 100 : 0; const uint8_t type = uint8_t(d[base + hdr]); const uint16_t n = U16(base + hdr + 3);
        if (type != 0x05 && type != 0x0D) throw std::runtime_error("unexpected b-tree page type " + std::to_string(type));
        const size_t ptrs = base + hdr + (type == 0x05 ? 12 : 8);
        for (uint16_t i = 0; i < n; ++i) {
            size_t c = base + U16(ptrs + 2 * i);
            if (type == 0x05) { Walk(U32(c), rows, depth + 1); continue; }
            size_t o = c; const uint64_t total = Var(o); Var(o); rows.push_back(Payload(total, o));
        }
        if (type == 0x05) Walk(U32(base + hdr + 8), rows, depth + 1);
    }
    static std::vector<json> Record(const std::string& r) {
        Sqlite t; t.d = r; size_t o = 0; const uint64_t hs = t.Var(o); std::vector<uint64_t> types; while (o < hs) types.push_back(t.Var(o));
        std::vector<json> v; size_t p = hs;
        for (auto st : types) {
            if (st == 0) v.push_back(nullptr); else if (st == 8) v.push_back(0); else if (st == 9) v.push_back(1);
            else if (st >= 1 && st <= 6) { static const int sz[] = {0, 1, 2, 3, 4, 6, 8}; int64_t x = int8_t(r.at(p)); for (int i = 1; i < sz[st]; ++i) x = (x << 8) | uint8_t(r.at(p + i)); v.push_back(x); p += sz[st]; }
            else if (st == 7) { v.push_back(nullptr); p += 8; }
            else if (st >= 12) { const size_t len = (st - (st % 2 ? 13 : 12)) / 2; v.push_back(st % 2 ? json(r.substr(p, len)) : json(nullptr)); p += len; }
        }
        return v;
    }
    static std::vector<std::string> Columns(const std::string& sql) {
        std::vector<std::string> cols; auto a = sql.find('('), b = sql.rfind(')'); if (a == std::string::npos || b == std::string::npos) return cols;
        std::string body = sql.substr(a + 1, b - a - 1), cur; int depth = 0;
        auto flush = [&] { auto s = Trim(cur); cur.clear(); if (s.empty()) return; std::string w; for (char c : s) { if (std::isspace(static_cast<unsigned char>(c))) break; w += c; }
            w.erase(std::remove(w.begin(), w.end(), '"'), w.end()); w.erase(std::remove(w.begin(), w.end(), '`'), w.end());
            const auto lw = Lower(w); if (lw == "primary" || lw == "unique" || lw == "check" || lw == "foreign" || lw == "constraint") return; cols.push_back(w); };
        for (char c : body) { if (c == '(') ++depth; if (c == ')') --depth; if (c == ',' && depth == 0) flush(); else cur += c; }
        flush(); return cols;
    }
};
json Preflight(const std::string& path, int target, const std::string& agent_id) {
    std::error_code ec; const auto abs = fs::absolute(path, ec).lexically_normal().string();
    json res{{"schema", agent_id.empty() ? "openclaw.state-schema-preflight.v1" : "openclaw.agent-schema-preflight.v1"}, {"databasePath", abs}, {"targetVersion", target},
             {"foundVersion", nullptr}, {"issues", json::array()}, {"requiresWrite", false}};
    if (agent_id.empty()) res["ownership"] = nullptr; else res["agentId"] = agent_id;
    auto bad = [&](const std::string& status, const std::string& reason) { res["status"] = status; res["reason"] = reason; return res; };
    if (!agent_id.empty() && (agent_id != Lower(Trim(agent_id)) || !std::regex_match(agent_id, std::regex("^[a-z0-9][a-z0-9_-]{0,63}$")))) return bad("indeterminate", "Agent preflight requires an explicit canonical agent ID.");
    if (!fs::is_regular_file(abs, ec)) return bad("indeterminate", "database file not found: " + abs);
    std::vector<std::string> side; for (auto s : {"-wal", "-shm", "-journal"}) if (fs::exists(abs + s, ec)) side.push_back(s);
    if (!side.empty()) { std::string j; for (auto& s : side) j += (j.empty() ? "" : ", ") + s;
        return bad("indeterminate", "SQLite preflight requires a consolidated snapshot with no sidecars; found " + j + ". Create a WAL-aware online backup and preflight the resulting standalone file."); }
    Sqlite db; { std::ifstream f(abs, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); db.d = ss.str(); }
    if (db.d.size() < 100 || db.d.compare(0, 16, std::string("SQLite format 3\0", 16)) != 0) return bad("indeterminate", "not a SQLite 3 database");
    db.page = db.U16(16) == 1 ? 65536 : db.U16(16); db.usable = db.page - uint8_t(db.d[20]);
    const int found = static_cast<int>(db.U32(60)); res["foundVersion"] = found;
    std::map<std::string, std::pair<uint32_t, std::string>> tables;
    try { std::vector<std::string> rows; db.Walk(1, rows);
          for (auto& r : rows) { auto v = Sqlite::Record(r); if (v.size() >= 5 && v[0] == "table" && v[1].is_string()) tables[v[1].get<std::string>()] = {v[3].is_number() ? v[3].get<uint32_t>() : 0, v[4].is_string() ? v[4].get<std::string>() : ""}; } }
    catch (const std::exception& e) { return bad("indeterminate", std::string("could not read the schema: ") + e.what()); }
    json tl = json::array(); for (auto& [k, v] : tables) if (k.rfind("sqlite_", 0) != 0) tl.push_back(k); res["tables"] = tl;
    auto rowsOf = [&](const std::string& t) { std::vector<std::map<std::string, json>> out; auto it = tables.find(t); if (it == tables.end()) return out;
        const auto cols = Sqlite::Columns(it->second.second); std::vector<std::string> raw; db.Walk(it->second.first, raw);
        for (auto& r : raw) { auto v = Sqlite::Record(r); std::map<std::string, json> m; for (size_t i = 0; i < cols.size() && i < v.size(); ++i) m[cols[i]] = v[i]; out.push_back(m); }
        return out; };
    if (found == 0 && tl.empty()) { res["requiresWrite"] = true; return bad(agent_id.empty() ? "startup-repairable" : "incompatible", "empty database; the runtime creates the schema on first start"); }
    if (found > target) return bad("incompatible", "database was written by a newer release (schema " + std::to_string(found) + " > " + std::to_string(target) + ")");
    try {
        if (!agent_id.empty()) {
            std::string owner; for (auto& r : rowsOf("schema_meta")) if (r["meta_key"] == "primary" && r["agent_id"].is_string()) owner = r["agent_id"];
            if (owner.empty()) return bad("incompatible", "schema_meta has no primary owner row");
            res["ownerAgentId"] = owner;
            if (owner != agent_id) return bad("incompatible", "database belongs to agent " + owner + ", not " + agent_id);
            if (found < target) return bad("incompatible", "agent schema " + std::to_string(found) + " needs a maintenance migration to " + std::to_string(target));
        } else {
            for (auto& r : rowsOf("config_machine_state")) if (r["state_key"] == "state-ownership" && r["value_json"].is_string()) { try { res["ownership"] = json::parse(r["value_json"].get<std::string>()); } catch (...) {} }
            if (found < target) { res["requiresWrite"] = true; res["status"] = "migration-required"; return res; }
        }
    } catch (const std::exception& e) { return bad("indeterminate", std::string("could not read table rows: ") + e.what()); }
    res["status"] = "exact"; return res;
}
fs::path OwnershipPath() { return DataRoot() / "state-ownership.json"; }
int CmdDatabase(int argc, char** argv) {
    const std::string sub = argc >= 3 ? argv[2] : "";
    if (sub == "preflight" || sub == "preflight-agent") {
        std::vector<std::string> al{"--json"}; if (sub == "preflight-agent") al.push_back("--agent-id=");
        auto a = P3(argc, argv, 3, al, "database " + sub); if (!a) return 2;
        if (a->pos.empty()) { std::cerr << "error: missing required argument 'path'\n"; return 2; }
        if (sub == "preflight-agent" && !a->has("--agent-id")) { std::cerr << "error: required option '--agent-id <id>' not specified\n"; return 2; }
        auto r = Preflight(a->pos[0], sub == "preflight" ? kStateSchemaVersion : kAgentSchemaVersion, sub == "preflight" ? "" : a->opt["--agent-id"].back());
        Emit(a->has("--json"), r, [&] { std::cout << "Database preflight: " << r["status"].get<std::string>() << " (found " << (r["foundVersion"].is_null() ? std::string("unknown") : std::to_string(r["foundVersion"].get<int>()))
                                               << ", target " << r["targetVersion"] << ")." << (r.contains("reason") ? "\n" + r["reason"].get<std::string>() : "") << "\nSee https://docs.openclaw.ai/reference/database-schema.\n"; });
        const auto st = r["status"].get<std::string>(); return st == "incompatible" || st == "indeterminate" ? 1 : 0;
    }
    if (sub == "ownership" || sub == "claim") {
        int from = 3; std::string act = sub == "claim" ? "claim" : "status";
        if (sub == "ownership" && argc >= 4 && argv[3][0] != '-') { act = argv[3]; from = 4; }
        if (act != "status" && act != "claim") { std::cerr << "error: unknown command '" << act << "' for 'database ownership'\n"; return 1; }
        auto a = P3(argc, argv, from, act == "claim" ? std::vector<std::string>{"--manager=", "--json"} : std::vector<std::string>{"--json"}, "database ownership " + act); if (!a) return 2;
        const auto p = OwnershipPath(); json out{{"databasePath", p.string()}};
        if (act == "claim") {
            const auto mgr = Trim(a->get("--manager"));
            if (mgr.empty()) { std::cerr << "error: required option '--manager <id>' not specified\n"; return 2; }
            if (!std::regex_match(mgr, std::regex(R"(^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$)"))) { std::cerr << "invalid manager id: " << mgr << "\n"; return 1; }
            auto cur = ReadJ(p, json(nullptr));
            if (cur.is_object() && cur.value("managerId", "") != mgr) { std::cerr << "Shared state is already externally owned by " << cur.value("managerId", "") << ".\n"; return 1; }
            if (!cur.is_object()) { cur = {{"version", 1}, {"mode", "external"}, {"managerId", mgr}, {"claimedAt", IsoNow()}}; WriteJ(p, cur); }
            out["status"] = "external"; out["ownership"] = cur;
        } else { auto cur = ReadJ(p, json(nullptr)); if (cur.is_object()) { out["status"] = "external"; out["ownership"] = cur; } else out["status"] = "unowned"; }
        Emit(a->has("--json"), out, [&] { std::cout << (out["status"] == "external" ? "Shared state is externally owned by " + out["ownership"].value("managerId", "") + "." : std::string("Shared state is not externally owned."))
                                                << "\nSee https://docs.openclaw.ai/reference/database-schema.\n"; });
        return 0;
    }
    return -1;
}

// ============================================================ update: wizard / cleanup / recover / migration-plan / admit / adopt-immutable
fs::path UpdDir() { return StateRoot() / "update"; }
fs::path SelfExe() { std::error_code ec; auto p = fs::read_symlink("/proc/self/exe", ec); return ec ? fs::path() : p; }
std::string Line() { std::string s; if (!std::getline(std::cin, s)) return {}; return Trim(s); }
int CmdUpdate(int argc, char** argv) {
    const std::string sub = argc >= 3 ? argv[2] : "";
    std::error_code ec;
    if (sub == "wizard") {
        auto a = P3(argc, argv, 3, {"--accept-capabilities", "--timeout="}, "update wizard"); if (!a) return 2;
        auto raw = LoadRaw();
        const auto cur = config::GetPath(raw, "update.channel"); const std::string cc = cur && cur->is_string() ? cur->get<std::string>() : "stable";
        std::cout << "Update channel [stable/beta/dev] (current " << cc << "): " << std::flush; auto ch = Line(); if (ch.empty()) ch = cc;
        if (ch != "stable" && ch != "beta" && ch != "dev") { std::cerr << "invalid channel: " << ch << "\n"; return 1; }
        std::cout << "Check for updates automatically on start? [Y/n]: " << std::flush; const auto au = Lower(Line()); const bool autoc = !(au == "n" || au == "no");
        if (!a->has("--accept-capabilities")) { std::cout << "Updates replace installed binaries and service files. Continue? [y/N]: " << std::flush; const auto y = Lower(Line()); if (y != "y" && y != "yes") { std::cout << "Cancelled; nothing changed.\n"; return 1; } }
        config::SetPath(raw, "update.channel", ch); config::SetPath(raw, "update.checkOnStart", autoc); SaveRaw(raw);
        std::cout << "Saved update.channel=" << ch << ", update.checkOnStart=" << (autoc ? "true" : "false") << ". Run `openclaw-cpp update run` to update now.\n"; return 0;
    }
    if (sub == "cleanup") {
        auto a = P3(argc, argv, 3, {"--dry-run", "--json", "--yes"}, "update cleanup"); if (!a) return 2;
        std::vector<fs::path> cand;
        for (auto d : {UpdDir() / "staging", UpdDir() / "downloads", UpdDir() / "tmp"}) if (fs::is_directory(d, ec)) for (auto& e : fs::directory_iterator(d, ec)) cand.push_back(e.path());
        if (auto exe = SelfExe(); !exe.empty() && fs::is_directory(exe.parent_path(), ec))
            for (auto& e : fs::directory_iterator(exe.parent_path(), ec)) { const auto n = e.path().filename().string(), b = exe.filename().string();
                if (n.rfind(b + ".old", 0) == 0 || n.rfind(b + ".bak", 0) == 0 || n.rfind(b + ".update-", 0) == 0) cand.push_back(e.path()); }
        json items = json::array(); uintmax_t bytes = 0;
        for (auto& p : cand) { uintmax_t sz = 0;
            if (fs::is_directory(p, ec)) { for (auto& x : fs::recursive_directory_iterator(p, ec)) if (x.is_regular_file(ec)) sz += x.file_size(ec); } else sz = fs::file_size(p, ec);
            bytes += sz; items.push_back({{"path", p.string()}, {"bytes", sz}}); }
        bool go = !a->has("--dry-run") && !items.empty();
        if (go && !a->has("--yes")) { if (a->has("--json")) go = false; else { std::cout << "Remove " << items.size() << " stale update artifact(s) (" << bytes << " bytes)? [y/N]: " << std::flush; const auto y = Lower(Line()); go = y == "y" || y == "yes"; } }
        if (go) for (auto& p : cand) fs::remove_all(p, ec);
        json out{{"dryRun", a->has("--dry-run")}, {"removed", go}, {"items", items}, {"bytes", bytes}};
        Emit(a->has("--json"), out, [&] { if (items.empty()) { std::cout << "No stale update artifacts.\n"; return; } for (auto& i : items) std::cout << (go ? "removed " : "would remove ") << i["path"].get<std::string>() << "\n"; });
        return 0;
    }
    if (sub == "recover") {
        auto a = P3(argc, argv, 3, {"--root=", "--timeout=", "--drain-timeout=", "--json"}, "update recover"); if (!a) return 2;
        const fs::path root = a->get("--root").empty() ? UpdDir() : fs::path(a->get("--root"));
        const auto jp = root / "journal.json"; auto j = ReadJ(jp, json(nullptr));
        json out{{"root", root.string()}};
        if (!j.is_object() || j.value("phase", "") == "done") out["status"] = "clean";
        else {
            const fs::path target = j.value("target", std::string()), prev = j.value("previous", std::string());
            if (target.empty() || prev.empty() || !fs::is_regular_file(prev, ec)) { out["status"] = "unrecoverable"; out["reason"] = "journal has no usable previous binary"; out["journal"] = j; }
            else {
                const auto tmp = target.string() + ".recover-tmp"; fs::copy_file(prev, tmp, fs::copy_options::overwrite_existing, ec);
                if (!ec) { fs::permissions(tmp, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec, ec); fs::rename(tmp, target, ec); }
                if (ec) { out["status"] = "failed"; out["reason"] = ec.message(); }
                else { j["phase"] = "done"; j["recoveredAt"] = IsoNow(); j["outcome"] = "rolled-back"; WriteJ(jp, j);
                    out["status"] = "recovered"; out["restored"] = target.string(); out["abandonedVersion"] = j.value("toVersion", json(nullptr)); out["version"] = j.value("fromVersion", json(nullptr)); }
            }
        }
        Emit(a->has("--json"), out, [&] { const auto s = out["status"].get<std::string>();
            std::cout << (s == "clean" ? std::string("No interrupted update; nothing to recover.") : s == "recovered" ? "Rolled back the interrupted update: restored " + out["restored"].get<std::string>() : "Recovery " + s + ": " + out.value("reason", std::string())) << "\n"; });
        const auto s = out["status"].get<std::string>(); return s == "clean" || s == "recovered" ? 0 : 1;
    }
    if (sub == "migration-plan") {
        auto a = P3(argc, argv, 3, {"--snapshot-home=", "--snapshot-config=", "--snapshot-state=", "--dry-run", "--json"}, "update migration-plan"); if (!a) return 2;
        const fs::path home = a->get("--snapshot-home").empty() ? HomeDir() : fs::path(a->get("--snapshot-home"));
        const fs::path cfgp = !a->get("--snapshot-config").empty() ? fs::path(a->get("--snapshot-config")) : a->has("--snapshot-home") ? home / ".openclaw" / "openclaw.json" : config::DefaultPath();
        const fs::path st = !a->get("--snapshot-state").empty() ? fs::path(a->get("--snapshot-state")) : a->has("--snapshot-home") ? home / ".openclaw" : StateRoot();
        json raw = json::object();
        if (fs::exists(cfgp, ec)) { std::ifstream in(cfgp); std::stringstream ss; ss << in.rdbuf(); try { raw = config::ParseJson5(ss.str()); } catch (const std::exception& e) { std::cerr << "config snapshot is not valid JSON5: " << e.what() << "\n"; return 1; } }
        const auto rep = config::ValidateSchema(raw);
        json steps = json::array();
        for (auto& w : rep.warnings) steps.push_back({{"kind", "config"}, {"action", "review"}, {"detail", w}, {"blocking", false}});
        for (auto& e : rep.errors) steps.push_back({{"kind", "config"}, {"action", "fix"}, {"detail", e}, {"blocking", true}});
        for (auto name : {"openclaw.sqlite", "state.sqlite"}) if (fs::exists(st / name, ec)) {
            auto pf = Preflight((st / name).string(), kStateSchemaVersion, "");
            steps.push_back({{"kind", "state-db"}, {"path", (st / name).string()}, {"action", pf["status"]}, {"detail", pf.value("reason", std::string("schema ") + pf["foundVersion"].dump() + " -> " + std::to_string(kStateSchemaVersion))},
                             {"blocking", pf["status"] == "incompatible" || pf["status"] == "indeterminate"}}); }
        bool blocking = false; for (auto& s : steps) blocking = blocking || s.value("blocking", false);
        json out{{"dryRun", a->has("--dry-run")}, {"config", cfgp.string()}, {"state", st.string()}, {"steps", steps}, {"blocking", blocking}, {"targetStateSchema", kStateSchemaVersion}};
        Emit(a->has("--json"), out, [&] { if (steps.empty()) { std::cout << "No migrations needed.\n"; return; }
            for (auto& s : steps) std::cout << "  [" << s["kind"].get<std::string>() << "] " << s["action"].get<std::string>() << ": " << s["detail"].get<std::string>() << (s["blocking"].get<bool>() ? "  (blocking)" : "") << "\n"; });
        return blocking ? 1 : 0;
    }
    if (sub == "admit") {
        auto a = P3(argc, argv, 3, {"--context="}, "update admit"); if (!a) return 2;
        json ctx; try { ctx = json::parse(a->get("--context", "{}")); } catch (...) { std::cout << json{{"admitted", false}, {"reason", "invalid --context JSON"}}.dump() << "\n"; return 1; }
        json out{{"admitted", true}};
        const auto lock = ReadJ(UpdDir() / "journal.json", json(nullptr));
        if (lock.is_object() && lock.value("phase", "") != "done") out = {{"admitted", false}, {"reason", "another update is in progress (phase " + lock.value("phase", std::string("?")) + "); run `openclaw-cpp update recover`"}};
        else if (ReadJ(UpdDir() / "immutable.json", json(nullptr)).is_object() && !ctx.value("immutableActivation", false)) out = {{"admitted", false}, {"reason", "this install is managed immutably; its manager activates new releases"}};
        else if (ReadJ(OwnershipPath(), json(nullptr)).is_object() && ctx.value("managerId", std::string()) != ReadJ(OwnershipPath()).value("managerId", std::string())) out = {{"admitted", false}, {"reason", "shared state is externally owned by " + ReadJ(OwnershipPath()).value("managerId", std::string())}};
        if (ctx.contains("toVersion")) out["toVersion"] = ctx["toVersion"];
        std::cout << out.dump() << "\n"; return out["admitted"].get<bool>() ? 0 : 1;
    }
    if (sub == "adopt-immutable") {
        auto a = P3(argc, argv, 3, {"--root=", "--service=", "--account=", "--state-dir=", "--config=", "--runtime=", "--profile=", "--enable-activation", "--previous-updater-stopped", "--json"}, "update adopt-immutable"); if (!a) return 2;
        if (a->get("--root").empty()) { std::cerr << "error: required option '--root <path>' not specified\n"; return 2; }
        if (!fs::is_directory(a->get("--root"), ec)) { std::cerr << "--root is not a directory: " << a->get("--root") << "\n"; return 1; }
        if (a->has("--enable-activation") && !a->has("--previous-updater-stopped")) { std::cerr << "--enable-activation requires --previous-updater-stopped (stop the previous updater first)\n"; return 1; }
        json rec{{"version", 1}, {"root", fs::absolute(a->get("--root")).lexically_normal().string()}, {"adoptedAt", IsoNow()}, {"activation", a->has("--enable-activation")}};
        for (auto k : {"--service", "--account", "--state-dir", "--config", "--runtime", "--profile"}) if (!a->get(k).empty()) rec[std::string(k).substr(2)] = a->get(k);
        WriteJ(UpdDir() / "immutable.json", rec);
        Emit(a->has("--json"), rec, [&] { std::cout << "Adopted immutable install at " << rec["root"].get<std::string>() << (rec["activation"].get<bool>() ? " (activation enabled)" : "")
                                                 << ".\nSelf-update is now refused; the immutable manager activates new releases.\n"; });
        return 0;
    }
    return -1;
}
// ============================================================ 1.123: mcp / reset / uninstall / docs
Resp HttpPost(const std::string& url, const std::vector<std::string>& headers, const std::string& body, long timeout_ms = 15000, std::map<std::string, std::string>* rh = nullptr) {
    Resp r; CURL* c = curl_easy_init(); if (!c) { r.error = "curl init"; return r; }
    curl_slist* h = nullptr; for (auto& x : headers) h = curl_slist_append(h, x.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str()); if (h) curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str()); curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms); curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); curl_easy_setopt(c, CURLOPT_PROXY, "");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, SinkS); curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    std::string hdrs; curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, SinkS); curl_easy_setopt(c, CURLOPT_HEADERDATA, &hdrs);
    auto rc = curl_easy_perform(c); if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(h); curl_easy_cleanup(c);
    if (rh) { std::istringstream is(hdrs); std::string l; while (std::getline(is, l)) { auto p = l.find(':'); if (p == std::string::npos) continue; (*rh)[Lower(l.substr(0, p))] = Trim(l.substr(p + 1)); } }
    return r;
}
std::string GwBase(const json& raw) {
    if (const char* e = std::getenv("OPENCLAW_GATEWAY_URL"); e && *e) return NoSlash(e);
    int port = 18789;
    if (const char* p = std::getenv("OPENCLAW_GATEWAY_PORT")) port = std::atoi(p);
    else if (raw.contains("gateway") && raw["gateway"].value("port", 0) > 0) port = raw["gateway"]["port"];
    return "http://127.0.0.1:" + std::to_string(port);
}
json GwRpc(const json& raw, const std::string& method, const json& params, std::string* err) {
    std::string tok; if (const char* t = std::getenv("OPENCLAW_GATEWAY_TOKEN")) tok = t;
    if (tok.empty() && raw.contains("gateway") && raw["gateway"].contains("auth") && raw["gateway"]["auth"].value("token", "") != "") tok = raw["gateway"]["auth"]["token"];
    std::vector<std::string> h{"Content-Type: application/json"}; if (!tok.empty()) h.push_back("Authorization: Bearer " + tok);
    auto r = HttpPost(GwBase(raw) + "/v1/rpc", h, json{{"id", 1}, {"method", method}, {"params", params}}.dump(), 60000);
    if (!r.error.empty()) { *err = "gateway unreachable at " + GwBase(raw) + ": " + r.error; return nullptr; }
    try { auto j = json::parse(r.body); if (!j.value("ok", false)) { *err = j.contains("error") ? (j["error"].is_object() ? j["error"].value("message", j["error"].dump()) : j["error"].dump()) : r.body; return nullptr; } return j.value("payload", json::object()); }
    catch (...) { *err = "bad gateway response (HTTP " + std::to_string(r.status) + ")"; return nullptr; }
}
fs::path McpTokPath() { return DataRoot() / "mcp-oauth-tokens.json"; }
json McpServers(const json& raw) { return raw.contains("mcp") && raw["mcp"].is_object() && raw["mcp"].contains("servers") && raw["mcp"]["servers"].is_object() ? raw["mcp"]["servers"] : json::object(); }
std::string McpTransport(const json& s) { if (s.contains("url")) return s.value("transport", std::string("streamable-http")); return "stdio"; }
std::vector<std::string> Csv(const std::string& s) { std::vector<std::string> o; std::stringstream ss(s); std::string x; while (std::getline(ss, x, ',')) if (!Trim(x).empty()) o.push_back(Trim(x)); return o; }

// JSON-RPC to a stdio MCP server: initialize + tools/list, bounded by timeout.
json ProbeStdio(const json& s, int timeout_s) {
    int in[2], out[2]; if (pipe(in) || pipe(out)) return {{"ok", false}, {"error", "pipe failed"}};
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0); dup2(out[1], 1); close(in[1]); close(out[0]);
        int dn = open("/dev/null", 1); if (dn >= 0) dup2(dn, 2);
        if (s.contains("env") && s["env"].is_object()) for (auto& [k, v] : s["env"].items()) setenv(k.c_str(), v.is_string() ? v.get<std::string>().c_str() : v.dump().c_str(), 1);
        if (s.contains("cwd") && s["cwd"].is_string()) if (chdir(s["cwd"].get<std::string>().c_str())) _exit(126);
        std::vector<std::string> av{s.value("command", "")}; if (s.contains("args") && s["args"].is_array()) for (auto& a : s["args"]) av.push_back(a.is_string() ? a.get<std::string>() : a.dump());
        std::vector<char*> cv; for (auto& a : av) cv.push_back(a.data()); cv.push_back(nullptr);
        execvp(cv[0], cv.data()); _exit(127);
    }
    close(in[0]); close(out[1]);
    auto send = [&](const json& j) { auto l = j.dump() + "\n"; return write(in[1], l.data(), l.size()) == static_cast<ssize_t>(l.size()); };
    std::string buf; const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    auto recv = [&](int id) -> json {
        for (;;) {
            auto nl = buf.find('\n');
            while (nl != std::string::npos) { auto line = buf.substr(0, nl); buf.erase(0, nl + 1); try { auto j = json::parse(line); if (j.value("id", -1) == id) return j; } catch (...) {} nl = buf.find('\n'); }
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count(); if (left <= 0) return nullptr;
            pollfd p{out[0], POLLIN, 0}; if (poll(&p, 1, static_cast<int>(left)) <= 0) return nullptr;
            char b[4096]; auto n = read(out[0], b, sizeof b); if (n <= 0) return nullptr; buf.append(b, n);
        }
    };
    json res{{"ok", false}};
    signal(SIGPIPE, SIG_IGN);
    if (send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()}, {"clientInfo", {{"name", "openclaw-cpp"}, {"version", "1"}}}}}})) {
        auto init = recv(1);
        if (init.is_null()) res["error"] = "no initialize response (server exited or timed out after " + std::to_string(timeout_s) + "s)";
        else if (init.contains("error")) res["error"] = init["error"].dump();
        else {
            send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
            send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", json::object()}});
            auto tl = recv(2); res["ok"] = true; res["serverInfo"] = init["result"].value("serverInfo", json::object());
            json names = json::array(); if (tl.is_object() && tl.contains("result")) for (auto& t : tl["result"].value("tools", json::array())) names.push_back(t.value("name", ""));
            res["tools"] = names;
        }
    } else res["error"] = "failed to start: " + s.value("command", "");
    close(in[1]); close(out[0]);
    int st = 0; for (int i = 0; i < 20 && waitpid(pid, &st, WNOHANG) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (waitpid(pid, &st, WNOHANG) == 0) { kill(pid, SIGTERM); waitpid(pid, &st, 0); }
    return res;
}
json ProbeHttp(const std::string& name, const json& s, int timeout_s) {
    std::vector<std::string> h{"Content-Type: application/json", "Accept: application/json, text/event-stream"};
    if (s.contains("headers") && s["headers"].is_object()) for (auto& [k, v] : s["headers"].items()) h.push_back(k + ": " + (v.is_string() ? v.get<std::string>() : v.dump()));
    auto toks = ReadJ(McpTokPath()); bool authed = false;
    if (toks.contains(name) && toks[name].value("access_token", "") != "") { h.push_back("Authorization: Bearer " + toks[name]["access_token"].get<std::string>()); authed = true; }
    auto parse = [](const std::string& b) -> json { try { return json::parse(b); } catch (...) {} std::istringstream is(b); std::string l; while (std::getline(is, l)) if (l.rfind("data:", 0) == 0) try { return json::parse(Trim(l.substr(5))); } catch (...) {} return nullptr; };
    std::map<std::string, std::string> rh;
    auto r = HttpPost(s["url"].get<std::string>(), h, json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()}, {"clientInfo", {{"name", "openclaw-cpp"}, {"version", "1"}}}}}}.dump(), timeout_s * 1000L, &rh);
    json res{{"ok", false}, {"authorized", authed}};
    if (!r.error.empty()) { res["error"] = r.error; return res; }
    if (r.status == 401) { res["error"] = "HTTP 401: authorization required (run `openclaw-cpp mcp login " + name + "`)"; res["needsLogin"] = true; return res; }
    auto init = parse(r.body);
    if (r.status >= 400 || !init.is_object() || init.contains("error")) { res["error"] = "initialize failed (HTTP " + std::to_string(r.status) + ")"; return res; }
    if (rh.count("mcp-session-id")) h.push_back("Mcp-Session-Id: " + rh["mcp-session-id"]);
    HttpPost(s["url"].get<std::string>(), h, json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump(), timeout_s * 1000L);
    auto t = parse(HttpPost(s["url"].get<std::string>(), h, json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", json::object()}}.dump(), timeout_s * 1000L).body);
    res["ok"] = true; res["serverInfo"] = init.value("result", json::object()).value("serverInfo", json::object());
    json names = json::array(); if (t.is_object() && t.contains("result")) for (auto& x : t["result"].value("tools", json::array())) names.push_back(x.value("name", ""));
    res["tools"] = names; return res;
}
json Probe(const std::string& name, const json& s) {
    const int to = s.contains("connectTimeout") && s["connectTimeout"].is_number() ? s["connectTimeout"].get<int>() : 10;
    if (s.value("disabled", false) || s.value("enabled", true) == false) return {{"ok", false}, {"skipped", "disabled"}};
    return s.contains("url") ? ProbeHttp(name, s, to) : ProbeStdio(s, to);
}
std::string McpValidate(const json& s) {
    if (!s.is_object()) return "server entry must be an object";
    const bool cmd = s.contains("command"), url = s.contains("url");
    if (cmd == url) return "set exactly one of \"command\" (stdio) or \"url\" (HTTP)";
    if (cmd && (!s["command"].is_string() || s["command"].get<std::string>().empty())) return "\"command\" must be a non-empty string";
    if (url && (!s["url"].is_string() || (s["url"].get<std::string>().rfind("http://", 0) && s["url"].get<std::string>().rfind("https://", 0)))) return "\"url\" must be an http(s) URL";
    if (s.contains("args") && !s["args"].is_array()) return "\"args\" must be an array";
    if (s.contains("transport") && s["transport"] != "streamable-http" && s["transport"] != "sse") return "\"transport\" must be streamable-http or sse";
    return "";
}
int CmdMcp(int argc, char** argv) {
    const std::string sub = argv[2];
    auto a = P3(argc, argv, 3, {"--json", "-v", "--verbose", "--command=", "--arg=", "--env=", "--cwd=", "--url=", "--transport=", "--header=", "--auth=", "--oauth-scope=",
                               "--oauth-redirect-url=", "--oauth-client-metadata-url=", "--include=", "--exclude=", "--timeout=", "--connect-timeout=", "--parallel",
                               "--approval=", "--disabled", "--ssl-verify=", "--client-cert=", "--client-key=", "--no-probe", "--code=", "--clear"}, "mcp " + sub);
    if (!a) return 1;
    const bool js = a->has("--json");
    json raw = LoadRaw(); json servers = McpServers(raw);
    auto need = [&](const char* what) -> std::string { if (a->pos.empty()) { std::cerr << "error: missing required argument '" << what << "'\n"; return ""; } return a->pos[0]; };
    auto save = [&](const json& sv) { raw["mcp"]["servers"] = sv; SaveRaw(raw); };
    if (sub == "list") {
        json out = json::array();
        for (auto& [k, v] : servers.items()) out.push_back({{"name", k}, {"transport", McpTransport(v)}, {"target", v.contains("url") ? v["url"] : v.value("command", json(""))}, {"enabled", !v.value("disabled", false)}});
        Emit(js, {{"path", config::DefaultPath().string()}, {"servers", out}}, [&] {
            if (out.empty()) { std::cout << "No OpenClaw-managed MCP servers configured in " << config::DefaultPath().string() << ". Add one with openclaw-cpp mcp set <name> '{\"command\":\"uvx\",\"args\":[\"context7-mcp\"]}'.\n"; return; }
            for (auto& s : out) std::cout << s["name"].get<std::string>() << "  " << s["transport"].get<std::string>() << "  " << (s["target"].is_string() ? s["target"].get<std::string>() : s["target"].dump()) << (s["enabled"] ? "" : "  (disabled)") << "\n"; });
        return 0;
    }
    if (sub == "show") {
        if (a->pos.empty()) { Emit(js, servers, [&] { std::cout << servers.dump(2) << "\n"; }); return 0; }
        if (!servers.contains(a->pos[0])) { std::cerr << "No MCP server named \"" << a->pos[0] << "\".\n"; return 1; }
        std::cout << servers[a->pos[0]].dump(2) << "\n"; return 0;
    }
    if (sub == "set" || sub == "add") {
        auto name = need("name"); if (name.empty()) return 1;
        json s;
        if (sub == "set") {
            if (a->pos.size() < 2) { std::cerr << "error: missing required argument 'value'\n"; return 1; }
            try { s = json::parse(a->pos[1]); } catch (const std::exception& e) { std::cerr << "Invalid JSON: " << e.what() << "\n"; return 1; }
        } else {
            s = json::object();
            if (a->has("--command")) { s["command"] = a->get("--command"); s["args"] = a->all("--arg"); }
            if (a->has("--url")) s["url"] = a->get("--url");
            for (auto& kv : a->all("--env")) { auto p = kv.find('='); if (p == std::string::npos) { std::cerr << "--env expects KEY=VALUE\n"; return 1; } s["env"][kv.substr(0, p)] = kv.substr(p + 1); }
            for (auto& kv : a->all("--header")) { auto p = kv.find('='); if (p == std::string::npos) { std::cerr << "--header expects KEY=VALUE\n"; return 1; } s["headers"][kv.substr(0, p)] = kv.substr(p + 1); }
            if (a->has("--cwd")) s["cwd"] = a->get("--cwd");
            if (a->has("--transport")) s["transport"] = a->get("--transport");
            if (a->has("--auth")) { if (a->get("--auth") != "oauth") { std::cerr << "--auth supports only: oauth\n"; return 1; } s["oauth"] = json::object(); }
            if (a->has("--oauth-scope")) s["oauth"]["scope"] = a->get("--oauth-scope");
            if (a->has("--oauth-redirect-url")) s["oauth"]["redirectUrl"] = a->get("--oauth-redirect-url");
            if (a->has("--oauth-client-metadata-url")) s["oauth"]["clientMetadataUrl"] = a->get("--oauth-client-metadata-url");
            if (a->has("--include")) s["include"] = Csv(a->get("--include"));
            if (a->has("--exclude")) s["exclude"] = Csv(a->get("--exclude"));
            if (a->has("--timeout")) s["timeout"] = std::atoi(a->get("--timeout").c_str());
            if (a->has("--connect-timeout")) s["connectTimeout"] = std::atoi(a->get("--connect-timeout").c_str());
            if (a->has("--parallel")) s["parallel"] = true;
            if (a->has("--approval")) { auto m = a->get("--approval"); if (m != "auto" && m != "prompt" && m != "approve") { std::cerr << "--approval must be auto, prompt, or approve\n"; return 1; } s["approval"] = m; }
            if (a->has("--disabled")) s["disabled"] = true;
            if (a->has("--ssl-verify")) s["sslVerify"] = a->get("--ssl-verify") != "false";
            if (a->has("--client-cert")) s["clientCert"] = a->get("--client-cert");
            if (a->has("--client-key")) s["clientKey"] = a->get("--client-key");
        }
        if (auto e = McpValidate(s); !e.empty()) { std::cerr << "Invalid MCP server \"" << name << "\": " << e << "\n"; return 1; }
        json pr = nullptr;
        if (sub == "add" && !a->has("--no-probe") && !s.value("disabled", false)) {
            pr = Probe(name, s);
            if (!pr.value("ok", false) && !pr.value("needsLogin", false)) { std::cerr << "MCP server \"" << name << "\" check failed: " << pr.value("error", std::string("unknown")) << "\nNot saved. Fix it or re-run with --no-probe.\n"; return 1; }
        }
        servers[name] = s; save(servers);
        Emit(js, {{"ok", true}, {"name", name}, {"server", s}, {"probe", pr}}, [&] {
            std::cout << "Saved MCP server \"" << name << "\" (" << McpTransport(s) << ").\n";
            if (pr.is_object() && pr.value("ok", false)) std::cout << "Connected: " << pr["tools"].size() << " tool(s).\n";
            if (pr.is_object() && pr.value("needsLogin", false)) std::cout << "Server needs authorization: openclaw-cpp mcp login " << name << "\n"; });
        return 0;
    }
    if (sub == "unset") {
        auto name = need("name"); if (name.empty()) return 1;
        if (!servers.contains(name)) { std::cerr << "No MCP server named \"" << name << "\".\n"; return 1; }
        servers.erase(name); save(servers); std::cout << "Removed MCP server \"" << name << "\".\n"; return 0;
    }
    if (sub == "tools") {
        auto name = need("name"); if (name.empty()) return 1;
        if (!servers.contains(name)) { std::cerr << "No MCP server named \"" << name << "\".\n"; return 1; }
        auto& s = servers[name];
        if (a->has("--clear")) { s.erase("include"); s.erase("exclude"); }
        if (a->has("--include")) s["include"] = Csv(a->get("--include"));
        if (a->has("--exclude")) s["exclude"] = Csv(a->get("--exclude"));
        save(servers);
        Emit(js, {{"name", name}, {"include", s.value("include", json::array())}, {"exclude", s.value("exclude", json::array())}}, [&] {
            std::cout << name << ": include=" << s.value("include", json::array()).dump() << " exclude=" << s.value("exclude", json::array()).dump() << "\n"; });
        return 0;
    }
    if (sub == "status" || sub == "probe" || sub == "doctor") {
        json out = json::array(); int bad = 0;
        const bool live = sub != "status" || a->has("-v") || a->has("--verbose");
        auto toks = ReadJ(McpTokPath());
        for (auto& [k, v] : servers.items()) {
            if (sub == "probe" && !a->pos.empty() && a->pos[0] != k) continue;
            json e{{"name", k}, {"transport", McpTransport(v)}, {"enabled", !v.value("disabled", false)}, {"oauth", v.contains("oauth")}, {"authorized", toks.contains(k)}};
            if (auto err = McpValidate(v); !err.empty()) { e["error"] = err; ++bad; }
            else if (live) { auto p = Probe(k, v); e["probe"] = p; if (!p.value("ok", false) && !p.contains("skipped")) ++bad; }
            out.push_back(e);
        }
        if (sub == "probe" && !a->pos.empty() && out.empty()) { std::cerr << "No MCP server named \"" << a->pos[0] << "\".\n"; return 1; }
        Emit(js, {{"servers", out}, {"problems", bad}}, [&] {
            if (out.empty()) std::cout << "No MCP servers configured.\n";
            for (auto& e : out) {
                std::cout << e["name"].get<std::string>() << "  " << e["transport"].get<std::string>() << (e["enabled"] ? "" : "  disabled") << (e["oauth"] ? (e["authorized"] ? "  oauth:authorized" : "  oauth:not-authorized") : "");
                if (e.contains("error")) std::cout << "  INVALID: " << e["error"].get<std::string>();
                else if (e.contains("probe")) { auto& p = e["probe"]; if (p.contains("skipped")) std::cout << "  skipped"; else if (p.value("ok", false)) std::cout << "  ok (" << p["tools"].size() << " tools)"; else std::cout << "  FAIL: " << p.value("error", std::string("")); }
                std::cout << "\n";
            } });
        return sub == "status" ? 0 : (bad ? 1 : 0);
    }
    if (sub == "logout") {
        auto name = need("name"); if (name.empty()) return 1;
        auto toks = ReadJ(McpTokPath()); const bool had = toks.contains(name); toks.erase(name); WriteJ(McpTokPath(), toks);
        std::filesystem::remove(StateRoot() / "mcp-login" / (name + ".json"));
        std::cout << (had ? "Cleared OAuth credentials for \"" : "No stored OAuth credentials for \"") << name << "\".\n"; return 0;
    }
    if (sub == "login") {
        auto name = need("name"); if (name.empty()) return 1;
        if (!servers.contains(name)) { std::cerr << "No MCP server named \"" << name << "\".\n"; return 1; }
        const auto pend = StateRoot() / "mcp-login" / (name + ".json");
        std::string err;
        if (a->has("--code")) {
            auto p = ReadJ(pend); if (!p.contains("sessionId")) { std::cerr << "No pending login for \"" << name << "\". Run `openclaw-cpp mcp login " << name << "` first.\n"; return 1; }
            auto w = GwRpc(raw, "wizard.next", {{"sessionId", p["sessionId"]}, {"answer", a->get("--code")}}, &err);
            if (w.is_null()) { std::cerr << "Login failed: " << err << "\n"; return 1; }
            if (w.value("status", "") != "done") { std::cerr << "Login failed: " << w.value("error", w.dump()) << "\n"; return 1; }
            std::filesystem::remove(pend);
            Emit(js, w, [&] { std::cout << "Authorized MCP server \"" << name << "\"" << (w["result"].value("verified", false) ? " (verified)" : "") << ".\n"; });
            return 0;
        }
        const std::string sid = "cli-mcp-" + name + "-" + RandHex(4);
        auto st = GwRpc(raw, "mcp.authLogin", {{"sessionId", sid}, {"serverName", name}}, &err);
        if (st.is_null()) { std::cerr << "Login could not start: " << err << "\n(The OAuth callback is served by the gateway; start it with `openclaw-cpp gateway`.)\n"; return 1; }
        WriteJ(pend, {{"sessionId", sid}, {"createdAt", IsoNow()}});
        Emit(js, {{"sessionId", sid}, {"url", st["step"]["url"]}}, [&] {
            std::cout << "Open this URL to authorize \"" << name << "\":\n  " << st["step"]["url"].get<std::string>() << "\n"
                      << "The gateway finishes automatically on redirect. Otherwise run: openclaw-cpp mcp login " << name << " --code <code-or-redirected-url>\n"; });
        return 0;
    }
    if (sub == "reload") {
        std::string err; auto r = GwRpc(raw, "config.get", json::object(), &err);
        std::cout << (r.is_null() ? "Gateway not running; MCP config will be read at next start.\n" : "The gateway reads mcp.servers from config on each turn; new config is used on the next turn.\n");
        return 0;
    }
    std::cerr << "openclaw-cpp mcp " << sub << ": not implemented in the C++ port (serve/configure are interactive or bridge modes; use `mcp add`/`mcp set`).\n";
    return 2;
}

int CmdReset(int argc, char** argv) {
    auto a = P3(argc, argv, 2, {"--scope=", "--yes", "--non-interactive", "--dry-run", "--json"}, "reset");
    if (!a) return 1;
    const auto scope = a->get("--scope", "config+creds+sessions");
    if (scope != "config" && scope != "config+creds+sessions" && scope != "full") { std::cerr << "Invalid --scope. Use config, config+creds+sessions, or full.\n"; return 1; }
    std::vector<fs::path> t{config::DefaultPath()};
    if (scope != "config") { t.push_back(StateRoot() / "credentials"); t.push_back(StateRoot() / "agents"); t.push_back(DataRoot() / "sessions"); if (const char* s = std::getenv("OPENCLAW_SESSION_DIR")) t.push_back(s); }
    if (scope == "full") { auto raw = LoadRaw(); if (raw.contains("agents") && raw["agents"].contains("defaults") && raw["agents"]["defaults"].value("workspace", "") != "") t.push_back(raw["agents"]["defaults"]["workspace"].get<std::string>()); t.push_back(DataRoot()); }
    std::error_code ec; json items = json::array(); for (auto& p : t) if (fs::exists(p, ec)) items.push_back(p.string());
    if (a->has("--dry-run")) { Emit(a->has("--json"), {{"scope", scope}, {"dryRun", true}, {"items", items}}, [&] { std::cout << "Would reset (" << scope << "):\n"; for (auto& i : items) std::cout << "  " << i.get<std::string>() << "\n"; }); return 0; }
    if (!a->has("--yes")) {
        if (a->has("--non-interactive") || !isatty(0)) { std::cerr << "Refusing to reset without --yes (non-interactive).\n"; return 1; }
        std::cout << "Reset " << scope << " (" << items.size() << " item(s)); moved to trash. Continue? [y/N] " << std::flush; std::string l; std::getline(std::cin, l); if (Lower(Trim(l)) != "y" && Lower(Trim(l)) != "yes") { std::cout << "Cancelled.\n"; return 1; }
    }
    const fs::path trash = HomeDir() / ".openclaw-trash" / ("reset-" + std::to_string(NowMs()));
    json moved = json::array();
    for (auto& i : items) { fs::path p = i.get<std::string>(); fs::create_directories(trash, ec); auto dst = trash / (std::to_string(moved.size()) + "-" + p.filename().string()); fs::rename(p, dst, ec); if (ec) { fs::copy(p, dst, fs::copy_options::recursive, ec); fs::remove_all(p, ec); } moved.push_back(p.string()); }
    Emit(a->has("--json"), {{"scope", scope}, {"moved", moved}, {"trash", trash.string()}}, [&] { std::cout << "Reset (" << scope << "): moved " << moved.size() << " item(s) to " << trash.string() << "\n"; });
    return 0;
}

int CmdUninstall(int argc, char** argv) {
    auto a = P3(argc, argv, 2, {"--service", "--state", "--workspace", "--app", "--all", "--yes", "--non-interactive", "--dry-run", "--json"}, "uninstall");
    if (!a) return 1;
    const bool all = a->has("--all");
    const bool svc = all || a->has("--service"), st = all || a->has("--state"), ws = all || a->has("--workspace"), app = all || a->has("--app");
    if (!svc && !st && !ws && !app) { std::cerr << "Choose what to remove: --service, --state, --workspace, --app, or --all.\n"; return 1; }
    json plan = json::array(); std::error_code ec;
    const fs::path unitdir = HomeDir() / ".config" / "systemd" / "user";
    std::vector<fs::path> units;
    if (svc && fs::is_directory(unitdir, ec)) for (auto& e : fs::directory_iterator(unitdir, ec)) { auto n = e.path().filename().string(); if (n.rfind("openclaw", 0) == 0 && n.size() > 8 && n.find(".service") != std::string::npos) { units.push_back(e.path()); plan.push_back({{"kind", "service"}, {"path", e.path().string()}}); } }
    auto raw = LoadRaw();
    if (ws && raw.contains("agents") && raw["agents"].contains("defaults") && raw["agents"]["defaults"].value("workspace", "") != "") { fs::path w = raw["agents"]["defaults"]["workspace"].get<std::string>(); if (fs::exists(w, ec)) plan.push_back({{"kind", "workspace"}, {"path", w.string()}}); }
    if (st) for (auto p : {StateRoot(), DataRoot(), config::DefaultPath()}) if (fs::exists(p, ec)) plan.push_back({{"kind", "state"}, {"path", p.string()}});
    json notes = json::array(); if (app) notes.push_back("--app: no desktop app bundle on this platform (macOS only); skipped");
    if (a->has("--dry-run")) { Emit(a->has("--json"), {{"dryRun", true}, {"plan", plan}, {"notes", notes}}, [&] { std::cout << "Would remove:\n"; for (auto& p : plan) std::cout << "  [" << p["kind"].get<std::string>() << "] " << p["path"].get<std::string>() << "\n"; for (auto& n : notes) std::cout << "  " << n.get<std::string>() << "\n"; }); return 0; }
    if (!a->has("--yes")) {
        if (a->has("--non-interactive") || !isatty(0)) { std::cerr << "Refusing to uninstall without --yes (non-interactive).\n"; return 1; }
        std::cout << "Permanently remove " << plan.size() << " item(s)? [y/N] " << std::flush; std::string l; std::getline(std::cin, l); if (Lower(Trim(l)) != "y" && Lower(Trim(l)) != "yes") { std::cout << "Cancelled.\n"; return 1; }
    }
    for (auto& u : units) { auto cmd = "systemctl --user disable --now '" + u.filename().string() + "' >/dev/null 2>&1"; if (std::system(cmd.c_str())) {} }
    json removed = json::array();
    for (auto& p : plan) { fs::remove_all(p["path"].get<std::string>(), ec); if (!ec) removed.push_back(p); }
    if (!units.empty()) if (std::system("systemctl --user daemon-reload >/dev/null 2>&1")) {}
    Emit(a->has("--json"), {{"removed", removed}, {"notes", notes}}, [&] { std::cout << "Removed " << removed.size() << " item(s).\n"; for (auto& n : notes) std::cout << n.get<std::string>() << "\n"; });
    return 0;
}

int CmdDocs(int argc, char** argv) {
    auto a = P3(argc, argv, 2, {"--json", "--limit="}, "docs");
    if (!a) return 1;
    const int limit = std::max(1, std::atoi(a->get("--limit", "8").c_str()));
    std::string base = "https://docs.openclaw.ai"; if (const char* e = std::getenv("OPENCLAW_DOCS_URL"); e && *e) base = NoSlash(e);
    std::string q; for (auto& p : a->pos) q += (q.empty() ? "" : " ") + p;
    if (q.empty()) { std::cout << "OpenClaw docs: " << base << "\nSearch: openclaw-cpp docs <query>\n"; return 0; }
    auto r = Http(base + "/llms.txt", {}, 15000);
    if (!r.error.empty() || r.status != 200) { std::cerr << "Docs search unavailable (" << (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error) << "). Browse " << base << "\n"; return 1; }
    std::vector<std::string> terms; { std::stringstream ss(Lower(q)); std::string t; while (ss >> t) terms.push_back(t); }
    static const std::regex link(R"(^\s*[-*]\s*\[([^\]]+)\]\(([^)]+)\)\s*:?\s*(.*)$)");
    std::vector<std::pair<int, json>> hits; std::istringstream is(r.body); std::string l;
    while (std::getline(is, l)) {
        std::smatch m; if (!std::regex_match(l, m, link)) continue;
        const std::string title = m[1], url = m[2], desc = m[3]; int sc = 0;
        for (auto& t : terms) { if (Lower(title).find(t) != std::string::npos) sc += 3; if (Lower(url).find(t) != std::string::npos) sc += 2; if (Lower(desc).find(t) != std::string::npos) sc += 1; }
        if (sc) hits.push_back({sc, {{"title", title}, {"url", url.rfind("http", 0) == 0 ? url : base + (url[0] == '/' ? "" : "/") + url}, {"snippet", desc}}});
    }
    std::stable_sort(hits.begin(), hits.end(), [](auto& x, auto& y) { return x.first > y.first; });
    json out = json::array(); for (size_t i = 0; i < hits.size() && static_cast<int>(i) < limit; ++i) out.push_back(hits[i].second);
    Emit(a->has("--json"), {{"query", q}, {"results", out}}, [&] {
        if (out.empty()) { std::cout << "No docs matched \"" << q << "\". Browse " << base << "\n"; return; }
        for (auto& h : out) std::cout << h["title"].get<std::string>() << "\n  " << h["url"].get<std::string>() << (h["snippet"].get<std::string>().empty() ? "" : "\n  " + h["snippet"].get<std::string>()) << "\n"; });
    return 0;
}
}  // namespace

int MaybeLocalCommand3(int argc, char** argv) {
    if (argc >= 2) { const std::string c1 = argv[1];
        if (c1 == "reset") return CmdReset(argc, argv);
        if (c1 == "uninstall") return CmdUninstall(argc, argv);
        if (c1 == "docs") return CmdDocs(argc, argv);
        if (c1 == "mcp" && (argc == 2 || std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
            std::cout << "Usage: openclaw-cpp mcp <command>\n\nManage OpenClaw-managed MCP servers (config: mcp.servers)\n\nCommands:\n"
                         "  list | show [name] | status [-v] | probe [name] | doctor\n  add <name> --command C [--arg A]... | --url U [--auth oauth]\n"
                         "  set <name> <json> | unset <name> | tools <name> --include/--exclude/--clear\n  login <name> [--code CODE] | logout <name> | reload\n";
            return 0;
        } }
    if (argc < 3) return -1;
    const std::string cmd = argv[1], sub = argv[2];
    if (cmd == "directory" && (sub == "self" || sub == "peers" || sub == "groups")) return CmdDirectory(argc, argv);
    if (cmd == "proxy" && (sub == "start" || sub == "run" || sub == "validate" || sub == "coverage" || sub == "sessions" || sub == "query" || sub == "blob" || sub == "purge")) return CmdProxy(argc, argv);
    if (cmd == "database" && (sub == "preflight" || sub == "preflight-agent" || sub == "ownership" || sub == "claim")) return CmdDatabase(argc, argv);
    if (cmd == "update" && (sub == "wizard" || sub == "cleanup" || sub == "recover" || sub == "migration-plan" || sub == "admit" || sub == "adopt-immutable")) return CmdUpdate(argc, argv);
    if (cmd == "mcp" && sub.rfind("-", 0) != 0) return CmdMcp(argc, argv);
    return -1;
}

}  // namespace openclaw::cli
