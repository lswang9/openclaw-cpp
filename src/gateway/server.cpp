#include "openclaw/gateway/server.hpp"
#include "openclaw/context/compaction.hpp"
#include "openclaw/gateway/methods.hpp"
#include "openclaw/gateway/extra.hpp"
#include "openclaw/diagnostics/doctor.hpp"
#include "openclaw/logging/tail.hpp"
#include "openclaw/context/assemble.hpp"
#include "openclaw/config/validate.hpp"
#include "openclaw/channels/outbound.hpp"
#include "openclaw/channels/slack.hpp"
#include "openclaw/channels/discord.hpp"
#include "openclaw/channels/discord_commands.hpp"
#include "openclaw/plugins/js_extensions.hpp"
#include "openclaw/agent/routing.hpp"
#include "openclaw/agent/pool.hpp"
#include "openclaw/tools/exec.hpp"
#include "openclaw/channels/signing.hpp"
#include "openclaw/channels/feishu.hpp"
#include "openclaw/channels/whatsapp.hpp"
#include <thread>

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

namespace openclaw::gateway {
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
namespace {

constexpr const char* kVersion = "1.52.0-dev";
constexpr const char* kAlign = "OpenClaw 2026.10.5";

std::string HttpResponse(int code, const std::string& status, const std::string& body,
                         const std::string& content_type = "application/json",
                         const std::string& extra_headers = "") {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << ' ' << status << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << extra_headers << "\r\n"
        << body;
    return oss.str();
}

std::string HeaderValue(const std::string& req, const std::string& name) {
    std::string lower_req = req, lkey = name + ":";
    for (char& c : lower_req) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : lkey) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto pos = lower_req.find(lkey);
    if (pos == std::string::npos) return {};
    auto start = req.find(':', pos) + 1;
    while (start < req.size() && (req[start] == ' ' || req[start] == '\t')) ++start;
    auto end = req.find("\r\n", start);
    return end == std::string::npos ? req.substr(start) : req.substr(start, end - start);
}

std::string BodyOf(const std::string& req) {
    auto pos = req.find("\r\n\r\n");
    return pos == std::string::npos ? std::string{} : req.substr(pos + 4);
}

std::string PathOnly(const std::string& path_with_q) {
    auto q = path_with_q.find('?');
    return q == std::string::npos ? path_with_q : path_with_q.substr(0, q);
}

std::string QueryParam(const std::string& path_with_q, const std::string& key) {
    auto q = path_with_q.find('?');
    if (q == std::string::npos) return {};
    std::string qs = path_with_q.substr(q + 1);
    const std::string needle = key + "=";
    size_t pos = 0;
    while (pos < qs.size()) {
        auto amp = qs.find('&', pos);
        auto part = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (part.rfind(needle, 0) == 0) {
            auto val = part.substr(needle.size());
            // minimal URL decode for %XX and +
            std::string out;
            for (size_t i = 0; i < val.size(); ++i) {
                if (val[i] == '+') out.push_back(' ');
                else if (val[i] == '%' && i + 2 < val.size()) {
                    auto hex = val.substr(i + 1, 2);
                    char* endp = nullptr;
                    long v = std::strtol(hex.c_str(), &endp, 16);
                    out.push_back(static_cast<char>(v));
                    i += 2;
                } else out.push_back(val[i]);
            }
            return out;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}


namespace sha1 {
using u32 = uint32_t;
using u8 = uint8_t;
inline u32 rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }
std::array<u8, 20> hash(const std::string& data) {
    u32 h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::vector<u8> msg(data.begin(), data.end());
    const uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<u8>((bit_len >> (i * 8)) & 0xff));
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        u32 w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (msg[chunk + i * 4] << 24) | (msg[chunk + i * 4 + 1] << 16) |
                   (msg[chunk + i * 4 + 2] << 8) | msg[chunk + i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            u32 temp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    std::array<u8, 20> out{};
    u32 hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = (hs[i] >> 24) & 0xff;
        out[i * 4 + 1] = (hs[i] >> 16) & 0xff;
        out[i * 4 + 2] = (hs[i] >> 8) & 0xff;
        out[i * 4 + 3] = hs[i] & 0xff;
    }
    return out;
}
}  // namespace sha1

std::string Base64(const std::array<uint8_t, 20>& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (uint8_t c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) { out.push_back(T[(val >> valb) & 0x3F]); valb -= 6; }
    }
    if (valb > -6) out.push_back(T[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string WsAccept(const std::string& key) {
    return Base64(sha1::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

std::string WsFrameText(const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    const auto len = payload.size();
    if (len < 126) frame.push_back(static_cast<char>(len));
    else if (len <= 0xffff) {
        frame.push_back(126);
        frame.push_back(static_cast<char>((len >> 8) & 0xff));
        frame.push_back(static_cast<char>(len & 0xff));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((len >> (i * 8)) & 0xff));
    }
    frame += payload;
    return frame;
}

bool ParseWsClientFrame(const std::string& buf, std::string& out_payload) {
    if (buf.size() < 2) return false;
    const auto b1 = static_cast<uint8_t>(buf[1]);
    const bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7f;
    size_t off = 2;
    if (len == 126) {
        if (buf.size() < 4) return false;
        len = (static_cast<uint8_t>(buf[2]) << 8) | static_cast<uint8_t>(buf[3]);
        off = 4;
    } else if (len == 127) return false;
    uint8_t mask[4] = {};
    if (masked) {
        if (buf.size() < off + 4) return false;
        for (int i = 0; i < 4; ++i) mask[i] = static_cast<uint8_t>(buf[off + i]);
        off += 4;
    }
    if (buf.size() < off + len) return false;
    out_payload.resize(static_cast<size_t>(len));
    for (uint64_t i = 0; i < len; ++i) {
        auto c = static_cast<uint8_t>(buf[off + static_cast<size_t>(i)]);
        if (masked) c ^= mask[i % 4];
        out_payload[static_cast<size_t>(i)] = static_cast<char>(c);
    }
    return true;
}

}  // namespace


agent::LoopOptions MakeLoopOpts(const ServerDeps& deps, const nlohmann::json& override_body = nlohmann::json::object()) {
    agent::LoopOptions o;
    const auto& raw = deps.config.raw;
    nlohmann::json agent = raw.value("agent", nlohmann::json::object());
    if (raw.contains("agents") && raw["agents"].contains("defaults")) {
        auto& d = raw["agents"]["defaults"];
        if (d.contains("max_turns")) agent["max_turns"] = d["max_turns"];
        if (d.contains("rate_limit_per_minute")) agent["rate_limit_per_minute"] = d["rate_limit_per_minute"];
    }
    o.max_turns = static_cast<std::size_t>(override_body.value(
        "max_turns", agent.value("max_turns", raw.value("max_turns", 8))));
    o.rate_limit_per_minute = static_cast<std::size_t>(override_body.value(
        "rate_limit_per_minute",
        agent.value("rate_limit_per_minute", raw.value("rate_limit_per_minute", 0))));
    o.identical_tool_warn_after = static_cast<std::size_t>(
        agent.value("identical_tool_warn_after", 2));
    o.identical_tool_break_after = static_cast<std::size_t>(
        agent.value("identical_tool_break_after", 4));
    int timeout_ms = override_body.value(
        "timeout_ms", agent.value("timeout_ms", raw.value("timeout_ms", 0)));
    if (timeout_ms > 0) o.timeout = std::chrono::milliseconds(timeout_ms);
    if (auto* js = deps.js_host) {
        if (js->HasHook("before_tool_call"))
            o.before_tool_call = [js](const std::string& t, const nlohmann::json& p) {
                return js->EmitHook("before_tool_call", {{"toolName", t}, {"params", p}}).value("merged", nlohmann::json::object());
            };
        if (js->HasHook("after_tool_call"))
            o.after_tool_call = [js](const std::string& t, const nlohmann::json& p, const std::string& r, long long ms, const std::string& e) {
                nlohmann::json ev{{"toolName", t}, {"params", p}, {"result", r}, {"durationMs", ms}};
                if (!e.empty()) ev["error"] = e;
                js->EmitHook("after_tool_call", ev);
            };
    }
    if (o.max_turns == 0) o.max_turns = 1;
    return o;
}

Server::Server(ServerOptions opts, ServerDeps deps) : opts_(std::move(opts)), deps_(std::move(deps)) {
    std::error_code ec;
    cfg_mtime_ = std::filesystem::last_write_time(config::DefaultPath(), ec);
    auto& broker = tools::exec::ApprovalBroker::Global();
    broker.has_ui = [this] {
        { std::lock_guard lock(ws_mu_); if (!ws_clients_.empty()) return true; }
        const auto& ctx = tools::exec::CurrentContext();
        return !ctx.channel.empty() && deps_.adapters.Get(ctx.channel) != nullptr;  // chat /approve surface
    };
    broker.on_requested = [this](const nlohmann::json& r) {
        Emit("exec.approval.requested", r);
        const auto& ctx = tools::exec::CurrentContext();
        if (ctx.channel.empty()) return;
        if (auto* out = dynamic_cast<channels::OutboundRecordAdapter*>(deps_.adapters.Get(ctx.channel))) {
            const std::string id = r.value("id", "");
            out->SendText(ctx.chat_id, "🔐 Exec approval required (" + id + "):\n" + r.value("command", "") + "\ncwd: " +
                                           r.value("cwd", "") + "\nReply /approve " + id + " allow-once|allow-always|deny",
                          {{"via", "exec-approval"}});
        }
    };
    broker.on_resolved = [this](const nlohmann::json& r) { Emit("exec.approval.resolved", r); };
}
Server::~Server() { Stop(); if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; } }
void Server::Stop() {
    cancel_agents_.store(true);
    running_.store(false);
    if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
    // Wake accept loop; workers joined at end of Run().
}

std::string Server::AgentSystemPrompt(const std::string& agent_id) {
    auto agents = agent::routing::ListAgents(deps_.config.raw);
    const auto* ad = agent::routing::FindAgent(agents, agent_id);
    if (!ad || (ad->workspace.empty() && ad->system_prompt.empty())) return deps_.system_prompt;
    std::lock_guard l(agent_prompts_mu_);
    auto it = agent_prompts_.find(agent_id);
    if (it != agent_prompts_.end()) return it->second;
    std::string sp = (!ad->workspace.empty() && deps_.system_prompt_for_agent) ? deps_.system_prompt_for_agent(agent_id, ad->workspace)
                                                                              : deps_.system_prompt;
    if (!ad->system_prompt.empty()) sp += "\n\n" + ad->system_prompt;
    agent_prompts_[agent_id] = sp;
    return sp;
}

void Server::PluginSessionHook(const std::string& event, const std::string& key, const std::string& reason) {
    if (deps_.js_host && deps_.js_host->HasHook(event))
        deps_.js_host->EmitHook(event, {{"sessionKey", key}, {"reason", reason}}, {{"sessionKey", key}});
}

nlohmann::json Server::CommandCatalog() const {
    nlohmann::json out = nlohmann::json::array();
    for (auto& c : channels::discord_commands::NativeCommands())
        out.push_back({{"name", c.name}, {"description", c.description}, {"source", "native"}, {"args", c.takes_args}});
    for (auto& s : deps_.skills.List())
        if (s.user_invocable && s.eligible && s.enabled)
            out.push_back({{"name", skills::SlashName(s.name)}, {"description", s.description}, {"source", "skill"}, {"args", true}});
    if (deps_.js_host) {
        auto lc = deps_.js_host->ListCommands();
        for (auto& c : lc.value("commands", nlohmann::json::array()))
            if (c.is_object()) out.push_back({{"name", c.value("name", "")}, {"description", c.value("description", "")},
                                              {"source", "plugin"}, {"args", c.value("acceptsArgs", true)}});
    }
    return out;
}

void Server::RefreshDiscordCommands() {
    auto* dc = dynamic_cast<channels::DiscordAdapter*>(deps_.adapters.Get("discord"));
    if (!dc) return;
    std::vector<channels::discord_commands::CommandDef> defs;
    for (auto& c : CommandCatalog()) defs.push_back({c.value("name", ""), c.value("description", ""), c.value("args", true)});
    dc->SetCommandSpecs(channels::discord_commands::BuildSpecs(defs));
}

void Server::RefreshPluginExtensions() {
    if (!deps_.js_host) return;
    for (auto& id : plugin_channels_) deps_.adapters.Remove(id);
    plugin_channels_.clear();
    auto lc = deps_.js_host->Rpc("list_channels", nlohmann::json::object());
    for (auto& c : lc.value("channels", nlohmann::json::array())) {
        const auto id = c.value("id", "");
        if (id.empty() || deps_.adapters.Get(id)) continue;  // never shadow a built-in channel
        deps_.adapters.Add(std::make_unique<plugins::PluginChannelAdapter>(deps_.js_host, id, c.value("label", id)));
        deps_.channels.Register({id, "plugin channel: " + c.value("label", id), true});
        plugin_channels_.push_back(id);
    }
    if (deps_.router) {
        auto lp = deps_.js_host->Rpc("list_providers", nlohmann::json::object());
        for (auto& p : lp.value("providers", nlohmann::json::array())) {
            const auto id = p.value("id", "");
            if (id.empty() || !p.value("chat", false) || (deps_.router->Has(id) && !plugin_providers_.count(id))) continue;
            std::string model = p.contains("models") && p["models"].is_array() && !p["models"].empty() && p["models"][0].is_string()
                                    ? p["models"][0].get<std::string>() : "default";
            deps_.router->Add(id, std::make_shared<plugins::PluginProvider>(deps_.js_host, id, model));
            plugin_providers_.insert(id);
        }
    }
}

void Server::Run() {
    for (auto* a : deps_.adapters.List()) a->SetConfig(deps_.config.raw);
    RefreshPluginExtensions();
    RefreshDiscordCommands();
    std::size_t nworkers = 8;
    if (const char* e = std::getenv("OPENCLAW_GATEWAY_WORKERS"); e && *e) {
        int v = std::atoi(e); if (v >= 1 && v <= 64) nworkers = static_cast<std::size_t>(v);
    }
    StartWorkers(nworkers);
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opts_.port);
    if (inet_pton(AF_INET, opts_.bind_host.c_str(), &addr.sin_addr) != 1) {
        close(listen_fd_); listen_fd_ = -1;
        throw std::runtime_error("invalid bind host");
    }
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_); listen_fd_ = -1;
        throw std::runtime_error("bind() failed");
    }
    if (listen(listen_fd_, 512) < 0) {
        close(listen_fd_); listen_fd_ = -1;
        throw std::runtime_error("listen() failed");
    }
    cancel_agents_.store(false);
    running_.store(true);
    std::cerr << "[gateway] http+ws://" << opts_.bind_host << ':' << opts_.port
              << " align=" << kAlign << " ver=" << kVersion
              << " methods=" << AdvertisedMethodCount()
              << " implemented=" << ImplementedMethods().size()
              << " auth=" << (deps_.auth.require_auth ? "on" : "off") << '\n';
    while (running_.load()) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int client = accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (client < 0) { if (!running_.load()) break; continue; }
        std::string ip;
        { char ipbuf[INET_ADDRSTRLEN] = {0}; inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof(ipbuf)); ip = ipbuf; }
        EnqueueClient(client, std::move(ip));
    }
    StopWorkers();
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
    running_.store(false);
}

