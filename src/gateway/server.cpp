#include "openclaw/gateway/server.hpp"
#include "openclaw/gateway/methods.hpp"
#include "openclaw/gateway/extra.hpp"
#include "openclaw/diagnostics/doctor.hpp"
#include "openclaw/logging/tail.hpp"
#include "openclaw/context/assemble.hpp"
#include "openclaw/config/validate.hpp"
#include "openclaw/channels/outbound.hpp"
#include "openclaw/channels/slack.hpp"
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

constexpr const char* kVersion = "1.11.0-dev";
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
    if (o.max_turns == 0) o.max_turns = 1;
    return o;
}

Server::Server(ServerOptions opts, ServerDeps deps) : opts_(std::move(opts)), deps_(std::move(deps)) {}
Server::~Server() { Stop(); if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; } }
void Server::Stop() {
    cancel_agents_.store(true);
    running_.store(false);
    if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
}

void Server::Run() {
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
    if (listen(listen_fd_, 64) < 0) {
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
        bool should_close = true;
        try { should_close = HandleClient(client); } catch (const std::exception& e) {
            if (opts_.verbose) std::cerr << "[gateway] " << e.what() << '\n';
            should_close = true;
        }
        if (should_close) close(client);
    }
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
    running_.store(false);
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

std::string Server::DeliverInbound(const channels::InboundMessage& msg) {
    auto messages = deps_.sessions.Load(msg.session_key);
    std::string content = msg.text;
    if (!msg.sender.empty()) {
        content = "[" + msg.channel + "/" + msg.sender + "] " + msg.text;
    }
    messages.push_back(agent::Message{.role = "user", .content = content});
    Emit("session.message", {{"sessionKey", msg.session_key}, {"role", "user"}, {"text", msg.text},
                             {"channel", msg.channel}, {"sender", msg.sender}});
    context::AssembleOptions aopts;
    aopts.system_prompt = deps_.system_prompt;
    auto assembled = context::Assemble(messages, aopts);
    agent::LoopOptions lopts = MakeLoopOpts(deps_);
    lopts.cancel = &cancel_agents_;
    provider::Provider* prov = &deps_.provider;
    if (deps_.router) prov = deps_.router;
    auto result = agent::RunAgentLoop(*prov, deps_.tools, assembled, lopts);
    for (const auto& w : result.warnings) {
        deps_.logs.Append("warn", w);
        Emit("session.tool", {{"warning", w}, {"sessionKey", msg.session_key}});
    }
    if (result.ok) {
        deps_.sessions.Save(msg.session_key, result.messages);
        deps_.logs.Append("info", "chat ok session=" + msg.session_key);
        Emit("chat", {{"sessionKey", msg.session_key}, {"text", result.final_text}, {"turns", result.turns},
                      {"warnings", result.warnings}});
        Emit("session.message", {{"sessionKey", msg.session_key}, {"role", "assistant"}, {"text", result.final_text}});
        return result.final_text;
    }
    Emit("session.message", {{"sessionKey", msg.session_key}, {"role", "error"}, {"text", result.error}});
    return std::string("error: ") + result.error;
}

void Server::HandleHttp(int client_fd, const std::string& req) {
    std::istringstream first(req);
    std::string method, path, version;
    first >> method >> path >> version;
    auto send = [&](const std::string& resp) { (void)::send(client_fd, resp.data(), resp.size(), 0); };

    // Auth (health exempt)
    const bool public_ok = (path == "/health" || path == "/healthz");
    if (!public_ok) {
        auto deny = auth::CheckHttpAuth(deps_.auth, HeaderValue(req, "Authorization"),
                                        HeaderValue(req, "X-OpenClaw-Token"));
        if (deny) {
            send(HttpResponse(401, "Unauthorized", nlohmann::json{{"error", *deny}}.dump()));
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
        send(HttpResponse(200, "OK", kUi, "text/html; charset=utf-8"));
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
    // POST /api/channels/<id>/inbound
    if (method == "POST" && path.rfind("/api/channels/", 0) == 0 && path.size() > 14 &&
        path.find("/inbound") != std::string::npos) {
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
            auto body = nlohmann::json::parse(BodyOf(req));
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

    send(HttpResponse(404, "Not Found", nlohmann::json{{"error", "not found"}, {"path", path}}.dump()));
}

nlohmann::json Server::DispatchRpc(const nlohmann::json& msg) {
    const std::string method = msg.value("method", msg.value("type", ""));
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
            Emit("session.tool", {{"tool", name}, {"args", args}, {"result", nlohmann::json::parse(result)}});
            return ok({{"tool", name}, {"result", nlohmann::json::parse(result)}});
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
    if (method == "channels.pairing.list" || method == "node.pair.list" || method == "device.pair.list") {
        return ok({{"pairing", deps_.pairing.ListJson()}});
    }
    if (method == "channels.pairing.approve") {
        auto ap = deps_.pairing.Approve(params.value("id", ""));
        PersistAll();
        return ok({{"approved", ap}});
    }
    if (method == "channels.pairing.dismiss") {
        auto di = deps_.pairing.Dismiss(params.value("id", ""));
        PersistAll();
        return ok({{"dismissed", di}});
    }

    // agents (in-memory via config.raw["agents_list"])
    if (method == "agents.list") {
        if (deps_.config.raw.contains("agents_list")) {
            return ok({{"agents", deps_.config.raw["agents_list"]}});
        }
        return ok({{"agents", nlohmann::json::array({{{"id", "main"}, {"model", deps_.config.deepseek_model}}})}});
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

    // device pairing deepen
    if (method == "device.pair.approve") {
        auto ap = deps_.pairing.Approve(params.value("id", ""));
        PersistAll();
        return ok({{"approved", ap}});
    }
    if (method == "device.pair.reject" || method == "device.pair.remove") {
        auto rm = deps_.pairing.Dismiss(params.value("id", ""));
        PersistAll();
        return ok({{"removed", rm}});
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
    auto deny = auth::CheckHttpAuth(deps_.auth, HeaderValue(req, "Authorization"),
                                    HeaderValue(req, "X-OpenClaw-Token"));
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
