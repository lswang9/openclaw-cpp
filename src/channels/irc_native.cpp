#include "openclaw/channels/irc_native.hpp"

#include "openclaw/config/config.hpp"

#include <netdb.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace openclaw::channels {
using json = nlohmann::json;
namespace {

struct Conn {
    int fd = -1; SSL* ssl = nullptr; SSL_CTX* ctx = nullptr; std::mutex wmu;
    bool Write(const std::string& line) {
        std::lock_guard<std::mutex> l(wmu);
        const std::string s = line + "\r\n";
        if (ssl) return SSL_write(ssl, s.data(), (int)s.size()) == (int)s.size();
        return ::send(fd, s.data(), s.size(), MSG_NOSIGNAL) == (ssize_t)s.size();
    }
    int Read(char* b, int n) { return ssl ? SSL_read(ssl, b, n) : (int)::recv(fd, b, (size_t)n, 0); }
    ~Conn() { if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); } if (ctx) SSL_CTX_free(ctx); if (fd >= 0) ::close(fd); }
};
std::mutex g_mu;
std::map<std::string, std::shared_ptr<Conn>> g_conns;

std::string S(const json& cfg, const std::string& ch, const char* key, const char* env, const std::string& def = {}) {
    if (env && *env) if (const char* e = std::getenv(env); e && *e) return e;
    auto v = config::GetPath(cfg, "channels." + ch + "." + key);
    if (v && v->is_string()) return v->get<std::string>();
    if (v && v->is_number_integer()) return std::to_string(v->get<long long>());
    return def;
}
bool B(const json& cfg, const std::string& ch, const char* key, bool def) { auto v = config::GetPath(cfg, "channels." + ch + "." + key); return v && v->is_boolean() ? v->get<bool>() : def; }
void Nap(const std::atomic<bool>& stop, int ms) { for (int i = 0; i < ms / 50 && !stop.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

std::shared_ptr<Conn> Dial(const std::string& host, const std::string& port, bool tls, std::string* why) {
    addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) { *why = "resolve " + host; return nullptr; }
    auto c = std::make_shared<Conn>();
    for (auto* a = res; a; a = a->ai_next) {
        c->fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (c->fd < 0) continue;
        if (::connect(c->fd, a->ai_addr, a->ai_addrlen) == 0) break;
        ::close(c->fd); c->fd = -1;
    }
    freeaddrinfo(res);
    if (c->fd < 0) { *why = "connect " + host + ":" + port; return nullptr; }
    if (tls) {
        c->ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_default_verify_paths(c->ctx); SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, nullptr);
        c->ssl = SSL_new(c->ctx); SSL_set_fd(c->ssl, c->fd); SSL_set_tlsext_host_name(c->ssl, host.c_str()); SSL_set1_host(c->ssl, host.c_str());
        if (SSL_connect(c->ssl) != 1) { *why = "tls handshake with " + host; return nullptr; }
    }
    return c;
}

std::string Clean(std::string s) { for (auto& ch : s) if (ch == '\r' || ch == '\n') ch = ' '; return s; }

}  // namespace

bool IrcNativeSend(const std::string& id, const std::string& target, const std::string& text) {
    std::shared_ptr<Conn> c;
    { std::lock_guard<std::mutex> l(g_mu); auto it = g_conns.find(id); if (it != g_conns.end()) c = it->second; }
    if (!c || target.empty()) return false;
    // one PRIVMSG per line, each kept under ~400 bytes of payload (IRC lines are capped at 512 incl. prefix)
    std::size_t start = 0; bool ok = true;
    while (start <= text.size()) {
        auto nl = text.find('\n', start); std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        for (std::size_t i = 0; i < line.size() || (i == 0 && line.empty() && false); i += 400) ok = c->Write("PRIVMSG " + target + " :" + Clean(line.substr(i, 400))) && ok;
        if (nl == std::string::npos) break; start = nl + 1;
    }
    return ok;
}