void Server::StartWorkers(std::size_t n) {
    workers_stop_ = false;
    for (std::size_t i = 0; i < n; ++i) {
        workers_.emplace_back([this] {
            for (;;) {
                int fd = -1; std::string ip;
                {
                    std::unique_lock lock(workers_mu_);
                    workers_cv_.wait(lock, [&] { return workers_stop_ || !workers_q_.empty(); });
                    if (workers_stop_ && workers_q_.empty()) return;
                    fd = workers_q_.front().first; ip = std::move(workers_q_.front().second);
                    workers_q_.pop();
                }
                auth::DeviceLinkStore::SetThreadClientIp(ip);
                bool should_close = true;
                try { should_close = HandleClient(fd); } catch (const std::exception& e) {
                    if (opts_.verbose) std::cerr << "[gateway] " << e.what() << '\n';
                    should_close = true;
                }
                if (should_close) close(fd);
            }
        });
    }
}

void Server::StopWorkers() {
    {
        std::lock_guard lock(workers_mu_);
        workers_stop_ = true;
    }
    workers_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    std::lock_guard lock(workers_mu_);
    while (!workers_q_.empty()) { close(workers_q_.front().first); workers_q_.pop(); }
}

void Server::EnqueueClient(int fd, std::string ip) {
    constexpr std::size_t kMaxQueue = 256;
    {
        std::lock_guard lock(workers_mu_);
        if (workers_q_.size() >= kMaxQueue) {
            const char msg[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)::send(fd, msg, sizeof(msg) - 1, MSG_NOSIGNAL);
            close(fd);
            return;
        }
        workers_q_.emplace(fd, std::move(ip));
    }
    workers_cv_.notify_one();
}

bool Server::HandleClient(int client_fd) {
    std::string req;
    char buf[8192];
    while (req.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) return true;
        req.append(buf, static_cast<size_t>(n));
        if (req.size() > 1 << 20) return true;
    }
    const auto cl = HeaderValue(req, "Content-Length");
    if (!cl.empty()) {
        size_t want = static_cast<size_t>(std::stoul(cl));
        auto body_pos = req.find("\r\n\r\n") + 4;
        while (req.size() < body_pos + want) {
            ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
        }
    }
    auto upgrade = HeaderValue(req, "Upgrade");
    for (char& c : upgrade) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (upgrade.find("websocket") != std::string::npos) {
        std::thread([this, client_fd, req]() {
            try { HandleWebSocket(client_fd, req); } catch (...) {}
            { std::lock_guard lock(ws_mu_); ws_clients_.erase(client_fd); }
            close(client_fd);
        }).detach();
        return false;  // thread owns fd
    }
    HandleHttp(client_fd, req);
    return true;
}


nlohmann::json Server::Stub(const std::string& method, const nlohmann::json& params) {
    return {
        {"stub", true},
        {"method", method},
        {"params", params},
        {"align_target", kAlign},
        {"note", "structured placeholder — not yet fully implemented in openclaw-cpp"},
    };
}

void Server::HandleChatCompletions(int client_fd, const nlohmann::json& body, bool stream) {
    auto deps_lock = LockDeps();
    auto send = [&](const std::string& resp) { (void)::send(client_fd, resp.data(), resp.size(), 0); };
    std::vector<agent::Message> messages;
    if (body.contains("messages") && body["messages"].is_array()) {
        for (const auto& m : body["messages"]) messages.push_back(agent::Message::FromJson(m));
    }
    if (messages.empty()) {
        send(HttpResponse(400, "Bad Request", nlohmann::json{{"error", "messages required"}}.dump()));
        return;
    }
    const std::string session_id = body.value("session_id", "");
    if (!session_id.empty()) {
        deps_.sessions.ResetIfStale(session_id, session::PolicyFromConfig(deps_.config.raw));
        auto prior = deps_.sessions.Load(session_id);
        prior.insert(prior.end(), messages.begin(), messages.end());
        messages = std::move(prior);
    }

    if (stream && deps_.deepseek) {
        // SSE headers
        {
            std::ostringstream oss;
            oss << "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                << "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
            const auto hs = oss.str();
            (void)::send(client_fd, hs.data(), hs.size(), 0);
        }
        auto result = deps_.deepseek->CompleteStream(
            messages, deps_.tools.OpenAiToolsArray(),
            [&](std::string_view delta) {
                nlohmann::json chunk{
                    {"id", "chatcmpl-openclaw-cpp"},
                    {"object", "chat.completion.chunk"},
                    {"choices", nlohmann::json::array({
                         {{"index", 0}, {"delta", {{"content", std::string(delta)}}}, {"finish_reason", nullptr}},
                     })},
                };
                const std::string line = "data: " + chunk.dump() + "\n\n";
                (void)::send(client_fd, line.data(), line.size(), 0);
            });
        if (!result.ok) {
            const std::string line = "data: " + nlohmann::json{{"error", result.error}}.dump() + "\n\n";
            (void)::send(client_fd, line.data(), line.size(), 0);
        }
        const std::string done = "data: [DONE]\n\n";
        (void)::send(client_fd, done.data(), done.size(), 0);
        if (!session_id.empty() && result.ok) {
            messages.push_back(result.message);
            deps_.sessions.Save(session_id, messages);
        }
        return;
    }

    agent::LoopOptions lopts = MakeLoopOpts(deps_, body);
    lopts.cancel = &cancel_agents_;
    if (deps_lock.owns_lock()) deps_lock.unlock();
    auto result = agent::RunAgentLoop(deps_.provider, deps_.tools, messages, lopts);
    if (!session_id.empty() && result.ok) deps_.sessions.Save(session_id, result.messages);
    if (!result.ok) {
        int code = (result.error.find("402") != std::string::npos) ? 402 : 502;
        send(HttpResponse(code, code == 402 ? "Payment Required" : "Bad Gateway",
                          nlohmann::json{{"error", {{"message", result.error}, {"type", "api_error"}}}}.dump()));
        return;
    }
    nlohmann::json resp{
        {"id", "chatcmpl-openclaw-cpp"},
        {"object", "chat.completion"},
        {"model", body.value("model", "deepseek-chat")},
        {"choices", nlohmann::json::array({
             {{"index", 0}, {"finish_reason", "stop"},
              {"message", {{"role", "assistant"}, {"content", result.final_text}}}},
         })},
        {"usage", {{"turns", result.turns}}},
    };
    send(HttpResponse(200, "OK", resp.dump()));
}




void Server::PersistAll() const {
    if (!deps_.persist) return;
    deps_.persist->SaveCron(deps_.cron);
    deps_.persist->SavePairing(deps_.pairing);
    deps_.persist->SaveNodes(deps_.nodes);
}

void Server::Emit(const std::string& event, const nlohmann::json& payload) {
    deps_.logs.Append("event", event);
    deps_.bus.Emit(event, payload);
    const auto frame_payload = nlohmann::json{{"type", "event"}, {"event", event}, {"payload", payload}}.dump();
    auto make_frame = [](const std::string& payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x81));
        const auto len = payload.size();
        if (len < 126) frame.push_back(static_cast<char>(len));
        else {
            frame.push_back(126);
            frame.push_back(static_cast<char>((len >> 8) & 0xff));
            frame.push_back(static_cast<char>(len & 0xff));
        }
        frame += payload;
        return frame;
    };
    const auto frame = make_frame(frame_payload);
    std::lock_guard lock(ws_mu_);
    for (int fd : ws_clients_) {
        (void)::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    }
}

nlohmann::json Server::ReloadConfigNow() {
    const auto path = config::DefaultPath();
    std::error_code ec;
    auto mt = std::filesystem::last_write_time(path, ec);
    if (ec) return {{"reloaded", false}, {"reason", "no config file"}};

    bool restart = false;
    bool prompt_changed = false;
    bool plugins_changed = false;
    nlohmann::json restarted = nlohmann::json::array();
    std::vector<std::string> channel_ids_to_restart;
    nlohmann::json warnings;
    std::size_t prompt_chars = 0;
    auto sub = [](const nlohmann::json& raw, const std::string& p) {
        return config::GetPath(raw, p).value_or(nlohmann::json());
    };

    {
        // Load file without locks (immutable path). Then unique deps + reload_mu_ together so we never
        // hold reload_mu_ while blocked on unique (shared holders of deps_mu_ call MaybeReloadConfig).
        if (mt == cfg_mtime_) return {{"reloaded", false}, {"reason", "unchanged"}};
        auto lr = config::Load(path);
        if (!lr.error.empty()) {
            std::lock_guard<std::mutex> g(reload_mu_);
            cfg_mtime_ = mt;
            deps_.logs.Append("warn", "config reload rejected: " + lr.error);
            Emit("config.reload", {{"ok", false}, {"error", lr.error}});
            return {{"reloaded", false}, {"error", lr.error}};
        }
        {
            auto deps_lock = LockDeps();
            std::lock_guard<std::mutex> g(reload_mu_);
            if (mt == cfg_mtime_) return {{"reloaded", false}, {"reason", "unchanged"}};  // lost race
            cfg_mtime_ = mt;
            restart = (config::GetPath(lr.snap.raw, "gateway.port") && lr.snap.gateway_port != deps_.config.gateway_port) ||
                      ((config::GetPath(lr.snap.raw, "gateway.host") || config::GetPath(lr.snap.raw, "gateway.bind")) &&
                       lr.snap.gateway_host != deps_.config.gateway_host);
            auto keep_host = deps_.config.gateway_host; auto keep_port = deps_.config.gateway_port;
            const auto old_raw = deps_.config.raw;
            deps_.config = lr.snap;
            deps_.config.gateway_host = keep_host; deps_.config.gateway_port = keep_port;
            warnings = lr.warnings;
            { std::lock_guard l(agent_prompts_mu_); agent_prompts_.clear(); }
            if (deps_.agent_pool) deps_.agent_pool->SetConfig(deps_.config.raw);
            if (deps_.rebuild_system_prompt) {
                auto np = deps_.rebuild_system_prompt();
                prompt_changed = np != deps_.system_prompt;
                deps_.system_prompt = std::move(np);
            }
            prompt_chars = deps_.system_prompt.size();
            for (auto* a : deps_.adapters.List()) {
                const auto id = a->Id();
                if (!a->Enabled() || sub(old_raw, "channels." + id) == sub(deps_.config.raw, "channels." + id)) continue;
                channel_ids_to_restart.push_back(id);
            }
            plugins_changed = sub(old_raw, "plugins") != sub(deps_.config.raw, "plugins");
        }
    }

    for (const auto& id : channel_ids_to_restart) {
        auto* a = deps_.adapters.Get(id);
        if (!a) continue;
        a->Stop();
        a->SetConfig(deps_.config.raw);
        a->Start([this](const channels::InboundMessage& m) { return DeliverInbound(m); });
        restarted.push_back(id);
    }

    bool plugins_restarted = false;
    if (plugins_changed) {
        auto deps_lock = LockDeps();
        std::lock_guard<std::mutex> g(reload_mu_);
        if (!deps_.plugins.Root().empty()) deps_.plugins.Scan(deps_.plugins.Root());
        if (deps_.js_host && deps_.js_host->Running() && !deps_.plugins.Root().empty()) {
            deps_.js_host->Stop();
            deps_.js_host->SetConfig(deps_.config.raw);
            deps_.js_host->Start(deps_.plugins.Root());
            deps_.js_host->RegisterTools(deps_.tools);
            if (deps_.agent_pool) deps_.agent_pool->SetConfig(deps_.config.raw);
            plugins_restarted = true;
        }
    }
    if (plugins_restarted) RefreshPluginExtensions();
    RefreshDiscordCommands();
    Emit("config.reload", {{"ok", true}, {"restartRequired", restart}, {"warnings", warnings},
                           {"systemPromptChanged", prompt_changed}, {"channelsRestarted", restarted},
                           {"pluginsRestarted", plugins_restarted}});
    deps_.logs.Append("info", std::string("config hot-reloaded") + (restart ? " (gateway bind change needs restart)" : ""));
    return {{"reloaded", true}, {"restartRequired", restart}, {"warnings", warnings},
            {"systemPromptChanged", prompt_changed}, {"channelsRestarted", restarted},
            {"pluginsRestarted", plugins_restarted}, {"systemPromptChars", prompt_chars}};
}


void Server::MaybeReloadConfig() {
    auto mode = deps_.config.raw.is_object() ? config::GetPath(deps_.config.raw, "gateway.reload.mode") : std::nullopt;
    if (mode && mode->is_string() && *mode == "off") return;
    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> g(reload_mu_, std::try_to_lock);
    if (!g.owns_lock()) return;
    if (now - last_reload_check_ < std::chrono::seconds(1)) return;
    last_reload_check_ = now;
    g.unlock();
    ReloadConfigNow();
}

