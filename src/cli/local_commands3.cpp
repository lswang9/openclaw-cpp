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
#include <ifaddrs.h>
#include <sqlite3.h>
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
// ============================================================ 1.124: promos / sandbox / dns / secrets / backup / gmail / node install / connect / hooks relay / mcp serve+configure / acp client
std::string ShQ(const std::string& s) { std::string o = "'"; for (char ch : s) { if (ch == '\'') o += "'\\''"; else o += ch; } return o + "'"; }
int ShRun(const std::string& cmd, std::string* out = nullptr) {
    FILE* p = popen((cmd + " 2>&1").c_str(), "r"); if (!p) return -1;
    std::string o; char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, p)) > 0) o.append(b, n);
    int rc = pclose(p); if (out) *out = o; return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}
bool InPath(const std::string& bin) { return ShRun("command -v " + ShQ(bin) + " >/dev/null") == 0; }
std::string Sanitize(const std::string& s) { std::string o; for (unsigned char ch : s) if (ch >= 0x20 && ch != 0x7f) o += static_cast<char>(ch); return o; }
std::string ClawHubBase() { for (auto k : {"CLAWHUB_REGISTRY", "CLAWDHUB_REGISTRY"}) if (const char* e = std::getenv(k); e && *e) return NoSlash(e); return "https://clawhub.ai"; }
bool Confirm(const std::string& q) { if (!isatty(0)) return false; std::cout << q << " [y/N] " << std::flush; std::string l; std::getline(std::cin, l); return Lower(Trim(l)) == "y" || Lower(Trim(l)) == "yes"; }
// ---------------------------------------------------------------- promos (ClawHub /api/v1/promotions)
int CmdPromos(int argc, char** argv, const std::string& sub) {
    auto a = P3(argc, argv, 3, {"--json", "--api-key=", "--set-default"}, "promos " + sub); if (!a) return 2;
    const bool js = a->has("--json");
    auto fetch = [&](const std::string& path, long* st) -> json {
        auto r = Http(ClawHubBase() + path, {"Accept: application/json"}); *st = r.status;
        if (!r.error.empty()) throw std::runtime_error("ClawHub unreachable: " + r.error);
        try { return json::parse(r.body); } catch (...) { return nullptr; }
    };
    auto days_left = [](const json& p) { long long d = std::max(0LL, (p.value("endsAt", 0LL) - NowMs()) / 86400000LL); return d == 0 ? std::string("ends today") : d == 1 ? std::string("1 day left") : std::to_string(d) + " days left"; };
    try {
        if (sub == "list") {
            long st = 0; auto j = fetch("/api/v1/promotions", &st);
            if (st == 404) { Emit(js, {{"promotions", json::array()}}, [] { std::cout << "Promotions are not available from ClawHub yet.\n"; }); return 0; }
            if (st != 200 || !j.is_object()) { std::cerr << "ClawHub promotions request failed (HTTP " << st << ")\n"; return 1; }
            auto ps = j.value("promotions", json::array());
            const fs::path notified = StateRoot() / "promotions-notified.json";
            auto seen = ReadJ(notified, json::array()); for (auto& p : ps) if (std::find(seen.begin(), seen.end(), p["slug"]) == seen.end()) seen.push_back(p["slug"]); WriteJ(notified, seen);
            Emit(js, {{"promotions", ps}}, [&] {
                if (ps.empty()) { std::cout << "No active promotions right now.\n"; return; }
                for (auto& p : ps) {
                    std::cout << Sanitize(p.value("title", "")) << (p.contains("sponsor") ? " — " + Sanitize(p.value("sponsor", "")) : "") << " (" << days_left(p) << ")\n  " << Sanitize(p.value("blurb", "")) << "\n";
                    for (auto& m : p.value("models", json::array())) std::cout << "  · " << Sanitize(m.value("modelRef", "")) << (m.contains("alias") ? " (" + Sanitize(m.value("alias", "")) + ")" : "") << (m.value("suggestedDefault", false) ? " — suggested default" : "") << "\n";
                    std::cout << "  Claim: openclaw-cpp promos claim " << Sanitize(p.value("slug", "")) << "\n";
                } });
            return 0;
        }
        if (a->pos.empty()) { std::cerr << "error: missing required argument 'slug'\n"; return 2; }
        const std::string slug = Lower(Trim(a->pos[0]));
        static const std::regex slug_re("^[a-z0-9][a-z0-9-]{0,63}$"), ref_re("^[a-z0-9][a-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._:/-]*$"), prov_re("^[a-z0-9][a-z0-9-]*$");
        if (!std::regex_match(slug, slug_re)) { std::cerr << "Invalid promotion slug.\n"; return 1; }
        auto load = [&]() {
            long st = 0; auto j = fetch("/api/v1/promotions/" + UrlEnc(slug), &st);
            if (st == 404) throw std::runtime_error("Promotion \"" + slug + "\" was not found or has ended.");
            if (st != 200 || !j.is_object()) throw std::runtime_error("ClawHub request failed (HTTP " + std::to_string(st) + ")");
            return j.contains("promotion") ? j["promotion"] : j;
        };
        auto live = [&](const json& x) { const auto now = NowMs(); if (x.value("startsAt", 0LL) > now || x.value("endsAt", 0LL) < now) throw std::runtime_error("Promotion \"" + slug + "\" is not active."); };
        auto p = load(); live(p);
        const std::string prov = Trim(p.value("provider", std::string()));
        if (prov.empty()) throw std::runtime_error("Promotion \"" + slug + "\" does not declare a provider; it cannot be claimed from the CLI.");
        if (!std::regex_match(prov, prov_re)) throw std::runtime_error("Promotion provider id is invalid.");
        for (auto& m : p.value("models", json::array())) {
            const auto ref = m.value("modelRef", std::string());
            if (!std::regex_match(ref, ref_re) || ref.rfind(prov + "/", 0) != 0) throw std::runtime_error("Promotion model ref \"" + Sanitize(ref) + "\" is invalid for provider " + prov + ".");
        }
        json raw = LoadRaw();
        const bool has_key = raw.contains("models") && raw["models"].contains("providers") && raw["models"]["providers"].contains(prov) && raw["models"]["providers"][prov].contains("apiKey");
        std::string key = a->get("--api-key");
        if (key.empty() && !has_key) {
            if (!isatty(0)) throw std::runtime_error("No credentials for provider " + prov + ". Re-run with --api-key <key>.");
            std::cout << prov << " API key: " << std::flush; std::getline(std::cin, key); key = Trim(key);
            if (key.empty()) throw std::runtime_error("API key required.");
        }
        auto p2 = load(); live(p2);
        if (p2.value("provider", "") != p.value("provider", "") || p2.value("models", json::array()) != p.value("models", json::array())) throw std::runtime_error("Promotion changed while claiming; re-run the claim.");
        if (!key.empty()) {
            raw["models"]["providers"][prov]["apiKey"] = key;
            if (p.contains("baseUrl") && !raw["models"]["providers"][prov].contains("baseUrl")) raw["models"]["providers"][prov]["baseUrl"] = p["baseUrl"];
        }
        json registered = json::array(), skipped = json::array();
        json& models = raw["agents"]["defaults"]["models"]; if (!models.is_object()) models = json::object();
        for (auto& m : p.value("models", json::array())) {
            const auto ref = m["modelRef"].get<std::string>(); if (!models.contains(ref)) models[ref] = json::object(); registered.push_back(ref);
            if (m.contains("alias")) {
                const auto al = Lower(Sanitize(m["alias"].get<std::string>())); bool taken = false;
                for (auto& [k, v] : models.items()) if (k != ref && v.value("alias", "") == al) taken = true;
                if (taken) skipped.push_back(al); else if (!models[ref].contains("alias")) models[ref]["alias"] = al;
            }
        }
        json suggested = nullptr; for (auto& m : p.value("models", json::array())) if (m.value("suggestedDefault", false)) suggested = m["modelRef"];
        if (suggested.is_null() && !p.value("models", json::array()).empty()) suggested = p["models"][0]["modelRef"];
        bool make_default = a->has("--set-default") && !suggested.is_null();
        if (!make_default && !suggested.is_null() && !a->has("--api-key")) make_default = Confirm("Set " + suggested.get<std::string>() + " as your default model?");
        if (make_default) raw["agents"]["defaults"]["model"]["primary"] = suggested;
        SaveRaw(raw);
        json claims = ReadJ(StateRoot() / "promotions-claimed.json"); claims[slug] = {{"claimedAt", IsoNow()}, {"provider", prov}, {"models", registered}}; WriteJ(StateRoot() / "promotions-claimed.json", claims);
        Emit(js, {{"slug", slug}, {"provider", prov}, {"registered", registered}, {"skippedAliases", skipped}, {"defaultModel", make_default ? suggested : json(nullptr)}}, [&] {
            std::cout << "Claimed \"" << Sanitize(p.value("title", slug)) << "\": " << registered.size() << " model(s) registered for " << prov << ".\n";
            if (make_default) std::cout << "Default model: " << suggested.get<std::string>() << "\n";
            for (auto& s : skipped) std::cout << "Alias \"" << s.get<std::string>() << "\" already in use; skipped.\n"; });
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
// ---------------------------------------------------------------- sandbox list / recreate (docker containers labelled by exec)
int CmdSandbox(int argc, char** argv, const std::string& sub) {
    auto a = P3(argc, argv, 3, {"--json", "--browser", "--all", "--session=", "--agent=", "--force"}, "sandbox " + sub); if (!a) return 2;
    const std::string label = a->has("--browser") ? "openclaw.browser=1" : "openclaw.sandbox=1";
    const bool docker = InPath("docker");
    json items = json::array();
    if (docker) {
        std::string out;
        if (ShRun("docker ps -a --filter " + ShQ("label=" + label) + " --format '{{json .}}'", &out) != 0) { std::cerr << "docker ps failed: " << Trim(out) << "\n"; return 1; }
        std::istringstream is(out); std::string l;
        while (std::getline(is, l)) {
            try {
                auto j = json::parse(l); json lab = json::object(); std::stringstream ls(j.value("Labels", "")); std::string kv;
                while (std::getline(ls, kv, ',')) { auto e = kv.find('='); if (e != std::string::npos) lab[kv.substr(0, e)] = kv.substr(e + 1); }
                items.push_back({{"id", j.value("ID", "")}, {"name", j.value("Names", "")}, {"image", j.value("Image", "")}, {"state", j.value("State", "")}, {"status", j.value("Status", "")},
                                 {"agent", lab.value("openclaw.agent", "")}, {"session", lab.value("openclaw.session", "")}});
            } catch (...) {}
        }
    }
    if (sub == "list") {
        Emit(a->has("--json"), {{"containers", items}, {"runtime", docker ? "docker" : (InPath("bwrap") ? "bwrap" : "none")}}, [&] {
            if (items.empty()) { std::cout << (docker ? "No sandbox containers.\n" : "No sandbox containers (docker not found; bwrap sandboxes are per-command and leave nothing running).\n"); return; }
            for (auto& c : items) std::cout << c["name"].get<std::string>() << "  " << c["state"].get<std::string>() << "  agent=" << c["agent"].get<std::string>() << "  session=" << c["session"].get<std::string>() << "  " << c["image"].get<std::string>() << "\n"; });
        return 0;
    }
    if (!a->has("--all") && !a->has("--session") && !a->has("--agent")) { std::cerr << "Choose --all, --session <key>, or --agent <id>.\n"; return 1; }
    json pick = json::array();
    for (auto& c : items) if (a->has("--all") || (a->has("--session") && c["session"] == a->get("--session")) || (a->has("--agent") && c["agent"] == a->get("--agent"))) pick.push_back(c);
    if (pick.empty()) { std::cout << "No matching sandbox containers.\n"; return 0; }
    if (!a->has("--force") && !Confirm("Remove " + std::to_string(pick.size()) + " container(s)?")) { std::cerr << "Refusing to remove " << pick.size() << " container(s) without --force.\n"; return 1; }
    int removed = 0; for (auto& c : pick) if (ShRun("docker rm -f " + ShQ(c["id"].get<std::string>()) + " >/dev/null") == 0) ++removed;
    std::cout << "Removed " << removed << " container(s); they are recreated with current config on next use.\n";
    return removed == static_cast<int>(pick.size()) ? 0 : 1;
}

// ---------------------------------------------------------------- dns setup
int CmdDnsSetup(int argc, char** argv) {
    auto a = P3(argc, argv, 3, {"--domain=", "--apply"}, "dns setup"); if (!a) return 2;
    json raw = LoadRaw();
    std::string dom = a->get("--domain");
    if (dom.empty() && raw.contains("discovery") && raw["discovery"].contains("wideArea")) dom = raw["discovery"]["wideArea"].value("domain", "");
    if (dom.empty()) if (const char* e = std::getenv("OPENCLAW_WIDE_AREA_DOMAIN")) dom = e;
    if (dom.empty()) { std::cerr << "No wide-area domain configured. Set discovery.wideArea.domain or pass --domain.\n"; return 1; }
    dom = Lower(dom); if (dom.back() == '.') dom.pop_back();
    static const std::regex dre("^([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\\.)+[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$");
    if (!std::regex_match(dom, dre) || dom == "local") { std::cerr << "Invalid wide-area domain \"" << dom << "\".\n"; return 1; }
    std::string v4, v6;
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (auto* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr) continue; char b[INET6_ADDRSTRLEN];
            if (i->ifa_addr->sa_family == AF_INET) {
                auto* s4 = reinterpret_cast<sockaddr_in*>(i->ifa_addr); auto ip = ntohl(s4->sin_addr.s_addr);
                if ((ip & 0xFFC00000u) == 0x64400000u && v4.empty()) { inet_ntop(AF_INET, &s4->sin_addr, b, sizeof b); v4 = b; }
            } else if (i->ifa_addr->sa_family == AF_INET6) {
                auto* s6 = reinterpret_cast<sockaddr_in6*>(i->ifa_addr); const unsigned char* x = s6->sin6_addr.s6_addr;
                if (x[0] == 0xfd && x[1] == 0x7a && x[2] == 0x11 && x[3] == 0x5c && x[4] == 0xa1 && x[5] == 0xe0 && v6.empty()) { inet_ntop(AF_INET6, &s6->sin6_addr, b, sizeof b); v6 = b; }
            }
        }
        freeifaddrs(ifs);
    }
    const fs::path zone = StateRoot() / "dns" / (dom + ".db");
    std::cout << "DNS setup\n  Domain      " << dom << "\n  Zone file   " << zone.string() << "\n  Tailnet IP  " << (v4.empty() ? "—" : v4) << (v6.empty() ? "" : " (v6 " + v6 + ")") << "\n\n"
              << "Recommended config ($OPENCLAW_CONFIG_PATH, default ~/.openclaw/openclaw.json):\n"
              << json{{"gateway", {{"bind", "auto"}}}, {"discovery", {{"wideArea", {{"domain", dom}}}}}}.dump(2) << "\n\n"
              << "Tailscale admin (DNS → Nameservers):\n- Add nameserver: " << (v4.empty() ? "<this machine's tailnet IPv4>" : v4) << "\n- Restrict to domain (Split DNS): " << dom << "\n";
    if (!a->has("--apply")) { std::cout << "\nRun with --apply to install CoreDNS and configure it.\n"; return 0; }
    std::cerr << "dns setup is currently supported on macOS only\n";  // upstream parity: --apply needs Homebrew CoreDNS
    return 1;
}
// ---------------------------------------------------------------- secrets apply / configure
json* PathRef(json& root, const std::string& dotted, bool create) {
    json* cur = &root; std::stringstream ss(dotted); std::string seg;
    while (std::getline(ss, seg, '.')) {
        if (seg.empty()) return nullptr;
        if (!cur->is_object()) { if (!create || !cur->is_null()) return nullptr; *cur = json::object(); }
        if (!cur->contains(seg)) { if (!create) return nullptr; (*cur)[seg] = nullptr; }
        cur = &(*cur)[seg];
    }
    return cur;
}
bool SecretKeyName(const std::string& k) {
    static const std::set<std::string> names{"apikey", "token", "bottoken", "apptoken", "password", "secret", "clientsecret", "signingsecret", "accesstoken", "refreshtoken", "webhooksecret", "privatekey"};
    return names.count(Lower(k)) > 0;
}
void FindPlain(const json& j, const std::string& path, std::vector<std::string>& out) {
    if (!j.is_object()) return;
    for (auto& [k, v] : j.items()) {
        const auto p = path.empty() ? k : path + "." + k;
        if (p == "secrets") continue;
        if (v.is_string() && SecretKeyName(k) && !v.get<std::string>().empty() && v.get<std::string>().rfind("${", 0) != 0) out.push_back(p);
        else if (v.is_object()) FindPlain(v, p, out);
    }
}
std::string JPtrEsc(const std::string& s) { std::string o; for (char c : s) { if (c == '~') o += "~0"; else if (c == '/') o += "~1"; else o += c; } return o; }
int CmdSecrets(int argc, char** argv, const std::string& sub) {
    auto a = P3(argc, argv, 3, {"--from=", "--dry-run", "--allow-exec", "--json", "--plan-out=", "--apply", "--yes", "--provider="}, "secrets " + sub); if (!a) return 2;
    json raw = LoadRaw(); const bool js = a->has("--json");
    if (sub == "apply") {
        if (!a->has("--from")) { std::cerr << "error: required option '--from <path>' not specified\n"; return 2; }
        json plan = ReadJ(a->get("--from"), nullptr);
        if (!plan.is_object()) { std::cerr << "Could not read plan " << a->get("--from") << "\n"; return 1; }
        if (plan.value("version", 0) != 1 || plan.value("protocolVersion", 0) != 1) { std::cerr << "Unsupported secrets plan (expected version 1, protocolVersion 1).\n"; return 1; }
        json changes = json::array();
        const json ups = plan.value("providerUpserts", json::object()), dels = plan.value("providerDeletes", json::array()), tgts = plan.value("targets", json::array());
        for (auto& [name, cfg] : ups.items()) {
            if (cfg.value("source", "") == "exec" && !a->has("--allow-exec")) { std::cerr << "Plan upserts exec provider \"" << name << "\"; re-run with --allow-exec to permit it.\n"; return 1; }
            raw["secrets"]["providers"][name] = cfg; changes.push_back({{"op", "providerUpsert"}, {"provider", name}});
        }
        for (auto& name : dels) {
            if (raw.contains("secrets") && raw["secrets"].contains("providers")) raw["secrets"]["providers"].erase(name.get<std::string>());
            changes.push_back({{"op", "providerDelete"}, {"provider", name}});
        }
        for (auto& t : tgts) {
            const auto path = t.value("path", std::string()); if (path.empty() || !t.contains("ref")) { std::cerr << "Invalid plan target: " << t.dump() << "\n"; return 1; }
            const auto& ref = t["ref"];
            if (!ref.is_object() || !ref.contains("source") || !ref.contains("provider") || !ref.contains("id")) { std::cerr << "Invalid SecretRef at " << path << "\n"; return 1; }
            const auto prov = ref["provider"].get<std::string>();
            const bool known = prov == "default" || (raw.contains("secrets") && raw["secrets"].contains("providers") && raw["secrets"]["providers"].contains(prov));
            if (!known) { std::cerr << "SecretRef at " << path << " uses unknown provider \"" << prov << "\".\n"; return 1; }
            if (ref["source"] == "exec" && !a->has("--allow-exec")) { std::cerr << "Target " << path << " uses an exec SecretRef; re-run with --allow-exec.\n"; return 1; }
            json* slot = PathRef(raw, path, true); if (!slot) { std::cerr << "Cannot write config path " << path << "\n"; return 1; }
            *slot = ref; changes.push_back({{"op", "target"}, {"path", path}});
        }
        if (!a->has("--dry-run")) SaveRaw(raw);
        Emit(js, {{"ok", true}, {"dryRun", a->has("--dry-run")}, {"changes", changes}}, [&] {
            std::cout << (a->has("--dry-run") ? "Dry run: " : "Applied ") << changes.size() << " change(s).\n";
            for (auto& c : changes) std::cout << "  " << c["op"].get<std::string>() << " " << (c.contains("path") ? c["path"].get<std::string>() : c["provider"].get<std::string>()) << "\n"; });
        return 0;
    }
    // configure: move plaintext secrets into a file provider.
    std::vector<std::string> found; FindPlain(raw, "", found);
    const std::string prov = a->get("--provider", "openclaw-file");
    const fs::path file = StateRoot() / "secrets" / "secrets.json";
    json plan = {{"version", 1}, {"protocolVersion", 1}, {"providerUpserts", {{prov, {{"source", "file"}, {"path", file.string()}, {"mode", "json"}}}}}, {"providerDeletes", json::array()}, {"targets", json::array()}};
    for (auto& p : found) { std::string ptr; std::stringstream ss(p); std::string seg; while (std::getline(ss, seg, '.')) ptr += "/" + JPtrEsc(seg); plan["targets"].push_back({{"path", p}, {"ref", {{"source", "file"}, {"provider", prov}, {"id", ptr}}}}); }
    if (a->has("--plan-out")) { std::error_code ec0; WriteJ(a->get("--plan-out"), plan); fs::permissions(a->get("--plan-out"), fs::perms::owner_read | fs::perms::owner_write, ec0); }
    if (!a->has("--apply")) {
        Emit(js, {{"plaintext", found}, {"plan", plan}}, [&] {
            if (found.empty()) { std::cout << "No plaintext secrets found in config.\n"; return; }
            std::cout << found.size() << " plaintext secret(s) found:\n"; for (auto& p : found) std::cout << "  " << p << "\n";
            std::cout << "Run `openclaw-cpp secrets configure --apply --yes` to move them into " << file.string() << ".\n"; });
        return 0;
    }
    if (!a->has("--yes") && !Confirm("Move " + std::to_string(found.size()) + " secret(s) into " + file.string() + "?")) { std::cerr << "Refusing to apply without --yes.\n"; return 1; }
    json store = ReadJ(file);
    for (auto& t : plan["targets"]) { json* v = PathRef(raw, t["path"], false); if (v) store[json::json_pointer(t["ref"]["id"].get<std::string>())] = *v; }
    std::error_code ec; fs::create_directories(file.parent_path(), ec); fs::permissions(file.parent_path(), fs::perms::owner_all, ec);
    { const auto tmp = file.string() + ".tmp"; int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600); if (fd < 0) { std::cerr << "Cannot write " << file.string() << "\n"; return 1; } auto s = store.dump(2); if (::write(fd, s.data(), s.size()) < 0) {} ::close(fd); fs::rename(tmp, file, ec); }
    raw["secrets"]["providers"][prov] = plan["providerUpserts"][prov];
    for (auto& t : plan["targets"]) if (json* v = PathRef(raw, t["path"], false)) *v = t["ref"];
    SaveRaw(raw);
    Emit(js, {{"ok", true}, {"moved", found}, {"file", file.string()}}, [&] { std::cout << "Moved " << found.size() << " secret(s) into " << file.string() << " (provider " << prov << ").\n"; });
    return 0;
}
// ---------------------------------------------------------------- backup sqlite / git
std::vector<fs::path> SqliteDbs() {
    std::vector<fs::path> v; std::error_code ec;
    for (const auto& root : {StateRoot(), DataRoot()}) {
        if (!fs::exists(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const auto p = it->path(); const auto s = p.string();
            if (s.find("/backups/") != std::string::npos || s.find("/backup-git/") != std::string::npos) continue;
            if (!it->is_regular_file(ec)) continue;
            std::ifstream f(p, std::ios::binary); char hdr[16] = {}; f.read(hdr, 16);
            if (f.gcount() == 16 && std::string(hdr, 15) == "SQLite format 3") v.push_back(p);
        }
    }
    std::sort(v.begin(), v.end()); return v;
}
std::string RelName(const fs::path& p) {
    for (const auto& [tag, root] : std::vector<std::pair<std::string, fs::path>>{{"state", StateRoot()}, {"data", DataRoot()}}) {
        auto r = fs::relative(p, root); if (!r.empty() && r.string().rfind("..", 0) != 0) return tag + "/" + r.string();
    }
    return p.filename().string();
}
bool SqlCopy(const fs::path& from, const fs::path& to, std::string* err) {
    sqlite3 *src = nullptr, *dst = nullptr;
    if (sqlite3_open_v2(from.c_str(), &src, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) { *err = sqlite3_errmsg(src); sqlite3_close(src); return false; }
    std::error_code ec; fs::create_directories(to.parent_path(), ec);
    if (sqlite3_open(to.c_str(), &dst) != SQLITE_OK) { *err = sqlite3_errmsg(dst); sqlite3_close(src); sqlite3_close(dst); return false; }
    auto* b = sqlite3_backup_init(dst, "main", src, "main"); bool ok = b && sqlite3_backup_step(b, -1) == SQLITE_DONE;
    if (b) sqlite3_backup_finish(b); if (!ok) *err = sqlite3_errmsg(dst);
    sqlite3_close(src); sqlite3_close(dst); return ok;
}
std::string SqlCheck(const fs::path& p) {
    sqlite3* db = nullptr; std::string res = "open failed";
    if (sqlite3_open_v2(p.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) res = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    sqlite3_close(db); return res;
}
std::string SqlDump(const fs::path& p) {
    sqlite3* db = nullptr; std::ostringstream o;
    if (sqlite3_open_v2(p.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) { sqlite3_close(db); return {}; }
    auto rows = [&](const std::string& sql, const std::function<void(sqlite3_stmt*)>& f) { sqlite3_stmt* st = nullptr; if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) while (sqlite3_step(st) == SQLITE_ROW) f(st); sqlite3_finalize(st); };
    std::vector<std::pair<std::string, std::string>> tables;
    rows("SELECT name, sql FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name", [&](sqlite3_stmt* st) { tables.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), sqlite3_column_text(st, 1) ? reinterpret_cast<const char*>(sqlite3_column_text(st, 1)) : ""); });
    for (auto& [name, sql] : tables) {
        o << sql << ";\n";
        std::string q = name; std::string esc; for (char c : q) { if (c == '"') esc += "\"\""; else esc += c; }
        rows("SELECT * FROM \"" + esc + "\" ORDER BY 1", [&](sqlite3_stmt* st) {
            o << "INSERT INTO \"" << esc << "\" VALUES(";
            for (int i = 0; i < sqlite3_column_count(st); ++i) {
                if (i) o << ",";
                switch (sqlite3_column_type(st, i)) {
                    case SQLITE_NULL: o << "NULL"; break;
                    case SQLITE_INTEGER: o << sqlite3_column_int64(st, i); break;
                    case SQLITE_FLOAT: { char b[64]; std::snprintf(b, sizeof b, "%.17g", sqlite3_column_double(st, i)); o << b; break; }
                    case SQLITE_BLOB: { auto* d = static_cast<const unsigned char*>(sqlite3_column_blob(st, i)); int n = sqlite3_column_bytes(st, i); o << "X'"; char b[3]; for (int k = 0; k < n; ++k) { std::snprintf(b, sizeof b, "%02x", d[k]); o << b; } o << "'"; break; }
                    default: { std::string s = reinterpret_cast<const char*>(sqlite3_column_text(st, i)); o << "'"; for (char c : s) { if (c == '\'') o << "''"; else o << c; } o << "'"; }
                }
            }
            o << ");\n"; });
    }
    rows("SELECT sql FROM sqlite_master WHERE type IN ('index','trigger','view') AND sql IS NOT NULL ORDER BY name", [&](sqlite3_stmt* st) { o << reinterpret_cast<const char*>(sqlite3_column_text(st, 0)) << ";\n"; });
    sqlite3_close(db); return o.str();
}
fs::path FromRel(const std::string& rel) { if (rel.rfind("state/", 0) == 0) return StateRoot() / rel.substr(6); if (rel.rfind("data/", 0) == 0) return DataRoot() / rel.substr(5); return {}; }
int CmdBackupSqlite(int argc, char** argv, const std::string& sub) {
    auto a = P3(argc, argv, 4, {"--json", "--out=", "--yes", "--dry-run"}, "backup sqlite " + sub); if (!a) return 2;
    const bool js = a->has("--json"); const fs::path dir = StateRoot() / "backups" / "sqlite";
    if (sub == "create") {
        const auto id = a->get("--out").empty() ? "sqlite-" + IsoNow() : a->get("--out"); const fs::path out = dir / id;
        json files = json::array(); std::string err;
        for (auto& db : SqliteDbs()) {
            const auto rel = RelName(db); const auto dst = out / "files" / rel;
            if (!SqlCopy(db, dst, &err)) { std::cerr << "backup of " << db.string() << " failed: " << err << "\n"; return 1; }
            std::ifstream f(dst, std::ios::binary); std::string body((std::istreambuf_iterator<char>(f)), {});
            files.push_back({{"path", rel}, {"bytes", body.size()}, {"sha256", Sha256(body)}});
        }
        json man = {{"version", 1}, {"kind", "sqlite"}, {"createdAt", IsoNow()}, {"files", files}}; WriteJ(out / "manifest.json", man);
        Emit(js, {{"ok", true}, {"path", out.string()}, {"manifest", man}}, [&] { std::cout << "Backed up " << files.size() << " SQLite database(s) to " << out.string() << "\n"; });
        return 0;
    }
    if (sub == "list") {
        json v = json::array(); std::error_code ec;
        if (fs::exists(dir, ec)) for (auto& e : fs::directory_iterator(dir, ec)) { auto m = ReadJ(e.path() / "manifest.json", nullptr); if (m.is_object()) v.push_back({{"id", e.path().filename().string()}, {"createdAt", m["createdAt"]}, {"files", m["files"].size()}}); }
        std::sort(v.begin(), v.end(), [](const json& x, const json& y) { return x["id"] < y["id"]; });
        Emit(js, {{"backups", v}}, [&] { if (v.empty()) std::cout << "No SQLite backups.\n"; for (auto& b : v) std::cout << b["id"].get<std::string>() << "  " << b["files"].get<size_t>() << " file(s)\n"; });
        return 0;
    }
    if (a->pos.empty()) { std::cerr << "error: missing required argument 'id'\n"; return 2; }
    const fs::path src = fs::path(a->pos[0]).is_absolute() ? fs::path(a->pos[0]) : dir / a->pos[0];
    auto man = ReadJ(src / "manifest.json", nullptr); if (!man.is_object()) { std::cerr << "No backup manifest at " << src.string() << "\n"; return 1; }
    json res = json::array(); bool ok = true;
    for (auto& f : man["files"]) {
        const auto p = src / "files" / f["path"].get<std::string>(); std::ifstream in(p, std::ios::binary); std::string body((std::istreambuf_iterator<char>(in)), {});
        const bool hash_ok = in.good() || !body.empty() ? Sha256(body) == f["sha256"].get<std::string>() : false; const auto integ = hash_ok ? SqlCheck(p) : std::string("missing or modified");
        res.push_back({{"path", f["path"]}, {"sha256Ok", hash_ok}, {"integrity", integ}}); ok = ok && hash_ok && integ == "ok";
    }
    if (sub == "verify") { Emit(js, {{"ok", ok}, {"files", res}}, [&] { for (auto& r : res) std::cout << r["path"].get<std::string>() << ": " << r["integrity"].get<std::string>() << "\n"; std::cout << (ok ? "Backup verified.\n" : "Backup verification FAILED.\n"); }); return ok ? 0 : 1; }
    if (!ok) { std::cerr << "Refusing to restore: backup failed verification.\n"; return 1; }
    if (a->has("--dry-run")) { for (auto& f : man["files"]) std::cout << "would restore " << FromRel(f["path"]).string() << "\n"; return 0; }
    if (!a->has("--yes") && !Confirm("Overwrite live databases from " + src.string() + "?")) { std::cerr << "Refusing to restore without --yes (stop the gateway first).\n"; return 1; }
    std::string err; int n = 0;
    for (auto& f : man["files"]) { const auto dst = FromRel(f["path"]); if (dst.empty()) continue; if (!SqlCopy(src / "files" / f["path"].get<std::string>(), dst, &err)) { std::cerr << "restore failed for " << dst.string() << ": " << err << "\n"; return 1; } ++n; }
    Emit(js, {{"ok", true}, {"restored", n}}, [&] { std::cout << "Restored " << n << " database(s).\n"; });
    return 0;
}
int CmdBackupGit(int argc, char** argv, const std::string& sub) {
    auto a = P3(argc, argv, 4, {"--json", "--message=", "--yes", "--limit=", "--ref="}, "backup git " + sub); if (!a) return 2;
    if (!InPath("git")) { std::cerr << "git is required for `backup git`.\n"; return 1; }
    const fs::path repo = StateRoot() / "backup-git"; const auto G = "git -C " + ShQ(repo.string()) + " -c user.name=openclaw -c user.email=openclaw@localhost ";
    std::string out;
    auto ensure = [&] { std::error_code ec; fs::create_directories(repo, ec); if (!fs::exists(repo / ".git")) ShRun(G + "init -q", &out); };
    if (sub == "init") { ensure(); std::cout << "Initialized backup repository at " << repo.string() << "\n"; return 0; }
    if (!fs::exists(repo / ".git")) { if (sub != "create") { std::cerr << "No backup repository yet; run `openclaw-cpp backup git init`.\n"; return 1; } ensure(); }
    if (sub == "create") {
        std::error_code ec; fs::remove_all(repo / "dumps", ec); json man = {{"version", 1}, {"files", json::array()}};
        for (auto& db : SqliteDbs()) { const auto rel = RelName(db) + ".sql"; const auto d = SqlDump(db); fs::create_directories((repo / "dumps" / rel).parent_path(), ec); std::ofstream(repo / "dumps" / rel) << d; man["files"].push_back({{"path", RelName(db)}, {"dump", "dumps/" + rel}, {"sha256", Sha256(d)}}); }
        WriteJ(repo / "manifest.json", man);
        ShRun(G + "add -A", &out);
        if (ShRun(G + "diff --cached --quiet") == 0) { std::cout << "No changes since the last backup.\n"; return 0; }
        if (ShRun(G + "commit -q -m " + ShQ(a->get("--message", "openclaw backup " + IsoNow())), &out) != 0) { std::cerr << out; return 1; }
        ShRun(G + "rev-parse --short HEAD", &out); std::cout << "Committed backup " << Trim(out) << " (" << man["files"].size() << " database dump(s)).\n"; return 0;
    }
    if (sub == "log") { ShRun(G + "log --format='%h %cI %s' -n " + std::to_string(std::max(1, std::atoi(a->get("--limit", "20").c_str()))), &out); std::cout << out; return 0; }
    const std::string ref = a->pos.empty() ? a->get("--ref", "HEAD") : a->pos[0];
    if (ShRun(G + "rev-parse --verify -q " + ShQ(ref + "^{commit}"), &out) != 0) { std::cerr << "Unknown backup ref " << ref << "\n"; return 1; }
    ShRun(G + "show " + ShQ(ref + ":manifest.json"), &out); json man; try { man = json::parse(out); } catch (...) { std::cerr << "Backup " << ref << " has no manifest.\n"; return 1; }
    bool ok = true;
    for (auto& f : man["files"]) {
        std::string d; ShRun(G + "show " + ShQ(ref + ":" + f["dump"].get<std::string>()), &d);
        sqlite3* db = nullptr; sqlite3_open(":memory:", &db); const bool loads = sqlite3_exec(db, d.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK; sqlite3_close(db);
        const bool h = Sha256(d) == f["sha256"].get<std::string>(); ok = ok && h && loads;
        if (sub == "verify") std::cout << f["path"].get<std::string>() << ": " << (h && loads ? "ok" : "FAILED") << "\n";
    }
    if (sub == "verify") { std::cout << (ok ? "Backup verified.\n" : "Backup verification FAILED.\n"); return ok ? 0 : 1; }
    if (!ok) { std::cerr << "Refusing to restore: backup failed verification.\n"; return 1; }
    if (!a->has("--yes") && !Confirm("Overwrite live databases from backup " + ref + "?")) { std::cerr << "Refusing to restore without --yes (stop the gateway first).\n"; return 1; }
    int n = 0;
    for (auto& f : man["files"]) {
        const auto dst = FromRel(f["path"]); if (dst.empty()) continue; std::string d; ShRun(G + "show " + ShQ(ref + ":" + f["dump"].get<std::string>()), &d);
        const auto tmp = dst.string() + ".restore"; std::error_code ec; fs::remove(tmp, ec); fs::create_directories(dst.parent_path(), ec);
        sqlite3* db = nullptr; sqlite3_open(tmp.c_str(), &db); const bool okx = sqlite3_exec(db, ("BEGIN;" + d + "COMMIT;").c_str(), nullptr, nullptr, nullptr) == SQLITE_OK; sqlite3_close(db);
        if (!okx) { std::cerr << "restore failed for " << dst.string() << "\n"; fs::remove(tmp, ec); return 1; }
        fs::rename(tmp, dst, ec); fs::remove(dst.string() + "-wal", ec); fs::remove(dst.string() + "-shm", ec); ++n;
    }
    std::cout << "Restored " << n << " database(s) from " << ref << ".\n"; return 0;
}
// ---------------------------------------------------------------- webhooks gmail setup / run
int CmdGmail(int argc, char** argv, const std::string& sub) {
    auto a = P3(argc, argv, 4, {"--account=", "--project=", "--topic=", "--subscription=", "--label=", "--hook-url=", "--hook-token=", "--push-token=", "--bind=", "--port=", "--path=", "--include-body", "--max-bytes=", "--renew-minutes=", "--tailscale=", "--tailscale-path=", "--push-endpoint=", "--json", "--dry-run"}, "webhooks gmail " + sub); if (!a) return 2;
    json raw = LoadRaw(); json g = raw.contains("hooks") && raw["hooks"].contains("gmail") ? raw["hooks"]["gmail"] : json::object();
    auto pick = [&](const std::string& opt, const std::string& key, const std::string& d) { return a->has(opt) ? a->get(opt) : g.value(key, d); };
    const std::string account = pick("--account", "account", "");
    if (account.empty()) { std::cerr << "error: required option '--account <email>' not specified\n"; return 2; }
    const std::string topic = pick("--topic", "topic", "gog-gmail-watch"), subn = pick("--subscription", "subscription", "gog-gmail-watch-push"), label = pick("--label", "label", "INBOX");
    const std::string bind = pick("--bind", "bind", "127.0.0.1"), port = a->has("--port") ? a->get("--port") : std::to_string(g.value("port", 8788)), path = pick("--path", "path", "/gmail-pubsub");
    std::string hook_token = pick("--hook-token", "hookToken", ""); if (hook_token.empty() && raw.contains("hooks")) hook_token = raw["hooks"].value("token", "");
    if (hook_token.empty()) hook_token = RandHex(24);
    std::string push_token = pick("--push-token", "pushToken", ""); if (push_token.empty()) push_token = RandHex(24);
    const std::string hook_url = pick("--hook-url", "hookUrl", GwBase(raw) + "/hooks/gmail");
    const bool body = a->has("--include-body") || g.value("includeBody", true);
    const std::string max_bytes = a->has("--max-bytes") ? a->get("--max-bytes") : std::to_string(g.value("maxBytes", 20000));
    const std::string renew = a->has("--renew-minutes") ? a->get("--renew-minutes") : std::to_string(g.value("renewEveryMinutes", 720));
    std::vector<std::string> serve = {"gog", "gmail", "watch", "serve", "--account", account, "--bind", bind, "--port", port, "--path", path, "--token", push_token, "--hook-url", hook_url, "--hook-token", hook_token};
    if (body) serve.push_back("--include-body"); serve.push_back("--max-bytes"); serve.push_back(max_bytes);
    auto joined = [](const std::vector<std::string>& v) { std::string s; for (auto& x : v) s += (s.empty() ? "" : " ") + ShQ(x); return s; };
    if (sub == "run") {
        if (!InPath("gog")) { std::cerr << "gog not found on PATH (install gogcli: https://gogcli.sh).\n"; return 1; }
        std::vector<std::string> start = {"gog", "gmail", "watch", "start", "--account", account, "--label", label, "--topic", "projects/" + pick("--project", "project", "") + "/topics/" + topic};
        std::string out; if (ShRun(joined(start), &out) != 0) std::cerr << "gmail watch start failed: " << Trim(out) << "\n";
        std::cout << "Serving Gmail push on " << bind << ":" << port << path << " (renew every " << renew << "m)\n" << std::flush;
        std::vector<char*> av; for (auto& s : serve) av.push_back(const_cast<char*>(s.c_str())); av.push_back(nullptr);
        execvp("gog", av.data()); std::perror("exec gog"); return 1;
    }
    std::string project = pick("--project", "project", "");
    std::vector<std::string> steps;
    if (project.empty() && InPath("gcloud")) { std::string out; ShRun("gcloud config get-value project", &out); project = Trim(out); }
    if (project.empty()) { std::cerr << "GCP project required (--project or `gcloud config set project`).\n"; return 1; }
    const std::string full_topic = "projects/" + project + "/topics/" + topic;
    std::string endpoint = a->get("--push-endpoint");
    if (endpoint.empty() && pick("--tailscale", "tailscaleMode", "funnel") != "off" && InPath("tailscale")) {
        std::string out; ShRun("tailscale status --json", &out); try { auto s = json::parse(out); auto dns = s["Self"].value("DNSName", ""); if (!dns.empty() && dns.back() == '.') dns.pop_back(); if (!dns.empty()) endpoint = "https://" + dns + pick("--tailscale-path", "tailscalePath", path); } catch (...) {}
    }
    if (endpoint.empty()) { std::cerr << "Push endpoint required (--push-endpoint URL, or tailscale funnel).\n"; return 1; }
    endpoint += (endpoint.find('?') == std::string::npos ? "?token=" : "&token=") + push_token;
    steps.push_back(joined({"gcloud", "services", "enable", "gmail.googleapis.com", "pubsub.googleapis.com", "--project", project}));
    steps.push_back(joined({"gcloud", "pubsub", "topics", "create", topic, "--project", project}));
    steps.push_back(joined({"gcloud", "pubsub", "topics", "add-iam-policy-binding", topic, "--project", project, "--member", "serviceAccount:gmail-api-push@system.gserviceaccount.com", "--role", "roles/pubsub.publisher"}));
    steps.push_back(joined({"gcloud", "pubsub", "subscriptions", "create", subn, "--project", project, "--topic", topic, "--push-endpoint", endpoint}));
    steps.push_back(joined({"gog", "gmail", "watch", "start", "--account", account, "--label", label, "--topic", full_topic}));
    json res = json::array();
    for (auto& s : steps) {
        if (a->has("--dry-run")) { res.push_back({{"cmd", s}, {"dryRun", true}}); continue; }
        std::string out; int rc = ShRun(s, &out); const bool exists = out.find("ALREADY_EXISTS") != std::string::npos || out.find("already exists") != std::string::npos;
        res.push_back({{"cmd", s}, {"exit", rc}}); if (rc != 0 && !exists) { std::cerr << "step failed: " << s << "\n" << out; return 1; }
    }
    raw["hooks"]["enabled"] = true; if (raw["hooks"].value("token", "").empty()) raw["hooks"]["token"] = hook_token;
    raw["hooks"]["gmail"] = {{"account", account}, {"project", project}, {"topic", full_topic}, {"subscription", subn}, {"label", label}, {"pushToken", push_token}, {"hookUrl", hook_url}, {"hookToken", hook_token}, {"includeBody", body}, {"maxBytes", std::stoi(max_bytes)}, {"renewEveryMinutes", std::stoi(renew)}, {"serve", {{"bind", bind}, {"port", std::stoi(port)}, {"path", path}}}};
    json presets = raw["hooks"].value("presets", json::array()); if (std::find(presets.begin(), presets.end(), "gmail") == presets.end()) presets.push_back("gmail"); raw["hooks"]["presets"] = presets;
    if (!a->has("--dry-run")) SaveRaw(raw);
    Emit(a->has("--json"), {{"ok", true}, {"steps", res}, {"config", raw["hooks"]["gmail"]}}, [&] {
        for (auto& r : res) std::cout << (a->has("--dry-run") ? "would run: " : "ran: ") << r["cmd"].get<std::string>() << "\n";
        std::cout << "Gmail hooks configured for " << account << ". Start the watcher with `openclaw-cpp webhooks gmail run`.\n"; });
    return 0;
}

// ---------------------------------------------------------------- node install (systemd user unit)
int InstallNodeUnit(const std::vector<std::string>& run_args, bool force, bool js) {
    const fs::path unit = HomeDir() / ".config" / "systemd" / "user" / "openclaw-node.service";
    std::error_code ec;
    if (fs::exists(unit, ec) && !force) { std::cerr << "Node service already installed at " << unit.string() << " (use --force to reinstall).\n"; return 1; }
    std::string exec = ShQ(SelfExe().string()) + " node run"; for (auto& x : run_args) exec += " " + ShQ(x);
    fs::create_directories(unit.parent_path(), ec);
    std::ofstream(unit) << "[Unit]\nDescription=OpenClaw node host\nAfter=network-online.target\n\n[Service]\nExecStart=" << exec << "\nRestart=always\nRestartSec=5\n"
                        << (std::getenv("OPENCLAW_STATE_DIR") ? std::string("Environment=OPENCLAW_STATE_DIR=") + std::getenv("OPENCLAW_STATE_DIR") + "\n" : "") << "\n[Install]\nWantedBy=default.target\n";
    std::string note = "installed";
    if (!std::getenv("OPENCLAW_SKIP_SYSTEMCTL") && InPath("systemctl")) {
        std::string out;
        if (ShRun("systemctl --user daemon-reload && systemctl --user enable --now openclaw-node.service", &out) != 0) { std::cerr << "systemctl failed: " << Trim(out) << "\nUnit written to " << unit.string() << "\n"; return 1; }
        note = "installed and started";
    } else note = "installed (systemctl skipped)";
    Emit(js, {{"ok", true}, {"unit", unit.string()}, {"exec", exec}, {"status", note}}, [&] { std::cout << "Node service " << note << ": " << unit.string() << "\n"; });
    return 0;
}
int CmdNodeInstall(int argc, char** argv) {
    auto a = P3(argc, argv, 3, {"--url=", "--node-id=", "--display-name=", "--token=", "--force", "--json", "--runtime="}, "node install"); if (!a) return 2;
    std::vector<std::string> ra; for (auto k : {"--url", "--node-id", "--display-name", "--token"}) if (a->has(k)) { ra.push_back(k); ra.push_back(a->get(k)); }
    return InstallNodeUnit(ra, a->has("--force"), a->has("--json"));
}

// ---------------------------------------------------------------- connect <oc-pair URL | setup code | join URL>
std::string B64UrlDecode(std::string s) {
    for (auto& c : s) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    while (s.size() % 4) s += '=';
    static const std::string T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; int v = 0, bits = -8;
    for (unsigned char c : s) { if (c == '=') break; auto p = T.find(static_cast<char>(c)); if (p == std::string::npos) return {}; v = (v << 6) + static_cast<int>(p); bits += 6; if (bits >= 0) { o += static_cast<char>((v >> bits) & 0xFF); bits -= 8; } }
    return o;
}
int CmdConnect(int argc, char** argv) {
    auto a = P3(argc, argv, 2, {"--service", "--ephemeral", "--target-file=", "--display-name=", "--node-id=", "--timeout=", "--json", "--force"}, "connect"); if (!a) return 2;
    std::string target = a->pos.empty() ? "" : a->pos[0];
    if (a->has("--target-file")) { std::ifstream f(a->get("--target-file")); std::getline(f, target); target = Trim(target); std::error_code ec; fs::remove(a->get("--target-file"), ec); }
    if (target.empty()) { std::cerr << "Missing connect target. Mint a join URL on the Gateway host with `openclaw-cpp devices join-code`, then run: openclaw-cpp connect <join-url>" << (a->has("--service") ? " --service" : "") << "\n"; return 2; }
    json payload;
    std::string code = target;
    if (code.rfind("oc-pair://", 0) == 0) { code = code.substr(10); auto q = code.find("code="); if (q != std::string::npos) code = code.substr(q + 5); else { auto s = code.find('/'); if (s != std::string::npos) code = code.substr(s + 1); } auto amp = code.find('&'); if (amp != std::string::npos) code = code.substr(0, amp); }
    if (code.rfind("http://", 0) == 0 || code.rfind("https://", 0) == 0) {
        auto j = code.find("/j/"); if (j == std::string::npos) { std::cerr << "Join URL must look like https://<gateway>/j/<code>.\n"; return 1; }
        const std::string base = code.substr(0, j), jc = code.substr(j + 3);
        auto r = Http(base + "/j/" + jc, {"Accept: application/json"}, 15000);
        try { payload = json::parse(r.body); } catch (...) {}
        if (!payload.is_object() || !payload.contains("bootstrapToken")) { std::cerr << "Join URL did not return a pairing payload (HTTP " << r.status << ").\n"; return 1; }
        if (!payload.contains("url")) payload["url"] = base;
    } else {
        try { payload = json::parse(B64UrlDecode(code)); } catch (...) {}
        if (!payload.is_object() || !payload.contains("url") || !payload.contains("bootstrapToken")) { std::cerr << "Invalid setup code: expected base64url JSON with url and bootstrapToken.\n"; return 1; }
    }
    const std::string url = NoSlash(payload["url"].get<std::string>()), boot = payload["bootstrapToken"].get<std::string>();
    std::string token;
    if (auto dot = boot.find('.'); boot.rfind("link-", 0) == 0 && dot != std::string::npos) {
        const std::string id = boot.substr(0, dot), secret = boot.substr(dot + 1);
        json raw = json::object(); raw["gateway"]["port"] = 0; setenv("OPENCLAW_GATEWAY_URL", url.c_str(), 1);
        const int timeout = std::max(1, std::atoi(a->get("--timeout", "300").c_str()));
        std::cerr << "Waiting for the operator to approve this node (link " << id << ")…\n";
        for (int t = 0; t < timeout * 2; ++t) {
            std::string err; auto r = GwRpc(raw, "pairing.link.claim", {{"id", id}, {"poll_secret", secret}}, &err);
            if (!err.empty()) { std::cerr << "claim failed: " << err << "\n"; return 1; }
            if (r.value("ok", false) && r.contains("token")) { token = r["token"]; break; }
            if (!r.value("pending", false)) { std::cerr << "claim rejected: " << r.value("error", r.dump()) << "\n"; return 1; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (token.empty()) { std::cerr << "Timed out waiting for approval.\n"; return 1; }
    } else token = boot;  // a pre-minted device token
    std::vector<std::string> ra{"--url", url, "--token", token};
    if (a->has("--display-name")) { ra.push_back("--display-name"); ra.push_back(a->get("--display-name")); }
    if (a->has("--node-id")) { ra.push_back("--node-id"); ra.push_back(a->get("--node-id")); }
    json st = ReadJ(StateRoot() / "node-connect.json"); st["url"] = url; st["connectedAt"] = IsoNow(); WriteJ(StateRoot() / "node-connect.json", st);
    if (a->has("--service")) return InstallNodeUnit(ra, true, a->has("--json"));
    std::cerr << "Paired with " << url << "; starting node host.\n";
    std::vector<std::string> full{SelfExe().string(), "node", "run"}; full.insert(full.end(), ra.begin(), ra.end());
    std::vector<char*> av; for (auto& s : full) av.push_back(const_cast<char*>(s.c_str())); av.push_back(nullptr);
    execv(full[0].c_str(), av.data()); std::perror("exec node run"); return 1;
}
// ---------------------------------------------------------------- hooks relay (native agent hook bridge)
int CmdHooksRelay(int argc, char** argv) {
    auto a = P3(argc, argv, 3, {"--event=", "--agent=", "--session=", "--pre-tool-use-unavailable=", "--timeout=", "--json"}, "hooks relay"); if (!a) return 2;
    std::string in((std::istreambuf_iterator<char>(std::cin)), {});
    json ev; try { ev = in.empty() ? json::object() : json::parse(in); } catch (...) { std::cerr << "hooks relay: stdin is not JSON\n"; return 1; }
    std::string event = a->get("--event", ev.value("hook_event_name", ev.value("hookEventName", std::string())));
    if (event.empty()) { std::cerr << "hooks relay: missing --event (or hook_event_name in stdin)\n"; return 2; }
    json raw = LoadRaw(); std::string err;
    json params = {{"event", event}, {"payload", ev}};
    if (a->has("--agent")) params["agentId"] = a->get("--agent"); if (a->has("--session")) params["sessionKey"] = a->get("--session");
    auto r = GwRpc(raw, "nativeHook.invoke", params, &err);
    const bool pre = Lower(event) == "pretooluse" || event == "pre_tool_use" || event == "beforeToolCall";
    if (!err.empty()) {
        if (pre && a->get("--pre-tool-use-unavailable", "allow") == "deny") { std::cerr << "OpenClaw gateway unavailable; blocking tool call (" << err << ")\n"; return 2; }
        return 0;  // fail open: never wedge the host agent
    }
    const json& d = r.contains("result") ? r["result"] : r;
    const std::string decision = Lower(d.value("decision", d.value("permissionDecision", std::string())));
    if (d.contains("output")) std::cout << (d["output"].is_string() ? d["output"].get<std::string>() : d["output"].dump()) << "\n";
    else if (!d.empty() && a->has("--json")) std::cout << d.dump() << "\n";
    if (decision == "deny" || decision == "block") { std::cerr << d.value("reason", std::string("blocked by OpenClaw hook")) << "\n"; return 2; }
    return 0;
}

// ---------------------------------------------------------------- mcp serve (stdio MCP bridge to the gateway)
int CmdMcpServe(int argc, char** argv) {
    auto a = P3(argc, argv, 3, {"--url=", "--token=", "--session=", "--claude-channel-mode=", "--verbose"}, "mcp serve"); if (!a) return 2;
    if (a->has("--url")) setenv("OPENCLAW_GATEWAY_URL", a->get("--url").c_str(), 1);
    if (a->has("--token")) setenv("OPENCLAW_GATEWAY_TOKEN", a->get("--token").c_str(), 1);
    const json raw = LoadRaw(); const bool verbose = a->has("--verbose");
    auto obj = [](json props, json req = json::array()) { return json{{"type", "object"}, {"properties", props}, {"required", req}}; };
    const json S = {{"type", "string"}}, N = {{"type", "number"}};
    const json tools = json::array({
        {{"name", "conversations_list"}, {"description", "List OpenClaw conversations (sessions)."}, {"inputSchema", obj({{"limit", N}, {"search", S}})}},
        {{"name", "conversation_get"}, {"description", "Get one conversation by session key."}, {"inputSchema", obj({{"session_key", S}}, {"session_key"})}},
        {{"name", "messages_read"}, {"description", "Read recent messages of a conversation."}, {"inputSchema", obj({{"session_key", S}, {"limit", N}}, {"session_key"})}},
        {{"name", "attachments_fetch"}, {"description", "Fetch attachments of one message."}, {"inputSchema", obj({{"session_key", S}, {"message_id", S}}, {"session_key", "message_id"})}},
        {{"name", "events_poll"}, {"description", "Return new messages since a cursor."}, {"inputSchema", obj({{"session_key", S}, {"after", N}})}},
        {{"name", "events_wait"}, {"description", "Wait up to timeout_ms for new messages."}, {"inputSchema", obj({{"session_key", S}, {"after", N}, {"timeout_ms", N}})}},
        {{"name", "messages_send"}, {"description", "Send a message into a conversation."}, {"inputSchema", obj({{"session_key", S}, {"text", S}}, {"session_key", "text"})}},
        {{"name", "permissions_list_open"}, {"description", "List open approval requests."}, {"inputSchema", obj(json::object())}},
        {{"name", "permissions_respond"}, {"description", "Resolve an approval request."}, {"inputSchema", obj({{"id", S}, {"decision", {{"type", "string"}, {"enum", {"allow-once", "allow-always", "deny"}}}}}, {"id", "decision"})}}});
    auto rpc = [&](const std::string& m, const json& p) { std::string err; auto r = GwRpc(raw, m, p, &err); if (!err.empty()) throw std::runtime_error(err); return r; };
    auto msgs = [&](const std::string& key, int limit) { auto h = rpc("chat.history", {{"sessionKey", key}, {"limit", limit}}); return h.contains("messages") ? h["messages"] : h; };
    auto since = [&](const std::string& key, long long after) {
        json all = msgs(key.empty() ? std::string("main") : key, 200), out = json::array(); long long idx = 0;
        for (auto& m : all) { ++idx; if (idx > after) out.push_back({{"seq", idx}, {"message", m}}); }
        return json{{"events", out}, {"cursor", idx}};
    };
    auto call = [&](const std::string& name, const json& x) -> json {
        const std::string key = x.value("session_key", a->get("--session"));
        if (name == "conversations_list") return rpc("sessions.list", {{"limit", x.value("limit", 50)}, {"search", x.value("search", "")}});
        if (name == "conversation_get") return rpc("sessions.get", {{"key", key}});
        if (name == "messages_read") return msgs(key, x.value("limit", 50));
        if (name == "attachments_fetch") { auto m = rpc("chat.message.get", {{"sessionKey", key}, {"messageId", x.value("message_id", "")}}); return m.value("attachments", m.contains("message") ? m["message"].value("attachments", json::array()) : json::array()); }
        if (name == "events_poll") return since(key, x.value("after", 0LL));
        if (name == "events_wait") {
            const auto end = NowMs() + std::min(300000LL, x.value("timeout_ms", 30000LL));
            for (;;) { auto r = since(key, x.value("after", 0LL)); if (!r["events"].empty() || NowMs() >= end) return r; std::this_thread::sleep_for(std::chrono::milliseconds(750)); }
        }
        if (name == "messages_send") return rpc("chat.send", {{"sessionKey", key.empty() ? std::string("main") : key}, {"message", x.value("text", "")}, {"idempotencyKey", "mcp-" + RandHex(8)}});
        if (name == "permissions_list_open") { json o = json::object(); o["exec"] = rpc("exec.approval.list", json::object()); try { o["plugin"] = rpc("plugin.approval.list", json::object()); } catch (...) {} return o; }
        if (name == "permissions_respond") {
            const auto id = x.value("id", ""), dec = x.value("decision", "");
            try { return rpc("exec.approval.resolve", {{"id", id}, {"decision", dec}}); } catch (...) { return rpc("plugin.approval.resolve", {{"id", id}, {"decision", dec}}); }
        }
        throw std::runtime_error("unknown tool " + name);
    };
    std::string line;
    while (std::getline(std::cin, line)) {
        if (Trim(line).empty()) continue;
        json req; try { req = json::parse(line); } catch (...) { std::cout << json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}}.dump() << "\n" << std::flush; continue; }
        const std::string m = req.value("method", ""); if (!req.contains("id")) continue;  // notifications
        json res, error;
        if (m == "initialize") res = {{"protocolVersion", req["params"].value("protocolVersion", "2025-06-18")}, {"capabilities", {{"tools", json::object()}}}, {"serverInfo", {{"name", "openclaw"}, {"version", "cpp"}}}};
        else if (m == "ping") res = json::object();
        else if (m == "tools/list") res = {{"tools", tools}};
        else if (m == "tools/call") {
            const auto name = req["params"].value("name", ""); const auto args = req["params"].value("arguments", json::object());
            try { auto out = call(name, args); res = {{"content", {{{"type", "text"}, {"text", out.dump(2)}}}}, {"structuredContent", out.is_object() ? out : json{{"result", out}}}}; }
            catch (const std::exception& e) { if (verbose) std::cerr << "mcp serve: " << name << ": " << e.what() << "\n"; res = {{"content", {{{"type", "text"}, {"text", e.what()}}}}, {"isError", true}}; }
        } else error = {{"code", -32601}, {"message", "method not found: " + m}};
        json out = {{"jsonrpc", "2.0"}, {"id", req["id"]}}; if (error.is_null()) out["result"] = res; else out["error"] = error;
        std::cout << out.dump() << "\n" << std::flush;
    }
    return 0;
}

// ---------------------------------------------------------------- mcp configure (interactive wrapper over `mcp add`)
int CmdMcpConfigure(int argc, char** argv) {
    auto a = P3(argc, argv, 3, {"--name=", "--command=", "--url=", "--arg=", "--auth=", "--yes"}, "mcp configure"); if (!a) return 2;
    auto ask = [&](const std::string& q, const std::string& d) { if (!isatty(0)) return d; std::cout << q << (d.empty() ? "" : " [" + d + "]") << ": " << std::flush; std::string l; std::getline(std::cin, l); l = Trim(l); return l.empty() ? d : l; };
    std::string name = a->get("--name"); if (name.empty() && !a->pos.empty()) name = a->pos[0]; name = ask("Server name", name);
    if (name.empty()) { std::cerr << "A server name is required (--name).\n"; return 2; }
    std::string url = a->get("--url"), cmd = a->get("--command");
    if (url.empty() && cmd.empty()) { const auto kind = ask("Transport (stdio/http)", "stdio"); if (kind == "http") url = ask("Server URL", ""); else cmd = ask("Command", ""); }
    if (url.empty() && cmd.empty()) { std::cerr << "Provide --command or --url.\n"; return 2; }
    std::vector<std::string> full{SelfExe().string(), "mcp", "add", name};
    if (!url.empty()) { full.push_back("--url"); full.push_back(url); const auto auth = a->has("--auth") ? a->get("--auth") : ask("Auth (none/oauth)", "none"); if (auth == "oauth") { full.push_back("--auth"); full.push_back("oauth"); } }
    else { full.push_back("--command"); full.push_back(cmd); auto args = a->all("--arg"); if (args.empty() && isatty(0)) { std::stringstream ss(ask("Arguments (space separated)", "")); std::string x; while (ss >> x) args.push_back(x); } for (auto& x : args) { full.push_back("--arg"); full.push_back(x); } }
    std::vector<char*> av; for (auto& s : full) av.push_back(const_cast<char*>(s.c_str())); av.push_back(nullptr);
    execv(full[0].c_str(), av.data()); std::perror("exec mcp add"); return 1;
}

// ---------------------------------------------------------------- acp client (drive `acp` over stdio)
int CmdAcpClient(int argc, char** argv) {
    auto a = P3(argc, argv, 3, {"--cwd=", "--server=", "--server-args=", "--server-verbose", "--verbose", "--prompt=", "--approve=", "--session=", "--url=", "--token="}, "acp client"); if (!a) return 2;
    int to_child[2], from_child[2]; if (pipe(to_child) || pipe(from_child)) { std::perror("pipe"); return 1; }
    std::vector<std::string> cargs; if (a->has("--server")) { std::stringstream ss(a->get("--server")); std::string x; while (ss >> x) cargs.push_back(x); } else { cargs = {SelfExe().string(), "acp"}; }
    if (a->has("--server-args")) { std::stringstream ss(a->get("--server-args")); std::string x; while (ss >> x) cargs.push_back(x); }
    if (!a->has("--server")) { for (auto k : {"--url", "--token", "--session"}) if (a->has(k)) { cargs.push_back(k); cargs.push_back(a->get(k)); } if (a->has("--server-verbose")) cargs.push_back("--verbose"); }
    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }
    if (pid == 0) {
        dup2(to_child[0], 0); dup2(from_child[1], 1); close(to_child[1]); close(from_child[0]);
        std::vector<char*> av; for (auto& s : cargs) av.push_back(const_cast<char*>(s.c_str())); av.push_back(nullptr);
        execvp(av[0], av.data()); _exit(127);
    }
    close(to_child[0]); close(from_child[1]);
    FILE* in = fdopen(from_child[0], "r"); const bool verbose = a->has("--verbose"); int next_id = 1;
    auto send = [&](const json& j) { const auto s = j.dump() + "\n"; if (verbose) std::cerr << "> " << s; return ::write(to_child[1], s.data(), s.size()) == static_cast<ssize_t>(s.size()); };
    const std::string approve = a->get("--approve", "ask");
    auto handle = [&](const json& msg) {
        const std::string m = msg.value("method", "");
        if (m == "session/update") {
            const auto& u = msg["params"]["update"]; const auto kind = u.value("sessionUpdate", "");
            if (kind == "agent_message_chunk" && u.contains("content")) std::cout << u["content"].value("text", "") << std::flush;
            else if (kind == "tool_call") std::cerr << "\n[tool] " << u.value("title", u.value("toolCallId", "")) << "\n";
            else if (verbose) std::cerr << "[" << kind << "]\n";
        } else if (m == "session/request_permission" && msg.contains("id")) {
            const auto opts = msg["params"].value("options", json::array()); std::string choice;
            auto find = [&](const std::string& kind) { for (auto& o : opts) if (o.value("kind", "") == kind) return o.value("optionId", std::string()); return std::string(); };
            if (approve == "allow") choice = find("allow_once"); else if (approve == "deny") choice = find("reject_once");
            else if (isatty(0)) {
                std::cerr << "\nPermission requested: " << msg["params"]["toolCall"].value("title", "tool call") << "\n"; for (size_t i = 0; i < opts.size(); ++i) std::cerr << "  " << i + 1 << ") " << opts[i].value("name", opts[i].value("optionId", "")) << "\n";
                std::cerr << "Choose: " << std::flush; std::string l; std::getline(std::cin, l); size_t k = std::strtoul(l.c_str(), nullptr, 10); if (k >= 1 && k <= opts.size()) choice = opts[k - 1].value("optionId", "");
            } else choice = find("reject_once");
            send({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", {{"outcome", choice.empty() ? json{{"outcome", "cancelled"}} : json{{"outcome", "selected"}, {"optionId", choice}}}}}});
        } else if (msg.contains("id") && msg.contains("method")) send({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"error", {{"code", -32601}, {"message", "client does not support " + m}}}});
    };
    auto request = [&](const std::string& m, const json& p) -> json {
        const int id = next_id++; if (!send({{"jsonrpc", "2.0"}, {"id", id}, {"method", m}, {"params", p}})) throw std::runtime_error("acp server closed stdin");
        char* buf = nullptr; size_t cap = 0;
        while (getline(&buf, &cap, in) > 0) {
            std::string l(buf); if (verbose) std::cerr << "< " << l; json j; try { j = json::parse(l); } catch (...) { continue; }
            if (j.contains("id") && !j.contains("method") && j["id"] == id) { free(buf); if (j.contains("error")) throw std::runtime_error(j["error"].value("message", j["error"].dump())); return j.value("result", json::object()); }
            handle(j);
        }
        free(buf); throw std::runtime_error("acp server exited");
    };
    int rc = 0;
    try {
        request("initialize", {{"protocolVersion", 1}, {"clientCapabilities", {{"fs", {{"readTextFile", false}, {"writeTextFile", false}}}, {"terminal", false}}}, {"clientInfo", {{"name", "openclaw-cpp-acp-client"}, {"version", "1"}}}});
        auto s = request("session/new", {{"cwd", a->get("--cwd", fs::current_path().string())}, {"mcpServers", json::array()}});
        const std::string sid = s.value("sessionId", "");
        auto prompt = [&](const std::string& text) { auto r = request("session/prompt", {{"sessionId", sid}, {"prompt", {{{"type", "text"}, {"text", text}}}}}); std::cout << "\n"; if (verbose) std::cerr << "[stop: " << r.value("stopReason", "") << "]\n"; };
        if (a->has("--prompt")) prompt(a->get("--prompt"));
        else { std::string l; if (isatty(0)) std::cerr << "acp client connected (session " << sid << "). Type a prompt; Ctrl-D to exit.\n> " << std::flush;
            while (std::getline(std::cin, l)) { if (!Trim(l).empty()) prompt(l); if (isatty(0)) std::cerr << "> " << std::flush; } }
    } catch (const std::exception& e) { std::cerr << "acp client: " << e.what() << "\n"; rc = 1; }
    close(to_child[1]); fclose(in);
    int st = 0; for (int i = 0; i < 50 && waitpid(pid, &st, WNOHANG) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (waitpid(pid, &st, WNOHANG) == 0) { kill(pid, SIGTERM); waitpid(pid, &st, 0); }
    return rc;
}

}  // namespace

int MaybeLocalCommand3(int argc, char** argv) {
    if (argc >= 2) { const std::string c1 = argv[1];
        if (c1 == "connect") return CmdConnect(argc, argv);
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
    {   // nested groups: `<cmd> <group> [--help]` prints the group's subcommands
        static const std::map<std::pair<std::string, std::string>, std::string> groups = {
            {{"backup", "sqlite"}, "create [--out ID] | list | verify <id> | restore <id> --yes [--dry-run]"},
            {{"backup", "git"}, "init | create [--message M] | log [--limit N] | verify [ref] | restore [ref] --yes"},
            {{"webhooks", "gmail"}, "setup --account EMAIL [--project P] [--push-endpoint URL] [--dry-run] | run --account EMAIL"},
            {{"directory", "list"}, "(use `directory peers list` or `directory groups list`)"},
            {{"directory", "members"}, "(use `directory groups members --group-id ID`)"}};
        auto g = groups.find({cmd, sub});
        if (g != groups.end() && (argc == 3 || std::string(argv[3]) == "--help" || std::string(argv[3]) == "-h")) {
            std::cout << "Usage: openclaw-cpp " << cmd << " " << sub << " <command>\n\nCommands:\n  " << g->second << "\n"; return 0;
        }
    }
    if (cmd == "dashboard" && (sub == "reset" || sub == "uninstall")) { std::vector<char*> av(argv, argv + argc); av.erase(av.begin() + 1); av.push_back(nullptr); return sub == "reset" ? CmdReset(argc - 1, av.data()) : CmdUninstall(argc - 1, av.data()); }
    if (cmd == "mcp" && sub == "serve") return CmdMcpServe(argc, argv);
    if (cmd == "mcp" && sub == "configure") return CmdMcpConfigure(argc, argv);
    if (cmd == "promos" && (sub == "list" || sub == "claim")) return CmdPromos(argc, argv, sub);
    if (cmd == "sandbox" && (sub == "list" || sub == "recreate")) return CmdSandbox(argc, argv, sub);
    if (cmd == "dns" && sub == "setup") return CmdDnsSetup(argc, argv);
    if (cmd == "secrets" && (sub == "apply" || sub == "configure")) return CmdSecrets(argc, argv, sub);
    if (cmd == "backup" && argc >= 4 && sub == "sqlite") { const std::string s3 = argv[3]; if (s3 == "create" || s3 == "list" || s3 == "verify" || s3 == "restore") return CmdBackupSqlite(argc, argv, s3); }
    if (cmd == "backup" && argc >= 4 && sub == "git") { const std::string s3 = argv[3]; if (s3 == "init" || s3 == "create" || s3 == "log" || s3 == "verify" || s3 == "restore") return CmdBackupGit(argc, argv, s3); }
    if (cmd == "webhooks" && argc >= 4 && sub == "gmail") { const std::string s3 = argv[3]; if (s3 == "setup" || s3 == "run") return CmdGmail(argc, argv, s3); }
    if (cmd == "webhooks" && (sub == "setup" || sub == "run")) { std::cerr << "error: unknown command 'webhooks " << sub << "' (did you mean `webhooks gmail " << sub << "`?)\n"; return 2; }
    if (cmd == "node" && sub == "install") return CmdNodeInstall(argc, argv);
    if (cmd == "hooks" && sub == "relay") return CmdHooksRelay(argc, argv);
    if (cmd == "acp" && sub == "client") return CmdAcpClient(argc, argv);
    if (cmd == "mcp" && sub.rfind("-", 0) != 0) return CmdMcp(argc, argv);
    return -1;
}

}  // namespace openclaw::cli