void RunIrcClient(const std::string& id, const json& cfg, const std::atomic<bool>& stop, Adapter* ad, const DeliverFn& deliver) {
    std::string host, port, nick, pass; bool tls; json chans = json::array();
    if (id == "twitch") {
        if (S(cfg, "twitch", "mode", "TWITCH_MODE") != "irc") return;
        const auto tok = S(cfg, "twitch", "accessToken", "TWITCH_ACCESS_TOKEN");
        nick = S(cfg, "twitch", "username", "TWITCH_USERNAME");
        if (tok.empty() || nick.empty()) return;
        pass = tok.rfind("oauth:", 0) == 0 ? tok : "oauth:" + tok;
        auto server = S(cfg, "twitch", "ircServer", "TWITCH_IRC_SERVER", "irc.chat.twitch.tv:6697");
        host = server.substr(0, server.rfind(':')); port = server.find(':') == std::string::npos ? "6697" : server.substr(server.rfind(':') + 1);
        tls = B(cfg, "twitch", "tls", port == "6697" || port == "443");
        auto ch = S(cfg, "twitch", "channel", "TWITCH_CHANNEL");
        if (!ch.empty()) chans.push_back(ch[0] == '#' ? ch : "#" + ch);
    } else {
        auto server = S(cfg, "irc", "server", "IRC_SERVER");
        nick = S(cfg, "irc", "nick", "IRC_NICK", "openclaw");
        if (server.empty()) return;
        host = server.find(':') == std::string::npos ? server : server.substr(0, server.rfind(':'));
        port = server.find(':') == std::string::npos ? S(cfg, "irc", "port", "", "6697") : server.substr(server.rfind(':') + 1);
        tls = B(cfg, "irc", "tls", port == "6697");
        pass = S(cfg, "irc", "password", "IRC_PASSWORD");
        if (auto v = config::GetPath(cfg, "channels.irc.channels"); v && v->is_array()) chans = *v;
    }
    std::cerr << "[gateway] " << id << ": native IRC " << host << ":" << port << (tls ? " (tls)" : "") << " as " << nick << "\n";
    int backoff = 1000;
    while (!stop.load()) {
        std::string why;
        auto c = Dial(host, port, tls, &why);
        if (!c) { std::cerr << "[gateway] " << id << " irc: " << why << "\n"; Nap(stop, backoff); backoff = std::min(backoff * 2, 60000); continue; }
        if (id == "twitch") c->Write("CAP REQ :twitch.tv/tags twitch.tv/commands");
        if (!pass.empty()) c->Write("PASS " + pass);
        c->Write("NICK " + nick);
        if (id != "twitch") c->Write("USER " + nick + " 0 * :OpenClaw");
        std::string buf; char rb[4096]; bool welcomed = false;
        while (!stop.load()) {
            if (!c->ssl || SSL_pending(c->ssl) == 0) {
                pollfd p{c->fd, POLLIN, 0};
                const int pr = ::poll(&p, 1, 200);
                if (pr == 0) continue;
                if (pr < 0) break;
            }
            const int n = c->Read(rb, sizeof rb);
            if (n <= 0) break;
            buf.append(rb, (std::size_t)n);
            std::size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos); buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty() && line[0] == '@') { auto sp = line.find(' '); line = sp == std::string::npos ? "" : line.substr(sp + 1); }  // IRCv3 tags
                if (line.rfind("PING", 0) == 0) { c->Write("PONG" + line.substr(4)); continue; }
                std::string prefix; if (!line.empty() && line[0] == ':') { auto sp = line.find(' '); prefix = line.substr(1, sp - 1); line = sp == std::string::npos ? "" : line.substr(sp + 1); }
                auto sp = line.find(' '); const std::string cmd = line.substr(0, sp); const std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
                if (cmd == "001" && !welcomed) {
                    welcomed = true; backoff = 1000;
                    { std::lock_guard<std::mutex> l(g_mu); g_conns[id] = c; }
                    for (auto& ch : chans) if (ch.is_string()) c->Write("JOIN " + ch.get<std::string>());
                    continue;
                }
                if (cmd == "433" && !welcomed) { nick += "_"; c->Write("NICK " + nick); continue; }
                if (cmd != "PRIVMSG") continue;
                const auto from = prefix.substr(0, prefix.find('!'));
                const auto tsp = rest.find(' '); if (tsp == std::string::npos) continue;
                const auto target = rest.substr(0, tsp);
                auto text = rest.substr(tsp + 1); if (!text.empty() && text[0] == ':') text.erase(0, 1);
                if (from == nick || text.empty() || text[0] == '\x01') continue;  // own echo / CTCP
                json body = id == "twitch" ? json{{"message", text}, {"user_id", from}, {"channel", target}}
                                           : json{{"message", text}, {"nick", from}, {"target", target}};
                const auto tok = S(cfg, id, "webhookToken", "");
                if (!tok.empty()) body["token"] = tok;
                try { ad->HandleHttpInbound(body, deliver); } catch (const std::exception& e) { std::cerr << "[gateway] " << id << " irc message: " << e.what() << "\n"; }
            }
        }
        { std::lock_guard<std::mutex> l(g_mu); if (g_conns[id] == c) g_conns.erase(id); }
        if (!stop.load()) { c->Write("QUIT :reconnecting"); Nap(stop, backoff); backoff = std::min(backoff * 2, 60000); }
        else c->Write("QUIT :shutdown");
    }
}

}  // namespace openclaw::channels