session::Store& Server::SessionsFor(const std::string& agent_id) {
    if (deps_.agent_pool) return deps_.agent_pool->Sessions(agent_id);
    return deps_.sessions;
}
std::shared_ptr<tools::Registry> Server::ToolsFor(const std::string& agent_id) {
    if (deps_.agent_pool) return deps_.agent_pool->Tools(agent_id);
    // non-owning shared_ptr with no-op deleter
    return std::shared_ptr<tools::Registry>(&deps_.tools, [](tools::Registry*) {});
}
std::unique_lock<std::mutex> Server::LockDeps() const {
    return std::unique_lock<std::mutex>(deps_mu_);
}
std::string Server::DeliverInbound(const channels::InboundMessage& msg_in) {
    MaybeReloadConfig();  // may unique-lock; must run before the shared request lock
    auto deps_lock = LockDeps();
    channels::InboundMessage msg = msg_in;
    std::string legacy_migrate_from;
    {
        // Upstream session keys (agent:main:main / :<ch>:group:<id> / :channel: / :thread:), unless the caller
        // passed an explicit key or config opts out (session.keyFormat: "legacy").
        auto fmt = config::GetPath(deps_.config.raw, "session.keyFormat");
        bool legacy_fmt = fmt && fmt->is_string() && *fmt == "legacy";
        if (!msg.explicit_key && !legacy_fmt && !msg.channel.empty() && msg.session_key.rfind("agent:", 0) != 0) {
            session::KeyParts kp;
            agent::routing::RouteInput ri;
            ri.channel = msg.channel;
            ri.account_id = msg.account_id;
            ri.peer_kind = msg.chat_type.empty() ? "direct" : msg.chat_type;
            ri.peer_id = ri.peer_kind == "direct" ? (msg.peer_id.empty() ? msg.sender : msg.peer_id) : msg.conversation_id;
            ri.guild_id = msg.guild_id;
            ri.team_id = msg.team_id;
            auto route = agent::routing::ResolveRoute(deps_.config.raw, ri);
            kp.agent_id = route.agent_id;
            if (route.matched_by != "default")
                Emit("agent.route", {{"agentId", route.agent_id}, {"matchedBy", route.matched_by}, {"channel", msg.channel}});
            kp.channel = msg.channel;
            kp.chat_type = msg.chat_type.empty() ? "direct" : msg.chat_type;
            kp.peer_id = msg.peer_id.empty() ? msg.sender : msg.peer_id;
            kp.conversation_id = msg.conversation_id;
            kp.thread_id = msg.thread_id;
            auto scope = config::GetPath(deps_.config.raw, "session.dmScope");
            legacy_migrate_from = msg.session_key;
            msg.session_key = session::BuildSessionKey(kp, scope && scope->is_string() ? scope->get<std::string>() : "main");
            if (msg.chat_type.empty()) msg.chat_type = kp.chat_type;
            if (legacy_migrate_from == msg.session_key) legacy_migrate_from.clear();
        }
    }
    const std::string agent_id = [&] {
        auto a = agent::routing::AgentFromSessionKey(msg.session_key);
        return a.empty() ? agent::routing::DefaultAgentId(deps_.config.raw) : a;
    }();
    session::Store& sessions = SessionsFor(agent_id);
    std::shared_ptr<tools::Registry> tools_ptr = ToolsFor(agent_id);
    tools::Registry& tools = *tools_ptr;
    if (!legacy_migrate_from.empty() && !sessions.Exists(msg.session_key)) {
        // Prefer the agent store; fall back to the legacy shared store.
        if (sessions.Exists(legacy_migrate_from)) {
            sessions.Save(msg.session_key, sessions.Load(legacy_migrate_from));
            Emit("session.migrated", {{"from", legacy_migrate_from}, {"to", msg.session_key}});
        } else if (deps_.sessions.Exists(legacy_migrate_from)) {
            sessions.Save(msg.session_key, deps_.sessions.Load(legacy_migrate_from));
            Emit("session.migrated", {{"from", legacy_migrate_from}, {"to", msg.session_key}});
        }
    }
    // Channel-bound ACP: slack/feishu/discord → session/prompt + outbound reply
    if (deps_.acp) {
        auto outbound = [this](const std::string& channel, const std::string& chat_id,
                               const std::string& text) -> nlohmann::json {
            auto* adapter = deps_.adapters.Get(channel);
            auto* out = dynamic_cast<channels::OutboundRecordAdapter*>(adapter);
            if (!out) return {{"ok", false}, {"error", "no outbound adapter"}, {"channel", channel}};
            out->SetEnabled(true);
            return out->SendText(chat_id, text, {{"via", "acp-channel-bind"}});
        };
        auto bound = deps_.acp->HandleChannelInbound(msg, outbound);
        if (bound.handled) {
            Emit("acp.channel", {{"channel_key", bound.channel_key},
                                 {"acp_session_id", bound.acp_session_id},
                                 {"reply", bound.reply},
                                 {"outbound", bound.outbound}});
            Emit("session.message", {{"sessionKey", msg.session_key}, {"role", "assistant"},
                                     {"text", bound.reply}, {"via", "acp-bind"}});
            return bound.reply;
        }
    }
    std::string text_in = msg.text;
    if (text_in.rfind("/approve", 0) == 0) {
        std::istringstream is(text_in.substr(8));
        std::string id, decision;
        is >> id >> decision;
        if (id.empty()) {
            auto p = tools::exec::ApprovalBroker::Global().Pending();
            std::string out = p.empty() ? "No pending exec approvals." : "Pending exec approvals:";
            for (auto& r : p) out += "\n" + r.value("id", "") + ": " + r.value("command", "");
            return out;
        }
        if (decision.empty()) decision = "allow-once";
        {
            // Only configured approvers (tools.exec.approvers) or, by default, the requester on the same channel.
            auto req = tools::exec::ApprovalBroker::Global().Get(id);
            auto approvers = config::GetPath(deps_.config.raw, "tools.exec.approvers");
            bool allowed = false;
            if (approvers && approvers->is_array()) {
                for (auto& a : *approvers)
                    if (a.is_string() && (a == msg.sender || a == msg.channel + ":" + msg.sender)) allowed = true;
            } else if (!req.is_null()) {
                allowed = req.value("requester", "") == msg.sender && req.value("channel", "") == msg.channel;
            }
            if (!allowed) return "❌ Not authorized to resolve exec approval " + id + ".";
        }
        bool ok = tools::exec::ApprovalBroker::Global().Resolve(id, decision);
        return ok ? "✅ Exec approval " + id + " resolved: " + decision : "❌ Unknown or already resolved approval: " + id;
    }
    {
        std::string rest;
        if (session::ParseResetCommand(text_in, rest)) {
            PluginSessionHook("session_end", msg.session_key, "reset");
            auto e = sessions.Reset(msg.session_key, "command");
            PluginSessionHook("session_start", msg.session_key, "reset");
            Emit("session.reset", {{"sessionKey", msg.session_key}, {"sessionId", e.value("sessionId", "")}, {"reason", "command"}});
            if (rest.empty()) return "✅ New session started.";
            text_in = rest;
        } else if (sessions.ResetIfStale(msg.session_key, session::PolicyFor(deps_.config.raw, msg.channel,
                                          msg.chat_type.empty() ? session::InferChatType(msg.session_key) : msg.chat_type))) {
            Emit("session.reset", {{"sessionKey", msg.session_key}, {"reason", "policy"}});
            PluginSessionHook("session_end", msg.session_key, "policy");
            PluginSessionHook("session_start", msg.session_key, "policy");
        }
    }
    provider::Provider* cprov = deps_.router ? static_cast<provider::Provider*>(deps_.router) : &deps_.provider;
    {
        std::string instr;
        if (context::ParseCompactCommand(text_in, instr)) {
            auto pol = context::CompactionFromConfig(deps_.config.raw);
            auto cr = context::CompactWithSummary(*cprov, sessions.Load(msg.session_key), pol.keep_recent, instr);
            if (!cr.compacted) return "Nothing to compact.";
            sessions.Save(msg.session_key, cr.messages);
            Emit("session.compacted", {{"sessionKey", msg.session_key}, {"before", cr.before_tokens},
                                       {"after", cr.after_tokens}, {"summarized", cr.summarized}, {"manual", true}});
            return "🧹 Compacted session: ~" + std::to_string(cr.before_tokens) + " → ~" +
                   std::to_string(cr.after_tokens) + " tokens.";
        }
    }
    // Native chat commands (no model turn).
    if (text_in == "/help" || text_in == "/commands") {
        std::string h = "Commands:";
        for (auto& c : CommandCatalog()) h += "\n/" + c.value("name", "") + " — " + c.value("description", "");
        return h;
    }
    if (text_in == "/status") {
        auto msgs = sessions.Load(msg.session_key);
        return "Session " + msg.session_key + ": " + std::to_string(msgs.size()) + " messages, ~" +
               std::to_string(context::EstimateTokens(msgs)) + " tokens.";
    }
    if (text_in == "/stop") {
        std::shared_ptr<std::atomic<bool>> flag;
        { std::lock_guard lock(session_ctrl_mu_);
          auto it = session_cancel_.find(msg.session_key);
          if (it != session_cancel_.end()) flag = it->second;
        }
        if (flag) { flag->store(true); return "⏹ Stopping current run…"; }
        return "No active run to stop.";
    }
    if (text_in == "/model" || text_in.rfind("/model ", 0) == 0) {
        std::string arg;
        if (text_in.size() > 7) arg = text_in.substr(7);
        while (!arg.empty() && (arg[0]==' '||arg[0]=='\t')) arg.erase(arg.begin());
        auto cur = config::GetPath(deps_.config.raw, "agents.defaults.model.primary").value_or(nlohmann::json(""));
        std::string def = cur.is_string() ? cur.get<std::string>() : "";
        if (def.empty()) if (const char* e = std::getenv("OPENCLAW_DEFAULT_PROVIDER")) def = e;
        std::string sess;
        { std::lock_guard lock(session_ctrl_mu_);
          auto it = session_model_.find(msg.session_key);
          if (it != session_model_.end()) sess = it->second;
        }
        if (arg.empty() || arg == "status") {
            return "Current model: " + (sess.empty() ? (def.empty() ? "default" : def) : sess)
                   + (sess.empty() ? " (config)" : " (session override)");
        }
        if (arg == "clear" || arg == "reset" || arg == "default") {
            std::lock_guard lock(session_ctrl_mu_);
            session_model_.erase(msg.session_key);
            return "Model override cleared; using " + (def.empty() ? std::string("default") : def);
        }
        if (arg == "list") {
            std::string out = "Models:";
            if (deps_.router) {
                // Router may expose names via Resolve trial — list config providers
                auto prov = config::GetPath(deps_.config.raw, "models.providers");
                if (prov && prov->is_object()) for (auto& [k,v] : prov->items()) out += "\n- " + k;
            }
            if (out == "Models:") out += "\n- " + (def.empty() ? "default" : def) + " (current default)";
            return out;
        }
        { std::lock_guard lock(session_ctrl_mu_); session_model_[msg.session_key] = arg; }
        return "Model set for this session: " + arg;
    }
    // Plugin commands (api.registerCommand) run before skills and bypass the model.
    if (deps_.js_host && !text_in.empty() && text_in[0] == '/') {
        auto sp = text_in.find_first_of(" \t\n");
        std::string name = text_in.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
        std::string args = sp == std::string::npos ? "" : text_in.substr(sp + 1);
        if (deps_.js_host->HasCommand(name)) {
            auto r = deps_.js_host->RunCommand(name, args, {{"senderId", msg.sender}, {"channel", msg.channel},
                                                             {"sessionKey", msg.session_key}});
            Emit("plugin.command", {{"sessionKey", msg.session_key}, {"command", name}});
            if (r.contains("error")) return "error: " + r["error"].value("message", std::string("plugin command failed"));
            return r.value("text", std::string());
        }
    }
    if (deps_.js_host && deps_.js_host->HasHook("message_received"))
        deps_.js_host->EmitHook("message_received", {{"from", msg.sender}, {"content", text_in}, {"channel", msg.channel}},
                                {{"sessionKey", msg.session_key}, {"channelId", msg.channel}});
    if (auto sc = skills::MatchSkillCommand(deps_.skills.List(), text_in)) {
        Emit("skill.command", {{"sessionKey", msg.session_key}, {"skill", sc->skill}, {"dispatch", sc->dispatch_tool.empty() ? "model" : "tool"}});
        if (!sc->dispatch_tool.empty()) {
            // command-dispatch: tool → deterministic tool call, no model turn
            auto skill_entries = config::GetPath(deps_.config.raw, "skills.entries").value_or(nlohmann::json::object());
            skills::ScopedEnv skill_env(skills::SkillEnvOverrides(deps_.skills.List(), skill_entries));
            std::string out;
            try { out = tools.Invoke(sc->dispatch_tool, skills::DispatchArgs(*sc)); }
            catch (const std::exception& e) { out = std::string("error: ") + e.what(); }
            try {
                auto j = nlohmann::json::parse(out);
                for (auto k : {"text", "output", "stdout", "result"}) if (j.contains(k) && j[k].is_string()) { out = j[k]; break; }
            } catch (...) {}
            return out;
        }
        text_in = sc->prompt;
    }
    auto messages = sessions.Load(msg.session_key);
    if (messages.empty()) PluginSessionHook("session_start", msg.session_key, "new");
    std::string content = text_in;
    if (!msg.sender.empty()) {
        content = "[" + msg.channel + "/" + msg.sender + "] " + text_in;
    }
    messages.push_back(agent::Message{.role = "user", .content = content});
    {
        auto pol = context::CompactionFromConfig(deps_.config.raw);
        if (context::NeedsCompaction(pol, messages)) {
            auto mf = context::MemoryFlushFromConfig(deps_.config.raw);
            if (mf.enabled) {
                // Silent agentic turn so the model can write durable memory before older turns are summarized.
                std::vector<agent::Message> fm;
                fm.push_back({.role = "system", .content = deps_.system_prompt + "\n\n" + mf.system_prompt});
                for (const auto& m : messages) if (m.role != "system") fm.push_back(m);
                fm.push_back({.role = "user", .content = mf.prompt});
                agent::LoopOptions flo = MakeLoopOpts(deps_);
                flo.cancel = &cancel_agents_;
                if (deps_lock.owns_lock()) deps_lock.unlock();
                auto fr = agent::RunAgentLoop(*cprov, tools, fm, flo);
                deps_.logs.Append("info", "memory flush session=" + msg.session_key + " ok=" + (fr.ok ? "1" : "0"));
                Emit("session.memoryFlush", {{"sessionKey", msg.session_key}, {"ok", fr.ok}, {"turns", fr.turns}});
            }
            auto cr = context::CompactWithSummary(*cprov, messages, pol.keep_recent);
            if (cr.compacted) {
                messages = std::move(cr.messages);
                Emit("session.compacted", {{"sessionKey", msg.session_key}, {"before", cr.before_tokens},
                                           {"after", cr.after_tokens}, {"summarized", cr.summarized}, {"manual", false}});
            }
        }
    }
    Emit("session.message", {{"sessionKey", msg.session_key}, {"role", "user"}, {"text", msg.text},
                             {"channel", msg.channel}, {"sender", msg.sender}});
    context::AssembleOptions aopts;
    aopts.system_prompt = AgentSystemPrompt(agent_id);
    if (deps_.js_host && deps_.js_host->HasHook("before_agent_start")) {
        auto h = deps_.js_host->EmitHook("before_agent_start", {{"prompt", content}}, {{"sessionKey", msg.session_key}});
        auto merged = h.value("merged", nlohmann::json::object());
        if (merged.contains("systemPrompt") && merged["systemPrompt"].is_string()) aopts.system_prompt = merged["systemPrompt"];
        if (merged.contains("prependContext") && merged["prependContext"].is_string())
            aopts.system_prompt += "\n\n" + merged["prependContext"].get<std::string>();
    }
    auto assembled = context::Assemble(messages, aopts);
    agent::LoopOptions lopts = MakeLoopOpts(deps_);
    auto run_cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lock(session_ctrl_mu_);
        session_cancel_[msg.session_key] = run_cancel;
    }
    // Session cancel; watcher also mirrors cancel_agents_.
    lopts.cancel = run_cancel.get();
    provider::Provider* prov = &deps_.provider;
    if (deps_.router) prov = deps_.router;
    {
        auto agents = agent::routing::ListAgents(deps_.config.raw);
        if (auto* ad = agent::routing::FindAgent(agents, agent_id); ad && !ad->model.empty() && deps_.router) {
            if (auto* rp = deps_.router->Resolve(ad->model)) prov = rp;
        }
    }
    {
        std::string override_model;
        { std::lock_guard lock(session_ctrl_mu_);
          auto it = session_model_.find(msg.session_key);
          if (it != session_model_.end()) override_model = it->second;
        }
        if (!override_model.empty() && deps_.router) {
            if (auto* rp = deps_.router->Resolve(override_model)) prov = rp;
        }
    }
    auto skill_entries = config::GetPath(deps_.config.raw, "skills.entries").value_or(nlohmann::json::object());
    agent::LoopResult result;
    {
        skills::ScopedEnv skill_env(skills::SkillEnvOverrides(deps_.skills.List(), skill_entries));
        tools::exec::ScopedRunContext exec_ctx({agent_id, msg.session_key, msg.channel,
                                                msg.conversation_id.empty() ? msg.sender : msg.conversation_id, msg.sender});
        if (deps_lock.owns_lock()) deps_lock.unlock();  // do not hold deps_mu_ across the model/tool loop
        // Poll gateway-wide cancel into session flag
        std::thread watch([&] {
            while (run_cancel && !run_cancel->load()) {
                if (cancel_agents_.load()) { run_cancel->store(true); break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
        result = agent::RunAgentLoop(*prov, tools, assembled, lopts);
        run_cancel->store(true);  // stop watcher
        if (watch.joinable()) watch.join();
    }
    {
        std::lock_guard lock(session_ctrl_mu_);
        auto it = session_cancel_.find(msg.session_key);
        if (it != session_cancel_.end() && it->second == run_cancel) session_cancel_.erase(it);
    }
    for (const auto& w : result.warnings) {
        deps_.logs.Append("warn", w);
        Emit("session.tool", {{"warning", w}, {"sessionKey", msg.session_key}});
    }
    if (result.ok) {
        // Persist the full transcript: un-pruned history + only the messages this run produced
        // (never the assembled/pruned view or the injected system prompt).
        std::vector<agent::Message> full = messages;
        for (std::size_t i = assembled.size(); i < result.messages.size(); ++i) full.push_back(result.messages[i]);
        sessions.Save(msg.session_key, full);
        deps_.logs.Append("info", "chat ok session=" + msg.session_key);
        Emit("chat", {{"sessionKey", msg.session_key}, {"text", result.final_text}, {"turns", result.turns},
                      {"warnings", result.warnings}});
        std::string reply = result.final_text;
        if (deps_.js_host && deps_.js_host->HasHook("agent_end"))
            deps_.js_host->EmitHook("agent_end", {{"success", true}, {"turns", result.turns}}, {{"sessionKey", msg.session_key}});
        if (deps_.js_host && deps_.js_host->HasHook("message_sending")) {
            auto h = deps_.js_host->EmitHook("message_sending", {{"to", msg.sender}, {"content", reply}, {"channel", msg.channel}},
                                             {{"sessionKey", msg.session_key}});
            auto merged = h.value("merged", nlohmann::json::object());
            if (merged.value("cancel", false)) return "";
            if (merged.contains("content") && merged["content"].is_string()) reply = merged["content"];
        }
        Emit("session.message", {{"sessionKey", msg.session_key}, {"role", "assistant"}, {"text", reply}});
        return reply;
    }
    Emit("session.message", {{"sessionKey", msg.session_key}, {"role", "error"}, {"text", result.error}});
    return std::string("error: ") + result.error;
}

namespace {
// Role of the caller for the current client thread (each connection runs on its own thread).
// admin = gateway token / auth off / in-process; operator|device = ocd_ device token role.
thread_local std::string g_rpc_role = "admin";
thread_local bool g_rpc_role_pending_device = false;
// Set by AuthRole: true when the only valid credential was the oc_session cookie (CSRF check required).
thread_local bool g_auth_via_cookie = false;
thread_local std::string g_session_csrf;

// Inject a fetch() wrapper that sends the double-submit CSRF header (from the non-HttpOnly oc_csrf cookie).
std::string WithCsrfShim(const std::string& html) {
    static const std::string shim =
        "<script>(function(){var f=window.fetch;window.fetch=function(u,o){o=o||{};var m=(o.method||'GET').toUpperCase();"
        "if(m!=='GET'&&m!=='HEAD'){var c=(document.cookie.match(/(?:^|; )oc_csrf=([^;]*)/)||[])[1];"
        "if(c){var h=new Headers(o.headers||{});h.set('X-CSRF-Token',decodeURIComponent(c));o.headers=h;}}"
        "return f.call(this,u,o);};})();</script>";
    auto pos = html.find("<head>");
    if (pos == std::string::npos) return shim + html;
    return html.substr(0, pos + 6) + shim + html.substr(pos + 6);
}

std::string CookieValue(const std::string& req, const std::string& name) {
    auto cookies = HeaderValue(req, "Cookie");
    std::size_t pos = 0;
    while (pos < cookies.size()) {
        auto end = cookies.find(';', pos);
        auto part = cookies.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        if (part.rfind(name + "=", 0) == 0) return part.substr(name.size() + 1);
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return {};
}

std::string BearerOf(const std::string& authz) {
    if (authz.size() <= 7) return {};
    std::string pre = authz.substr(0, 7);
    for (auto& ch : pre) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (pre != "bearer ") return {};
    auto b = authz.substr(7);
    while (!b.empty() && (b.front() == ' ' || b.front() == '\t')) b.erase(b.begin());
    return b;
}

// RPCs a "device" role may NOT call (pairing/device admin, secrets, config, approvals, plugins, gateway control).
bool DeviceRoleForbidden(const std::string& m) {
    auto starts = [&](const char* p) { return m.rfind(p, 0) == 0; };
    if (m == "pairing.link.start" || m == "pairing.link.claim" || m == "pairing.link.status" ||
        m == "pairing.token.verify")
        return false;
    if (starts("pairing.") || starts("channels.pairing.") || starts("device.pair") || starts("device.token") ||
        starts("node.pair") || starts("secrets.") || starts("config.") || starts("approvals.") ||
        starts("exec.approval") || starts("plugins.install") || starts("plugins.uninstall") ||
        starts("update.") || starts("acp.auth.") || m == "gateway.restart" || m == "gateway.stop" ||
        m == "shutdown")
        return true;
    return false;
}

// Operators may administer pairing but not read/write secrets or rewrite config.
bool OperatorRoleForbidden(const std::string& m) {
    return m.rfind("secrets.", 0) == 0 || m == "exec.approvals.set" || m == "config.set" || m == "config.patch" || m == "config.apply" ||
           m.rfind("update.", 0) == 0 || m == "gateway.restart" || m == "gateway.stop" || m == "shutdown";
}
}  // namespace

std::string Server::AuthRole(const std::string& req) const {
    g_auth_via_cookie = false;
    g_session_csrf.clear();
    if (!deps_.auth.require_auth) return "admin";
    const auto authz = HeaderValue(req, "Authorization");
    const auto xtok = HeaderValue(req, "X-OpenClaw-Token");
    if (!auth::CheckHttpAuth(deps_.auth, authz, xtok)) return "admin";
    if (deps_.device_links) {
        for (const auto& tok : {BearerOf(authz), xtok}) {
            auto role = deps_.device_links->TokenRole(tok);
            if (!role.empty()) return role;
        }
        const auto sid = CookieValue(req, "oc_session");
        if (!sid.empty()) {
            const auto gw_sha = deps_.auth.gateway_token.empty()
                ? std::string() : auth::DeviceLinkStore::Sha256Hex(deps_.auth.gateway_token);
            auto s = deps_.device_links->SessionLookup(sid, gw_sha);
            if (!s.role.empty()) { g_auth_via_cookie = true; g_session_csrf = s.csrf; return s.role; }
        }
    }
    return {};
}

std::optional<std::string> Server::GatewayAuthDeny(const std::string& req) const {
    auto role = AuthRole(req);
    g_rpc_role = role.empty() ? "admin" : role;
    if (!role.empty()) return std::nullopt;
    return "unauthorized: provide Authorization: Bearer <OPENCLAW_GATEWAY_TOKEN or ocd_ device token>, "
           "X-OpenClaw-Token, or log in at /login";
}

void Server::HandleHttp(int client_fd, const std::string& req) {
    std::istringstream first(req);
    std::string method, path_raw, version;
    first >> method >> path_raw >> version;
    const std::string path = PathOnly(path_raw);
    auto send = [&](const std::string& resp) { (void)::send(client_fd, resp.data(), resp.size(), 0); };
    // Deps locking: DeliverInbound / DispatchRpc / HandleChatCompletions take shared (or unique for
    // config.reload). Do not hold a shared lock across this whole handler — reload needs unique.

    // Auth (health exempt)
    g_rpc_role = "admin";
    g_rpc_role_pending_device = false;
    bool public_ok = (path == "/health" || path == "/healthz");
    // Unpaired devices may start/claim/poll a link without a token (operator approval still required).
    if (!public_ok && method == "POST" && path == "/v1/rpc") {
        auto pos = req.find("\r\n\r\n");
        if (pos != std::string::npos) {
            try {
                auto j = nlohmann::json::parse(req.substr(pos + 4));
                auto m = j.value("method", std::string());
                public_ok = (m == "pairing.link.start" || m == "pairing.link.claim" || m == "pairing.link.status");
                if (public_ok && AuthRole(req).empty()) {
                    // run unauthenticated pairing calls with least privilege
                    g_rpc_role_pending_device = true;
                }
            } catch (...) {}
        }
    }
    if (path == "/login" || path == "/ui/login" || path == "/logout") public_ok = true;
    if (!public_ok && deps_.js_host) {
        auto route = deps_.js_host->MatchHttpRoute(path);
        if (!route.is_null() && route.value("auth", "gateway") == "plugin") public_ok = true;
    }
    if (public_ok) g_rpc_role = g_rpc_role_pending_device ? "device" : "admin";
    g_rpc_role_pending_device = false;
    if (!public_ok) {
        auto deny = GatewayAuthDeny(req);
        const bool html_page = method == "GET" &&
            (path == "/" || path == "/control" || path == "/ui" || path == "/board" || path == "/ui/board" ||
             path == "/canvas" || path == "/ui/canvas" || path == "/pairing" || path == "/ui/pairing");
        if (deny && html_page) {
            send(HttpResponse(302, "Found", "", "text/plain", "Location: /login?next=" + path + "\r\n"));
            return;
        }
        if (deny) {
            send(HttpResponse(401, "Unauthorized", nlohmann::json{{"error", *deny}}.dump()));
            return;
        }
        if (method != "GET" && method != "HEAD" && method != "OPTIONS" && g_auth_via_cookie &&
            (g_session_csrf.empty() || HeaderValue(req, "X-CSRF-Token") != g_session_csrf)) {
            send(HttpResponse(403, "Forbidden", nlohmann::json{{"error", "csrf: missing or bad X-CSRF-Token"}}.dump()));
            return;
        }
    }

    if (method == "GET" && (path == "/health" || path == "/healthz")) {
        send(HttpResponse(200, "OK", nlohmann::json{{"ok", true}, {"service", "openclaw-cpp-gateway"}}.dump()));
        return;
    }
    if (method == "GET" && (path == "/" || path == "/control" || path == "/ui")) {
        static const std::string kUi =
#include "control_ui_html.inc"
        ;
        send(HttpResponse(200, "OK", WithCsrfShim(kUi), "text/html; charset=utf-8"));
        return;
    }
    if (method == "GET" && (path == "/board" || path == "/ui/board")) {
        static const std::string kBoard =
#include "board_ui_html.inc"
        ;
        send(HttpResponse(200, "OK", WithCsrfShim(kBoard), "text/html; charset=utf-8"));
        return;
    }
    if (method == "POST" && path == "/login") {
        std::string tok;
        try { tok = nlohmann::json::parse(BodyOf(req)).value("token", std::string()); } catch (...) {}
        if (!deps_.auth.require_auth) {
            send(HttpResponse(200, "OK", nlohmann::json{{"ok", true}, {"role", "admin"}, {"auth_required", false}}.dump()));
            return;
        }
        auto role = AuthRole("GET / HTTP/1.1\r\nAuthorization: Bearer " + tok + "\r\n\r\n");
        if (tok.empty() || role.empty() || !deps_.device_links) {
            send(HttpResponse(401, "Unauthorized", nlohmann::json{{"ok", false}, {"error", "invalid token"}}.dump()));
            return;
        }
        const bool is_gw = !deps_.auth.gateway_token.empty() && tok == deps_.auth.gateway_token;
        auto [sid, csrf] = deps_.device_links->CreateSession(auth::DeviceLinkStore::Sha256Hex(tok), is_gw);
        send(HttpResponse(200, "OK", nlohmann::json{{"ok", true}, {"role", role}, {"csrf", csrf}}.dump(),
                          "application/json",
                          "Set-Cookie: oc_session=" + sid + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000\r\n"
                          "Set-Cookie: oc_csrf=" + csrf + "; Path=/; SameSite=Strict; Max-Age=2592000\r\n"
                          "Set-Cookie: oc_token=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0\r\n"));
        return;
    }
    if (method == "POST" && path == "/logout") {
        const auto sid = CookieValue(req, "oc_session");
        if (!sid.empty() && deps_.device_links) {
            auto role = AuthRole(req);
            if (!role.empty() && g_auth_via_cookie && HeaderValue(req, "X-CSRF-Token") != g_session_csrf) {
                send(HttpResponse(403, "Forbidden", nlohmann::json{{"error", "csrf: missing or bad X-CSRF-Token"}}.dump()));
                return;
            }
            deps_.device_links->DeleteSession(sid);
        }
        send(HttpResponse(200, "OK", nlohmann::json{{"ok", true}}.dump(), "application/json",
                          "Set-Cookie: oc_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0\r\n"
                          "Set-Cookie: oc_csrf=; Path=/; SameSite=Strict; Max-Age=0\r\n"));
        return;
    }
    if (method == "GET" && (path == "/login" || path == "/ui/login")) {
        static const std::string kLogin =
#include "login_ui_html.inc"
        ;
        send(HttpResponse(200, "OK", WithCsrfShim(kLogin), "text/html; charset=utf-8"));
        return;
    }
    if (method == "GET" && path == "/v1/whoami") {
        send(HttpResponse(200, "OK", nlohmann::json{{"role", g_rpc_role}, {"auth_required", deps_.auth.require_auth}}.dump()));
        return;
    }
    if (method == "GET" && (path == "/pairing" || path == "/ui/pairing")) {
        static const std::string kPairing =
#include "pairing_ui_html.inc"
        ;
        send(HttpResponse(200, "OK", WithCsrfShim(kPairing), "text/html; charset=utf-8"));
        return;
    }
    if (method == "GET" && (path == "/canvas" || path == "/ui/canvas")) {
        static const std::string kCanvas =
#include "canvas_ui_html.inc"
        ;
        send(HttpResponse(200, "OK", WithCsrfShim(kCanvas), "text/html; charset=utf-8"));
        return;
    }
    if (method == "GET" && path == "/v1/status") {
        send(HttpResponse(200, "OK", nlohmann::json{
            {"service", "openclaw-cpp"}, {"version", kVersion}, {"align_target", kAlign},
            {"gateway", {{"bind", opts_.bind_host}, {"port", opts_.port}, {"http", true}, {"websocket", true},
                         {"auth", deps_.auth.require_auth}}},
            {"tools", deps_.tools.Names()},
            {"sessions", deps_.sessions.List()},
            {"channels", deps_.channels.ListJson()},
            {"skills", deps_.skills.StatusJson()},
            {"adapters", deps_.adapters.StatusJson()},
            {"approvals_pending", deps_.approvals.ListJson("pending")},
            {"secrets_keys", deps_.secrets.ListJson()},
            {"plugins", deps_.plugins.ListJson()},
            {"methods_advertised", AdvertisedMethodCount()},
            {"methods_implemented", ImplementedMethods()},
            {"events_advertised", AdvertisedEventCount()},
        }.dump(2)));
        return;
    }
    if (method == "GET" && path == "/v1/models") {
        send(HttpResponse(200, "OK", nlohmann::json{
            {"object", "list"},
            {"data", nlohmann::json::array({{{"id", "deepseek-chat"}, {"object", "model"}, {"owned_by", "deepseek"}}})},
        }.dump()));
        return;
    }
    if (method == "GET" && path == "/v1/methods") {
        send(HttpResponse(200, "OK", nlohmann::json{
            {"methods", AdvertisedMethods()},
            {"implemented", ImplementedMethods()},
            {"events", AdvertisedEvents()},
            {"counts", {{"advertised", AdvertisedMethodCount()}, {"implemented", ImplementedMethods().size()},
                        {"events", AdvertisedEventCount()}}},
        }.dump()));
        return;
    }
    if (method == "GET" && path == "/v1/channels") {
        send(HttpResponse(200, "OK", nlohmann::json{{"channels", deps_.channels.ListJson()}}.dump(2)));
        return;
    }
    if (method == "GET" && path == "/v1/config" && g_rpc_role == "device") {
        send(HttpResponse(403, "Forbidden", nlohmann::json{{"error", "forbidden: device role"}}.dump()));
        return;
    }
    if (method == "GET" && path == "/v1/config") {
        send(HttpResponse(200, "OK", deps_.config.ToJson().dump(2)));
        return;
    }
    if (method == "GET" && path == "/v1/tools") {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& n : deps_.tools.Names()) arr.push_back({{"name", n}});
        send(HttpResponse(200, "OK", nlohmann::json{{"tools", arr}, {"openai", deps_.tools.OpenAiToolsArray()}}.dump(2)));
        return;
    }
    if (method == "GET" && path.rfind("/v1/sessions/", 0) == 0 && path.size() > 13 &&
        path.find("/export") != std::string::npos) {
        // /v1/sessions/<id>/export
        auto rest = path.substr(std::string("/v1/sessions/").size());
        auto slash = rest.find('/');
        std::string id = slash == std::string::npos ? rest : rest.substr(0, slash);
        nlohmann::json msgs = nlohmann::json::array();
        for (const auto& m : deps_.sessions.Load(id)) msgs.push_back(m.ToJson());
        send(HttpResponse(200, "OK",
            nlohmann::json{{"id", id}, {"messages", msgs}, {"impl", "real"}}.dump(2)));
        return;
    }
    if (method == "POST" && path == "/v1/rpc") {
        try {
            auto body = nlohmann::json::parse(BodyOf(req));
            auto reply = DispatchRpc(body);
            send(HttpResponse(200, "OK", reply.dump(2)));
        } catch (const std::exception& e) {
            send(HttpResponse(400, "Bad Request", nlohmann::json{{"error", e.what()}}.dump()));
        }
        return;
    }
    if (method == "POST" && path == "/v1/sessions") {
        try {
            auto body = nlohmann::json::parse(BodyOf(req));
            const std::string key = body.value("key", body.value("id", body.value("session_id",
                "session-" + std::to_string(deps_.sessions.List().size() + 1))));
            deps_.sessions.Save(key, {});
            send(HttpResponse(200, "OK", nlohmann::json{{"id", key}, {"created", true}, {"impl", "real"}}.dump(2)));
        } catch (const std::exception& e) {
            send(HttpResponse(400, "Bad Request", nlohmann::json{{"error", e.what()}}.dump()));
        }
        return;
    }
    if (method == "GET" && path == "/v1/sessions") {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& id : deps_.sessions.List()) {
            arr.push_back({{"id", id}, {"messages", deps_.sessions.Load(id).size()}});
        }
        send(HttpResponse(200, "OK", nlohmann::json{{"sessions", arr}}.dump(2)));
        return;
    }
    if (method == "GET" && path.rfind("/v1/media/", 0) == 0) {
        auto id = path.substr(std::string("/v1/media/").size());
        auto bytes = deps_.media.ReadBytes(id);
        if (!bytes) { send(HttpResponse(404, "Not Found", nlohmann::json{{"error","not found"},{"id",id}}.dump())); return; }
        auto meta = deps_.media.Find(id);
        std::string ctype = meta ? meta->content_type : "application/octet-stream";
        std::string hdr =
            "HTTP/1.1 200 OK\r\nContent-Type: " + ctype +
            "\r\nContent-Length: " + std::to_string(bytes->size()) +
            "\r\nConnection: close\r\n\r\n";
        send(hdr);
        send(*bytes);
        return;
    }
    if (method == "GET" && path == "/v1/media") {
        send(HttpResponse(200, "OK", nlohmann::json{{"media", deps_.media.ListJson()}}.dump(2)));
        return;
    }
    if (method == "POST" && path == "/v1/media") {
        try {
            auto j = nlohmann::json::parse(BodyOf(req));
            std::string data = j.value("text", j.value("data", ""));
            if (j.contains("base64")) data = media::Store::DecodeBase64(j.value("base64", ""));
            auto asset = deps_.media.Put(j.value("filename", "upload.bin"),
                                         j.value("content_type", "application/octet-stream"), data);
            send(HttpResponse(200, "OK", nlohmann::json{{"id", asset.id}, {"filename", asset.filename},
                {"bytes", asset.bytes}, {"url", "/v1/media/" + asset.id}, {"impl", "real"}}.dump(2)));
        } catch (const std::exception& e) {
            send(HttpResponse(400, "Bad Request", nlohmann::json{{"error", e.what()}}.dump()));
        }
        return;
    }
    if (method == "GET" && path == "/v1/skills") {
        send(HttpResponse(200, "OK", deps_.skills.StatusJson().dump(2)));
        return;
    }
    if (method == "POST" && path == "/tools/invoke") {
        try {
            auto body = nlohmann::json::parse(BodyOf(req));
            std::string name = body.value("tool", body.value("name", ""));
            auto args = body.contains("args") ? body["args"] : body.value("arguments", nlohmann::json::object());
            auto result = deps_.tools.Invoke(name, args);
            send(HttpResponse(200, "OK", nlohmann::json{{"ok", true}, {"tool", name}, {"result", nlohmann::json::parse(result)}}.dump()));
        } catch (const std::exception& e) {
            send(HttpResponse(400, "Bad Request", nlohmann::json{{"error", e.what()}}.dump()));
        }
        return;
    }
    if (method == "POST" && path == "/v1/chat/completions") {
        try {
            auto body = nlohmann::json::parse(BodyOf(req));
            bool stream = body.value("stream", false);
            HandleChatCompletions(client_fd, body, stream);
        } catch (const std::exception& e) {
            send(HttpResponse(400, "Bad Request", nlohmann::json{{"error", e.what()}}.dump()));
        }
        return;
    }
    
    if (method == "GET" && path == "/v1/adapters") {
        send(HttpResponse(200, "OK", nlohmann::json{{"adapters", deps_.adapters.StatusJson()}}.dump(2)));
        return;
    }
    if (method == "GET" && path == "/v1/approvals") {
        send(HttpResponse(200, "OK", nlohmann::json{{"approvals", deps_.approvals.ListJson()}}.dump(2)));
        return;
    }
    // GET /api/channels/whatsapp/inbound?hub.mode=subscribe&hub.verify_token=&hub.challenge=
    if (method == "GET" && path.rfind("/api/channels/", 0) == 0 &&
        path.find("/inbound") != std::string::npos) {
        auto rest = path.substr(std::string("/api/channels/").size());
        auto slash = rest.find('/');
        std::string chan_id = slash == std::string::npos ? rest : rest.substr(0, slash);
        if (chan_id == "whatsapp") {
            auto* wa = dynamic_cast<channels::WhatsAppCloudAdapter*>(deps_.adapters.Get("whatsapp"));
            if (!wa) {
                send(HttpResponse(404, "Not Found", nlohmann::json{{"error", "whatsapp missing"}}.dump()));
                return;
            }
            auto mode = QueryParam(path_raw, "hub.mode");
            auto token = QueryParam(path_raw, "hub.verify_token");
            auto challenge = QueryParam(path_raw, "hub.challenge");
            if (mode == "subscribe" || !challenge.empty()) {
                auto result = wa->HandleVerifyGet(token, challenge);
                if (!result.value("ok", false)) {
                    send(HttpResponse(403, "Forbidden", result.dump()));
                    return;
                }
                // Meta expects plain-text challenge body
                send(HttpResponse(200, "OK", challenge, "text/plain; charset=utf-8"));
                return;
            }
        }
    }

    // POST /api/channels/<id>/inbound
    if (method == "POST" && path.rfind("/api/channels/", 0) == 0 && path.size() > 14 &&
        path.find("/inbound") != std::string::npos) {
        // Keep shared for adapter/config lookup; drop before DeliverInbound (nested MaybeReload/unique).
        try {
            auto rest = path.substr(std::string("/api/channels/").size());
            auto slash = rest.find('/');
            std::string chan_id = slash == std::string::npos ? rest : rest.substr(0, slash);
            auto* adapter = deps_.adapters.Get(chan_id);
            if (!adapter) {
                send(HttpResponse(404, "Not Found", nlohmann::json{{"error", "unknown channel adapter"}, {"id", chan_id}}.dump()));
                return;
            }
            if (!adapter->SupportsHttpInbound()) {
                send(HttpResponse(400, "Bad Request",
                                  nlohmann::json{{"error", "adapter does not support HTTP inbound"},
                                                 {"id", chan_id},
                                                 {"status", adapter->Status()}}.dump()));
                return;
            }
            const std::string raw_body = BodyOf(req);
            auto cfg_str = [&](const std::string& p, const char* env) {
                if (const char* e = std::getenv(env); e && *e) return std::string(e);
                auto v = config::GetPath(deps_.config.raw, p);
                return v && v->is_string() ? v->get<std::string>() : std::string();
            };
            std::string why;
            if (chan_id == "slack") {
                if (auto secret = cfg_str("channels.slack.signingSecret", "SLACK_SIGNING_SECRET"); !secret.empty()) {
                    const long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    if (!channels::signing::VerifySlack(secret, HeaderValue(req, "X-Slack-Request-Timestamp"), raw_body,
                                                        HeaderValue(req, "X-Slack-Signature"), now, &why)) {
                        send(HttpResponse(401, "Unauthorized", nlohmann::json{{"error", why}}.dump()));
                        return;
                    }
                }
            } else if (chan_id == "discord") {
                if (auto pk = cfg_str("channels.discord.publicKey", "DISCORD_PUBLIC_KEY"); !pk.empty()) {
                    if (!channels::signing::VerifyDiscord(pk, HeaderValue(req, "X-Signature-Timestamp"), raw_body,
                                                          HeaderValue(req, "X-Signature-Ed25519"), &why)) {
                        send(HttpResponse(401, "Unauthorized", nlohmann::json{{"error", why}}.dump()));
                        return;
                    }
                }
            }
            auto ctype = HeaderValue(req, "Content-Type");
            nlohmann::json body = ctype.find("application/x-www-form-urlencoded") != std::string::npos
                                      ? channels::signing::ParseForm(raw_body)
                                      : nlohmann::json::parse(raw_body);
                auto deliver = [this](const channels::InboundMessage& m) { return DeliverInbound(m); };
            auto result = adapter->HandleHttpInbound(body, deliver);
            // Feishu/Slack URL verification
            if (result.contains("challenge") && result.value("mode", "") == "url_verification") {
                send(HttpResponse(200, "OK", nlohmann::json{{"challenge", result["challenge"]}}.dump()));
                return;
            }
            // Discord Interactions PING
            if (result.value("mode", "") == "discord_ping") {
                send(HttpResponse(200, "OK", nlohmann::json{{"type", 1}}.dump()));
                return;
            }
            send(HttpResponse(200, "OK", result.dump(2)));
        } catch (const std::exception& e) {
            send(HttpResponse(400, "Bad Request", nlohmann::json{{"error", e.what()}}.dump()));
        }
        return;
    }

    if (deps_.js_host) try {
        auto route = deps_.js_host->MatchHttpRoute(path);
        if (!route.is_null()) {
            const auto q = path_raw.find('?');
            auto r = deps_.js_host->Rpc("http_request", {{"method", method}, {"path", path},
                                                         {"query", q == std::string::npos ? "" : path_raw.substr(q + 1)},
                                                         {"body", BodyOf(req)}});
            if (std::getenv("OPENCLAW_DEBUG_PLUGIN_HTTP")) std::cerr << "[plugin-http] " << route.dump() << " -> " << r.dump() << "\n";
            if (r.value("handled", false)) {
                std::string ctype = "text/plain";
                std::string extra;
                const auto hdrs = r.value("headers", nlohmann::json::object());
                for (auto& [k, v] : hdrs.items()) {
                    if (k == "content-type") ctype = v.get<std::string>();
                    else if (k != "content-length") extra += k + ": " + v.get<std::string>() + "\r\n";
                }
                int code = r.value("status", 200);
                send(HttpResponse(code, code < 400 ? "OK" : "Error", r.value("body", std::string()), ctype, extra));
                return;
            }
            if (r.contains("error")) {
                send(HttpResponse(500, "Internal Server Error", nlohmann::json{{"error", r["error"]}}.dump()));
                return;
            }
        }
    } catch (const std::exception& e) {
        send(HttpResponse(500, "Internal Server Error", nlohmann::json{{"error", e.what()}}.dump()));
        return;
    }
    send(HttpResponse(404, "Not Found", nlohmann::json{{"error", "not found"}, {"path", path}}.dump()));
}

nlohmann::json Server::DispatchRpc(const nlohmann::json& msg) {
    const std::string method = msg.value("method", msg.value("type", ""));
    if (method == "config.reload") {
        // Exclusive path: ReloadConfigNow takes the unique deps lock. Do not hold shared.
        const auto id = msg.contains("id") ? msg["id"] : nlohmann::json();
        auto payload = ReloadConfigNow();
        nlohmann::json r{{"type", "res"}, {"ok", true}, {"payload", std::move(payload)}};
        if (!id.is_null()) r["id"] = id;
        return r;
    }
    MaybeReloadConfig();
    auto deps_lock = LockDeps();
    if (deps_.js_host && deps_.js_host->HasGatewayMethod(method)) {
        auto p = msg.contains("params") ? msg["params"] : nlohmann::json::object();
        auto r = deps_.js_host->CallGatewayMethod(method, p);
        const auto rid = msg.contains("id") ? msg["id"] : nlohmann::json();
        if (r.contains("error")) return {{"type", "res"}, {"id", rid}, {"ok", false}, {"error", r["error"]}};
        return {{"type", "res"}, {"id", rid}, {"ok", true}, {"payload", r}};
    }
    const auto params = msg.contains("params") ? msg["params"] : nlohmann::json::object();
    const auto id = msg.contains("id") ? msg["id"] : nlohmann::json();
    auto ok = [&](nlohmann::json payload) {
        nlohmann::json r{{"type", "res"}, {"ok", true}, {"payload", std::move(payload)}};
        if (!id.is_null()) r["id"] = id;
        return r;
    };
    auto err = [&](const std::string& e) {
        nlohmann::json r{{"type", "res"}, {"ok", false}, {"error", e}};
        if (!id.is_null()) r["id"] = id;
        return r;
    };

    if (g_rpc_role == "device" && DeviceRoleForbidden(method)) {
        nlohmann::json r{{"type", "res"}, {"ok", false}, {"error", "forbidden: device role cannot call " + method},
                         {"role", "device"}, {"code", 403}};
        if (!id.is_null()) r["id"] = id;
        return r;
    }
    if (g_rpc_role == "operator" && OperatorRoleForbidden(method)) {
        nlohmann::json r{{"type", "res"}, {"ok", false}, {"error", "forbidden: operator role cannot call " + method},
                         {"role", "operator"}, {"code", 403}};
        if (!id.is_null()) r["id"] = id;
        return r;
    }
    if (method == "health" || method == "status" || method == "gateway.status" || method == "system.info") {
        return ok({{"runtime", "running"}, {"version", kVersion}, {"align_target", kAlign},
                   {"tools", deps_.tools.Names()}, {"sessions", deps_.sessions.List()},
                   {"providers", deps_.router ? nlohmann::json(deps_.router->Names()) : nlohmann::json::array()},
                   {"skills", deps_.skills.StatusJson()}, {"adapters", deps_.adapters.StatusJson()},
                   {"auth", deps_.auth.require_auth}});
    }
    if (method == "connect") {
        return ok({{"protocol", 1}, {"server", "openclaw-cpp"}, {"version", kVersion}});
    }
    if (method == "methods.list") {
        return ok({{"methods", AdvertisedMethods()}, {"implemented", ImplementedMethods()}});
    }
    if (method == "events.list") return ok({{"events", AdvertisedEvents()}});

    if (method == "tools.list" || method == "tools.catalog" || method == "tools.effective") {
        return ok({{"tools", deps_.tools.Names()}, {"openai", deps_.tools.OpenAiToolsArray()}});
    }
    if (method == "tools.invoke") {
        const std::string name = params.value("tool", params.value("name", ""));
        if (name.empty()) return err("missing tool name");
        auto args = params.contains("args") ? params["args"] : params.value("arguments", nlohmann::json::object());
        try {
            auto result = deps_.tools.Invoke(name, args);
            nlohmann::json rj;
            try { rj = nlohmann::json::parse(result); } catch (...) { rj = result; }  // plain-text tool output
            Emit("session.tool", {{"tool", name}, {"args", args}, {"result", rj}});
            return ok({{"tool", name}, {"result", rj}});
        } catch (const std::exception& e) { return err(e.what()); }
    }

    if (method == "sessions.list") return ok({{"sessions", deps_.sessions.List()}});
    if (method == "sessions.get" || method == "sessions.preview" || method == "chat.history") {
        const std::string key = params.value("key", params.value("id", params.value("sessionKey", "")));
        if (key.empty()) return err("missing session key");
        auto all = deps_.sessions.Load(key);
        const std::size_t limit = params.value("limit", params.value("page_size", 50));
        const std::size_t offset = params.value("offset", 0);
        nlohmann::json msgs = nlohmann::json::array();
        for (std::size_t i = offset; i < all.size() && msgs.size() < limit; ++i) {
            msgs.push_back(all[i].ToJson());
        }
        return ok({{"key", key}, {"messages", msgs}, {"total", all.size()},
                   {"offset", offset}, {"limit", limit},
                   {"has_more", offset + msgs.size() < all.size()}});
    }
    if (method == "sessions.create") {
        const std::string key = params.value("key", params.value("id", "session-" + std::to_string(deps_.sessions.List().size() + 1)));
        deps_.sessions.Save(key, {});
        return ok({{"key", key}, {"created", true}});
    }
    if (method == "sessions.delete") {
        const std::string key = params.value("key", params.value("id", ""));
        if (key.empty()) return err("missing session key");
        return ok({{"key", key}, {"removed", deps_.sessions.Remove(key)}});
    }
    if (method == "sessions.reset" || method == "sessions.compact") {
        const std::string key = params.value("key", params.value("id", ""));
        if (key.empty()) return err("missing session key");
        auto msgs = deps_.sessions.Load(key);
        if (method == "sessions.compact") {
            const std::size_t keep = params.value("keep_recent", 24);
            auto kept = context::Compact(msgs, keep);
            deps_.sessions.Save(key, kept);
            deps_.logs.Append("info", "compacted session " + key);
            return ok({{"key", key}, {"compacted", true}, {"before", msgs.size()}, {"messages", kept.size()}});
        }
        deps_.sessions.Save(key, {});
        return ok({{"key", key}, {"reset", true}});
    }
    if (method == "sessions.patch") {
        const std::string key = params.value("key", params.value("id", ""));
        if (key.empty()) return err("missing session key");
        if (params.contains("messages") && params["messages"].is_array()) {
            std::vector<agent::Message> msgs;
            for (const auto& m : params["messages"]) msgs.push_back(agent::Message::FromJson(m));
            deps_.sessions.Save(key, msgs);
        }
        return ok({{"key", key}, {"patched", true}});
    }
    if (method == "sessions.subscribe" || method == "sessions.messages.subscribe") {
        events::SubFilter filt;
        const std::string key = params.value("key", params.value("sessionKey", ""));
        if (!key.empty()) filt.session_key = key;
        if (params.contains("event")) filt.event_name = params.value("event", "");
        return ok({{"subscribed", true},
                   {"key", key},
                   {"filter", {{"sessionKey", key}, {"event", params.value("event", "")}}},
                   {"recent", deps_.bus.Recent(16, filt)},
                   {"listeners", deps_.bus.ListenerCount()}});
    }
    if (method == "sessions.send" || method == "chat.send" || method == "chat.inject") {
        const std::string text = params.value("message", params.value("text", ""));
        const std::string session_key = params.value("sessionKey", params.value("session_id", params.value("key", "default")));
        if (text.empty()) return err("missing message");
        channels::InboundMessage im;
        im.channel = "rpc";
        im.sender = params.value("sender", "user");
        im.text = text;
        im.session_key = session_key;
        if (deps_lock.owns_lock()) deps_lock.unlock();  // DeliverInbound may reload then LockDeps
        auto reply = DeliverInbound(im);
        if (reply.rfind("error:", 0) == 0) return err(reply.substr(6));
        return ok({{"sessionKey", session_key}, {"text", reply}});
    }
    if (method == "chat.abort") {
        return ok({{"aborted", true}, {"note", "no active run to abort in single-threaded stub"}});
    }

    if (method == "config.get") return ok(deps_.config.ToJson());
    if (method == "config.schema") {
        auto schema = nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"gateway",
               {{"type", "object"},
                {"properties",
                 {{"host", {{"type", "string"}}},
                  {"port", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}}}}}},
              {"agents",
               {{"type", "object"},
                {"properties",
                 {{"defaults",
                   {{"type", "object"},
                    {"properties", {{"model", {{"type", "string"}}}}}}}}}}},
              {"session_dir", {{"type", "string"}}}}},
            {"validation", config::ValidateLightJson(deps_.config)},
        };
        return ok(schema);
    }
    if (method == "config.schema.lookup") {
        const std::string path = params.value("path", params.value("key", ""));
        auto v = config::ValidateLightJson(deps_.config);
        return ok({{"path", path}, {"validation", v}, {"config", deps_.config.ToJson()}});
    }
    if (method == "config.set" || method == "config.apply" || method == "config.patch") {
        if (params.contains("gateway")) {
            deps_.config.gateway_host = params["gateway"].value("host", deps_.config.gateway_host);
            deps_.config.gateway_port = static_cast<std::uint16_t>(
                params["gateway"].value("port", deps_.config.gateway_port));
        }
        if (params.contains("agents") && params["agents"].contains("defaults")) {
            deps_.config.deepseek_model =
                params["agents"]["defaults"].value("model", deps_.config.deepseek_model);
        }
        if (params.contains("session_dir")) {
            deps_.config.session_dir = params.value("session_dir", deps_.config.session_dir);
        }
        deps_.config.raw.merge_patch(params);
        config::Save(config::DefaultPath(), deps_.config);
        return ok({{"saved", true}, {"config", deps_.config.ToJson()},
                   {"validation", config::ValidateLightJson(deps_.config)}});
    }

    if (method == "channels.status") {
        return ok({{"channels", deps_.channels.ListJson()}, {"adapters", deps_.adapters.StatusJson()}});
    }
    if (method == "channels.start" || method == "channels.stop") {
        const std::string cid = params.value("id", params.value("channel", ""));
        auto* a = deps_.adapters.Get(cid);
        if (!a) return err("unknown adapter: " + cid);
        if (method == "channels.start") {
            a->SetEnabled(true);
            auto deliver = [this](const channels::InboundMessage& m) { return DeliverInbound(m); };
            a->Start(deliver);
        } else {
            a->Stop();
            a->SetEnabled(false);
        }
        return ok({{"id", cid}, {"enabled", a->Enabled()}, {"status", a->Status()}});
    }
    // Legacy pairing RPCs: device links (link-*/codes) route to DeviceLinkStore; channel
    // sender pairing (PairingStore) stays as-is for chat-channel approvals.
    auto link_key = [&]() {
        return params.value("id", params.value("code", params.value("link_id", std::string())));
    };
    if (method == "channels.pairing.list" || method == "node.pair.list" || method == "device.pair.list") {
        nlohmann::json out{{"pairing", deps_.pairing.ListJson()}};
        if (deps_.device_links) out["device_links"] = deps_.device_links->List();
        return ok(out);
    }
    if (method == "device.pair.request" && deps_.device_links) {
        nlohmann::json p = params;
        if (!p.contains("device_name") && params.contains("name")) p["device_name"] = params["name"];
        auto r = deps_.device_links->Start(p);
        r["routed"] = "pairing.link.start";
        return ok(r);
    }
    if (method == "channels.pairing.approve" || method == "device.pair.approve") {
        if (deps_.device_links) {
            auto r = deps_.device_links->Approve(link_key(), params.value("role", std::string("device")));
            if (r.value("ok", false)) { r["approved"] = true; r["routed"] = "pairing.link.approve"; return ok(r); }
        }
        auto ap = deps_.pairing.Approve(params.value("id", ""));
        PersistAll();
        return ok({{"approved", ap}, {"store", "channel_pairing"}});
    }
    if (method == "channels.pairing.dismiss" || method == "device.pair.reject" || method == "device.pair.remove") {
        if (deps_.device_links) {
            auto r = deps_.device_links->Deny(link_key());
            if (r.value("ok", false)) { r["dismissed"] = true; r["removed"] = true; r["routed"] = "pairing.link.deny"; return ok(r); }
        }
        auto di = deps_.pairing.Dismiss(params.value("id", ""));
        PersistAll();
        return ok({{"dismissed", di}, {"removed", di}, {"store", "channel_pairing"}});
    }

    // agents (in-memory via config.raw["agents_list"])
    if (method == "agents.list") {
        if (auto al = config::GetPath(deps_.config.raw, "agents.list"); al && al->is_array() && !al->empty()) {
            nlohmann::json out = nlohmann::json::array();
            for (auto& a : agent::routing::ListAgents(deps_.config.raw))
                out.push_back({{"id", a.id}, {"name", a.name}, {"workspace", a.workspace.string()}, {"model", a.model}, {"default", a.is_default}});
            return ok({{"agents", out}, {"defaultId", agent::routing::DefaultAgentId(deps_.config.raw)}});
        }
        if (deps_.config.raw.contains("agents_list")) {
            return ok({{"agents", deps_.config.raw["agents_list"]}});
        }
        return ok({{"agents", nlohmann::json::array({{{"id", "main"}, {"model", deps_.config.deepseek_model}}})}});
    }
    if (method == "exec.approvals.list" || method == "exec.approval.list") {
        return ok({{"pending", tools::exec::ApprovalBroker::Global().Pending()}});
    }
    if (method == "exec.approval.resolve") {
        const auto id = params.value("id", "");
        const auto decision = params.value("decision", "deny");
        if (!tools::exec::ApprovalBroker::Global().Resolve(id, decision)) return err("unknown or already resolved approval: " + id);
        return ok({{"id", id}, {"decision", decision}, {"resolved", true}});
    }
    if (method == "exec.approvals.get") {
        auto p = tools::exec::DefaultApprovalsPath();
        auto doc = tools::exec::LoadApprovals(p);
        const auto aid = params.value("agentId", agent::routing::DefaultAgentId(deps_.config.raw));
        auto pol = tools::exec::ResolvePolicy(deps_.config.raw, doc, aid, "agent:" + aid + ":main");
        return ok({{"path", p.string()}, {"file", doc},
                   {"effective", {{"agentId", aid}, {"host", pol.host}, {"security", pol.security}, {"ask", pol.ask},
                                  {"askFallback", pol.ask_fallback}, {"allowlist", pol.allowlist.size()},
                                  {"sandboxBackend", pol.sandbox_backend}}}});
    }
    if (method == "exec.approvals.set") {
        if (!params.contains("file") || !params["file"].is_object()) return err("missing file object");
        auto p = tools::exec::DefaultApprovalsPath();
        tools::exec::SaveApprovals(p, params["file"]);
        return ok({{"path", p.string()}, {"saved", true}});
    }
    if (method == "agents.route") {
        agent::routing::RouteInput ri{params.value("channel", ""), params.value("accountId", ""), params.value("peerKind", "direct"),
                                      params.value("peerId", ""), params.value("guildId", ""), params.value("teamId", "")};
        auto r = agent::routing::ResolveRoute(deps_.config.raw, ri);
        return ok({{"agentId", r.agent_id}, {"matchedBy", r.matched_by}, {"bindingIndex", r.binding_index}});
    }
    if (method == "agents.create") {
        auto id = params.value("id", "agent-" + std::to_string(deps_.config.raw.value("agents_list", nlohmann::json::array()).size() + 1));
        if (!deps_.config.raw.contains("agents_list")) deps_.config.raw["agents_list"] = nlohmann::json::array();
        deps_.config.raw["agents_list"].push_back({{"id", id}, {"model", params.value("model", deps_.config.deepseek_model)}});
        config::Save(config::DefaultPath(), deps_.config);
        return ok({{"created", true}, {"id", id}});
    }
    if (method == "agents.update") {
        auto id = params.value("id", "");
        if (!deps_.config.raw.contains("agents_list")) return err("no agents");
        for (auto& a : deps_.config.raw["agents_list"]) {
            if (a.value("id", "") == id) {
                if (params.contains("model")) a["model"] = params["model"];
                config::Save(config::DefaultPath(), deps_.config);
                return ok({{"updated", true}, {"agent", a}});
            }
        }
        return err("agent not found");
    }
    if (method == "agents.delete") {
        auto id = params.value("id", "");
        if (!deps_.config.raw.contains("agents_list")) return err("no agents");
        auto& arr = deps_.config.raw["agents_list"];
        nlohmann::json next = nlohmann::json::array();
        for (auto& a : arr) if (a.value("id", "") != id) next.push_back(a);
        arr = next;
        config::Save(config::DefaultPath(), deps_.config);
        return ok({{"deleted", id}});
    }

    if (method == "skills.status" || method == "skills.library.list") {
        return ok(deps_.skills.StatusJson());
    }
    if (method == "skills.library.read" || method == "skills.detail") {
        auto name = params.value("name", params.value("id", ""));
        auto s = deps_.skills.Find(name);
        if (!s) return err("skill not found: " + name);
        return ok({{"name", s->name}, {"description", s->description}, {"path", s->path.string()},
                   {"body", s->body.substr(0, 4000)}});
    }
    if (method == "skills.search") {
        auto q = params.value("q", params.value("query", ""));
        nlohmann::json hits = nlohmann::json::array();
        for (const auto& s : deps_.skills.List()) {
            if (q.empty() || s.name.find(q) != std::string::npos ||
                s.description.find(q) != std::string::npos) {
                hits.push_back({{"name", s.name}, {"description", s.description}});
            }
        }
        return ok({{"hits", hits}});
    }
    if (method == "plugins.list") return ok({{"plugins", deps_.plugins.ListJson()}});
    if (method == "plugins.setEnabled") {
        return ok({{"stub", true}, {"note", "plugin enable persists after plugin.json writer lands"},
                   {"id", params.value("id", "")}, {"enabled", params.value("enabled", false)}});
    }

    if (method == "cron.list" || method == "cron" || method == "cron.runs") {
        return ok({{"jobs", deps_.cron.ListJson()}});
    }
    if (method == "cron.add") {
        cron::Job j;
        j.id = params.value("id", "job-" + std::to_string(deps_.cron.List().size() + 1));
        j.cron_expr = params.value("cron", params.value("expr", "*/1 * * * *"));
        j.payload = params.value("payload", params.value("message", ""));
        j.enabled = params.value("enabled", true);
        deps_.cron.Add(j);
        PersistAll();
        return ok({{"added", true}, {"job", {{"id", j.id}, {"cron", j.cron_expr}}}});
    }
    if (method == "cron.update") {
        cron::Job j;
        j.id = params.value("id", "");
        j.cron_expr = params.value("cron", params.value("expr", "* * * * *"));
        j.payload = params.value("payload", "");
        j.enabled = params.value("enabled", true);
        if (!deps_.cron.Update(j.id, j)) return err("cron job not found");
        PersistAll();
        return ok({{"updated", true}, {"id", j.id}});
    }
    if (method == "cron.remove") {
        auto rem = deps_.cron.Remove(params.value("id", ""));
        PersistAll();
        return ok({{"removed", rem}});
    }
    if (method == "cron.run") {
        auto fired = deps_.cron.Tick(std::chrono::system_clock::now());
        return ok({{"fired", fired}, {"jobs", deps_.cron.ListJson()}});
    }

    if (method == "memory.set") {
        auto key = params.value("key", "");
        auto value = params.value("value", "");
        if (key.empty()) return err("missing key");
        deps_.memory.Set(key, value);
        return ok({{"ok", true}, {"key", key}});
    }
    if (method == "memory.get") {
        auto key = params.value("key", "");
        auto v = deps_.memory.Get(key);
        if (!v) return ok({{"found", false}, {"key", key}});
        return ok({{"found", true}, {"key", key}, {"value", *v}});
    }
    if (method == "memory.status") {
        return ok({{"keys", deps_.memory.Keys()}, {"count", deps_.memory.Keys().size()}});
    }
    if (method == "memory.search") {
        auto q = params.value("q", params.value("query", ""));
        nlohmann::json hits = nlohmann::json::array();
        for (const auto& k : deps_.memory.Keys()) {
            auto v = deps_.memory.Get(k);
            if (!v) continue;
            if (q.empty() || k.find(q) != std::string::npos || v->find(q) != std::string::npos) {
                hits.push_back({{"key", k}, {"value", *v}});
            }
        }
        return ok({{"hits", hits}});
    }

    // approvals
    if (method == "approval.list" || method == "exec.approval.list" || method == "plugin.approval.list") {
        return ok({{"approvals", deps_.approvals.ListJson(params.value("status", ""))}});
    }
    if (method == "approval.get") {
        return ok(deps_.approvals.GetJson(params.value("id", "")));
    }
    if (method == "approval.resolve" || method == "exec.approval.resolve" || method == "plugin.approval.resolve") {
        bool approve = params.value("approve", params.value("decision", "approve") == std::string("approve"));
        return ok({{"resolved", deps_.approvals.Resolve(params.value("id", ""), approve)}});
    }
    if (method == "exec.approval.request" || method == "plugin.approval.request") {
        std::string kind = method.find("plugin") != std::string::npos ? "plugin" : "exec";
        auto id = deps_.approvals.Create(kind, params.value("summary", params.dump()));
        return ok({{"id", id}, {"status", "pending"}});
    }

    // secrets (never return raw values in list)
    if (method == "secrets.store.list" || method == "secrets.reload") {
        return ok({{"keys", deps_.secrets.ListJson()}});
    }
    if (method == "secrets.store.set") {
        auto key = params.value("key", "");
        auto value = params.value("value", "");
        if (key.empty()) return err("missing key");
        deps_.secrets.Set(key, value);
        return ok({{"set", true}, {"key", key}});
    }
    if (method == "secrets.store.delete") {
        return ok({{"deleted", deps_.secrets.Delete(params.value("key", ""))}});
    }





    if (method == "diagnostics.stability" || method == "doctor.memory.status") {
        nlohmann::json ctx{
            {"version", kVersion},
            {"align_target", kAlign},
            {"methods_implemented", ImplementedMethods().size()},
            {"providers", deps_.router ? nlohmann::json(deps_.router->Names()) : nlohmann::json::array({"default"})},
            {"session_dir", deps_.config.session_dir},
            {"data_dir", deps_.persist ? deps_.persist->Root().string() : ""},
            {"deepseek_key_set", !deps_.auth.gateway_token.empty() || true},
            {"memory_keys", deps_.memory.Keys().size()},
            {"cron_jobs", deps_.cron.List().size()},
        };
        // better deepseek key check via env in doctor ctx from caller — override:
        if (const char* k = std::getenv("DEEPSEEK_API_KEY"); k && *k) ctx["deepseek_key_set"] = true;
        else ctx["deepseek_key_set"] = false;
        auto doc = diagnostics::RunDoctorJson(ctx);
        if (method == "doctor.memory.status") {
            return ok({{"memory", deps_.memory.Dump()}, {"doctor", doc}});
        }
        return ok(doc);
    }

    if (method == "models.list" || method == "models.probe") {
        nlohmann::json models = nlohmann::json::array();
        if (deps_.router) {
            for (const auto& n : deps_.router->Names()) {
                models.push_back({{"id", n}, {"object", "model"}, {"owned_by", n}});
            }
        } else {
            models.push_back({{"id", "deepseek-chat"}, {"object", "model"}});
        }
        if (method == "models.probe") {
            return ok({{"ok", true}, {"models", models}, {"default", deps_.router ? deps_.router->Default() : "deepseek"}});
        }
        return ok({{"object", "list"}, {"data", models}});
    }

    if (method == "chat.startup") {
        return ok({{"ready", true}, {"version", kVersion}, {"providers", deps_.router ? nlohmann::json(deps_.router->Names()) : nlohmann::json::array()}});
    }

    if (method == "sessions.abort") {
        return ok({{"aborted", true}, {"key", params.value("key", "")}, {"note", "single-threaded: no in-flight cancel token yet"}});
    }
    if (method == "sessions.usage") {
        const std::string key = params.value("key", params.value("sessionKey", ""));
        auto msgs = key.empty() ? std::vector<agent::Message>{} : deps_.sessions.Load(key);
        return ok({{"key", key}, {"messages", msgs.size()}, {"sessions", deps_.sessions.List().size()}});
    }

    if (method == "cron.get") {
        auto id = params.value("id", "");
        auto* j = deps_.cron.Find(id);
        if (!j) return err("cron job not found");
        return ok({{"id", j->id}, {"cron", j->cron_expr}, {"payload", j->payload},
                   {"enabled", j->enabled}, {"run_count", j->run_count}, {"last_run_utc", j->last_run_utc}});
    }
    if (method == "cron.history") {
        return ok({{"jobs", deps_.cron.ListJson()}, {"note", "history embedded in job run_count/last_run_utc"}});
    }

    if (method == "skills.bins") {
        nlohmann::json bins = nlohmann::json::array();
        for (const auto& s : deps_.skills.List()) bins.push_back(s.name);
        return ok({{"bins", bins}});
    }
    if (method == "skills.install") {
        return ok({{"installed", false}, {"stub", true},
                   {"note", "copy SKILL.md into skills dir and rescan; online install TODO"},
                   {"name", params.value("name", "")}});
    }

    // memory.set via RPC
    if (method == "memory.set" || (method == "memory.get" && params.contains("value"))) {
        // fallthrough - memory.get handled below; add set:
    }

    // agents.files
    if (method == "agents.files.list") {
        const std::string aid = params.value("agentId", params.value("id", "main"));
        return ok({{"agentId", aid}, {"files", deps_.agent_files.ListJson(aid)}});
    }
    if (method == "agents.files.get") {
        const std::string aid = params.value("agentId", params.value("id", "main"));
        const std::string name = params.value("name", params.value("path", ""));
        auto v = deps_.agent_files.Get(aid, name);
        if (!v) return err("file not found");
        return ok({{"agentId", aid}, {"name", name}, {"content", *v}});
    }
    if (method == "agents.files.set") {
        const std::string aid = params.value("agentId", params.value("id", "main"));
        const std::string name = params.value("name", params.value("path", ""));
        const std::string content = params.value("content", params.value("text", ""));
        if (name.empty()) return err("missing name");
        deps_.agent_files.Set(aid, name, content);
        return ok({{"agentId", aid}, {"name", name}, {"saved", true}});
    }


    if (method == "logs.tail") {
        const std::size_t n = params.value("n", params.value("limit", 100));
        return ok({{"lines", deps_.logs.Tail(n)}, {"size", deps_.logs.Size()}});
    }

    // nodes
    if (method == "node.list") return ok({{"nodes", deps_.nodes.ListJson()}});
    if (method == "node.describe") return ok(deps_.nodes.Describe(params.value("id", params.value("nodeId", ""))));
    if (method == "node.rename") {
        auto id = params.value("id", "");
        auto name = params.value("name", "");
        auto d = deps_.nodes.Describe(id);
        if (d.contains("error")) return err("node not found");
        deps_.nodes.Upsert({.id = id, .name = name, .kind = d.value("kind", "stub"), .online = d.value("online", false)});
        PersistAll();
        return ok({{"id", id}, {"name", name}});
    }

    // media / artifacts
    if (method == "artifacts.list" || method == "assistant.media.get") {
        if (method == "assistant.media.get") return ok(deps_.media.GetJson(params.value("id", "")));
        return ok({{"artifacts", deps_.media.ListJson()}});
    }
    if (method == "artifacts.get") return ok(deps_.media.GetJson(params.value("id", "")));
    // upload via params.base64 or params.text
    if (method == "message.action" && params.value("action", "") == "media.upload") {
        std::string data = params.value("text", params.value("data", ""));
        auto asset = deps_.media.Put(params.value("filename", "upload.bin"),
                                     params.value("content_type", "application/octet-stream"), data);
        Emit("session.message", {{"media_id", asset.id}, {"filename", asset.filename}});
        return ok({{"id", asset.id}, {"filename", asset.filename}, {"bytes", asset.bytes}});
    }

    // channel outbound (telegram/feishu record)
    if (method == "message.action" || method == "conversations.send") {
        const std::string channel = params.value("channel", params.value("to_channel", ""));
        const std::string text = params.value("text", params.value("message", ""));
        const std::string chat_id = params.value("chat_id", params.value("to", ""));
        if (channel.empty() || text.empty()) {
            // fall through if not an outbound send shape
        } else {
            auto* adapter = deps_.adapters.Get(channel);
            auto* out = dynamic_cast<channels::OutboundRecordAdapter*>(adapter);
            if (!out) return err("channel does not support outbound record: " + channel);
            out->SetEnabled(true);
            auto res = out->SendText(chat_id, text, params);
            Emit("chat", {{"outbound", true}, {"channel", channel}, {"chat_id", chat_id}});
            return ok(res);
        }
    }

    if (method == "mcp.app.listTools") {
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& n : deps_.tools.Names()) {
            if (n.rfind("mcp.", 0) == 0) tools.push_back(n);
        }
        return ok({{"tools", tools}, {"note", "mcp.* tools bridged into registry when MCP client attached"}});
    }

    // WhatsApp Cloud API
    if (method.rfind("whatsapp.", 0) == 0 || method == "channels.whatsapp.inbound") {
        auto* wa = dynamic_cast<channels::WhatsAppCloudAdapter*>(deps_.adapters.Get("whatsapp"));
        if (!wa) return err("whatsapp adapter missing");
        auto deliver = [this](const channels::InboundMessage& m) { return DeliverInbound(m); };
        if (method == "whatsapp.status" || method == "whatsapp.cloud.status") {
            return ok(wa->Status());
        }
        if (method == "whatsapp.feed" || method == "whatsapp.cloud.feed" || method == "whatsapp.mock") {
            if (params.contains("frames") && params["frames"].is_array()) {
                std::vector<nlohmann::json> frames;
                for (const auto& f : params["frames"]) frames.push_back(f);
                return ok(wa->RunMockFrames(frames, deliver));
            }
            nlohmann::json payload = params.contains("payload") ? params["payload"] : params;
            return ok(wa->FeedPayload(payload, deliver));
        }
        if (method == "whatsapp.send" || method == "whatsapp.cloud.send") {
            wa->SetEnabled(true);
            return ok(wa->SendText(params.value("to", params.value("chat_id", "")),
                                   params.value("text", params.value("message", "")), params));
        }
        if (method == "channels.whatsapp.inbound") {
            return ok(wa->HandleHttpInbound(params, deliver));
        }
        return ok(wa->Status());
    }

    // Feishu long-connection WS
    if (method.rfind("feishu.", 0) == 0 || method == "channels.feishu.inbound") {
        auto* fs = dynamic_cast<channels::FeishuAdapter*>(deps_.adapters.Get("feishu"));
        if (!fs) return err("feishu adapter missing");
        auto deliver = [this](const channels::InboundMessage& m) { return DeliverInbound(m); };
        if (method == "feishu.status" || method == "feishu.ws.status" ||
            method == "feishu.longconn.status") {
            return ok(fs->Status());
        }
        if (method == "feishu.ws.feed" || method == "feishu.ws.mock" ||
            method == "feishu.longconn.feed") {
            if (params.contains("frames") && params["frames"].is_array()) {
                std::vector<nlohmann::json> frames;
                for (const auto& f : params["frames"]) frames.push_back(f);
                return ok(fs->LongConn().RunMockFrames(frames, deliver));
            }
            nlohmann::json payload = params.contains("payload") ? params["payload"] : params;
            auto handled = fs->LongConn().FeedPayload(payload, deliver);
            return ok({{"ok", true}, {"handled", handled}, {"status", fs->LongConn().Status()},
                       {"impl", "real-sim"}});
        }
        if (method == "feishu.ws.start" || method == "feishu.longconn.start") {
            fs->Start(deliver);
            return ok({{"started", fs->LongConn().Running()},
                       {"status", fs->Status()},
                       {"note", "live needs FEISHU_APP_ID/SECRET (+ FEISHU_WS_URL if discovery fails)"}});
        }
        if (method == "feishu.ws.stop" || method == "feishu.longconn.stop") {
            fs->Stop();
            return ok({{"stopped", true}, {"status", fs->Status()}});
        }
        if (method == "channels.feishu.inbound") {
            return ok(fs->HandleHttpInbound(params, deliver));
        }
        return ok(fs->Status());
    }

    // Discord Gateway WS (real deliver into agent)
    if (method.rfind("discord.", 0) == 0 || method == "channels.discord.inbound") {
        auto* dc = dynamic_cast<channels::DiscordAdapter*>(deps_.adapters.Get("discord"));
        if (!dc) return err("discord adapter missing");
        auto deliver = [this](const channels::InboundMessage& m) { return DeliverInbound(m); };
        if (method == "discord.status" || method == "discord.gateway.status") {
            return ok(dc->Status());
        }
        if (method == "discord.gateway.feed" || method == "discord.gateway.mock") {
            if (params.contains("frames") && params["frames"].is_array()) {
                std::vector<nlohmann::json> frames;
                for (const auto& f : params["frames"]) frames.push_back(f);
                return ok(dc->Gateway().RunMockFrames(frames, deliver));
            }
            nlohmann::json payload = params.contains("payload") ? params["payload"] : params;
            auto handled = dc->Gateway().FeedPayload(payload, deliver);
            return ok({{"ok", true}, {"handled", handled}, {"status", dc->Gateway().Status()},
                       {"impl", "real-sim"}});
        }
        if (method == "discord.commands") {
            RefreshDiscordCommands();
            return ok({{"commands", dc->CommandSpecs()}});
        }
        if (method == "discord.commands.sync") {
            RefreshDiscordCommands();
            return ok(dc->SyncCommands());
        }
        if (method == "discord.gateway.start") {
            RefreshDiscordCommands();
            dc->Start(deliver);
            return ok({{"started", dc->Gateway().Running()},
                       {"status", dc->Status()},
                       {"note", "live requires DISCORD_BOT_TOKEN; else idle/mock"}});
        }
        if (method == "discord.gateway.stop") {
            dc->Stop();
            return ok({{"stopped", true}, {"status", dc->Status()}});
        }
        if (method == "channels.discord.inbound") {
            return ok(dc->HandleHttpInbound(params, deliver));
        }
        return ok(dc->Status());
    }

    // Slack Socket Mode (real deliver into agent)
    if (method.rfind("slack.", 0) == 0 || method == "channels.slack.inbound") {
        auto* sl = dynamic_cast<channels::SlackAdapter*>(deps_.adapters.Get("slack"));
        if (!sl) return err("slack adapter missing");
        auto deliver = [this](const channels::InboundMessage& m) { return DeliverInbound(m); };
        if (method == "slack.status" || method == "slack.socket.status") {
            return ok(sl->Status());
        }
        if (method == "slack.socket.feed" || method == "slack.socket.mock") {
            if (params.contains("frames") && params["frames"].is_array()) {
                std::vector<nlohmann::json> frames;
                for (const auto& f : params["frames"]) frames.push_back(f);
                return ok(sl->Socket().RunMockFrames(frames, deliver));
            }
            nlohmann::json env = params.contains("envelope") ? params["envelope"] : params;
            auto ack = sl->Socket().FeedEnvelope(env, deliver);
            return ok({{"ok", true}, {"ack", ack}, {"status", sl->Socket().Status()}, {"impl", "real-sim"}});
        }
        if (method == "slack.socket.start") {
            sl->Start(deliver);
            return ok({{"started", sl->Socket().Running()},
                       {"status", sl->Status()},
                       {"note", "live requires SLACK_APP_TOKEN; else idle/mock"}});
        }
        if (method == "slack.socket.stop") {
            sl->Stop();
            return ok({{"stopped", true}, {"status", sl->Status()}});
        }
        if (method == "channels.slack.inbound") {
            return ok(sl->HandleHttpInbound(params, deliver));
        }
        return ok(sl->Status());
    }

    {
        auto extra = TryExtraDispatch(deps_, method, params);
        if (extra.handled) {
            if (!extra.ok) return err(extra.error);
            return ok(extra.payload);
        }
    }
    const auto advertised = AdvertisedMethods();
    if (std::find(advertised.begin(), advertised.end(), method) != advertised.end()) {
        return ok(Stub(method, params));
    }
    return err("unknown method: " + method);
}


void Server::HandleWebSocket(int client_fd, const std::string& req) {
    // Optional auth on WS via header
    auto deny = GatewayAuthDeny(req);
    if (!deny && g_auth_via_cookie) {
        // Cookie-authenticated WS: reject cross-site upgrades (CSWSH) by checking Origin against Host.
        const auto origin = HeaderValue(req, "Origin");
        const auto host = HeaderValue(req, "Host");
        if (origin.empty() || (origin != "http://" + host && origin != "https://" + host))
            deny = std::string("forbidden: cookie-authenticated WebSocket requires same-origin Origin");
    }
    if (deny) {
        auto resp = HttpResponse(401, "Unauthorized", nlohmann::json{{"error", *deny}}.dump());
        (void)::send(client_fd, resp.data(), resp.size(), 0);
        return;
    }
    const auto key = HeaderValue(req, "Sec-WebSocket-Key");
    if (key.empty()) {
        auto resp = HttpResponse(400, "Bad Request", nlohmann::json{{"error", "missing Sec-WebSocket-Key"}}.dump());
        (void)::send(client_fd, resp.data(), resp.size(), 0);
        return;
    }
    std::ostringstream hs;
    hs << "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
       << "Sec-WebSocket-Accept: " << WsAccept(key) << "\r\n\r\n";
    const auto hs_s = hs.str();
    (void)::send(client_fd, hs_s.data(), hs_s.size(), 0);
    { std::lock_guard lock(ws_mu_); ws_clients_.insert(client_fd); }


    auto hello = nlohmann::json{
        {"type", "event"}, {"event", "connect.challenge"},
        {"payload", {{"service", "openclaw-cpp"}, {"version", kVersion}, {"align_target", kAlign},
                     {"methods", AdvertisedMethodCount()}, {"implemented", ImplementedMethods().size()}}},
    }.dump();
    { auto frame = WsFrameText(hello); (void)::send(client_fd, frame.data(), frame.size(), 0); }

    char buf[8192];
    while (running_.load()) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        std::string raw(buf, static_cast<size_t>(n));
        if (!raw.empty() && (static_cast<uint8_t>(raw[0]) & 0x0f) == 0x8) break;
        std::string payload;
        if (!ParseWsClientFrame(raw, payload)) continue;
        nlohmann::json reply;
        try { reply = DispatchRpc(nlohmann::json::parse(payload)); }
        catch (...) { reply = {{"type", "res"}, {"ok", false}, {"error", "invalid JSON"}}; }
        auto frame = WsFrameText(reply.dump());
        if (::send(client_fd, frame.data(), frame.size(), 0) <= 0) break;
    }
}

}  // namespace openclaw::gateway
