#include "openclaw/gateway/server.hpp"
#include "openclaw/gateway/methods.hpp"

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
namespace {

constexpr const char* kVersion = "0.4.0-dev";
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

Server::Server(ServerOptions opts, ServerDeps deps) : opts_(std::move(opts)), deps_(std::move(deps)) {}
Server::~Server() { Stop(); if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; } }
void Server::Stop() {
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
        try { HandleClient(client); } catch (const std::exception& e) {
            if (opts_.verbose) std::cerr << "[gateway] " << e.what() << '\n';
        }
        close(client);
    }
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
    running_.store(false);
}

void Server::HandleClient(int client_fd) {
    std::string req;
    char buf[8192];
    while (req.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) return;
        req.append(buf, static_cast<size_t>(n));
        if (req.size() > 1 << 20) return;
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
        HandleWebSocket(client_fd, req);
        return;
    }
    HandleHttp(client_fd, req);
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

    agent::LoopOptions lopts;
    lopts.max_turns = body.value("max_turns", 8);
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


std::string Server::DeliverInbound(const channels::InboundMessage& msg) {
    auto messages = deps_.sessions.Load(msg.session_key);
    std::string content = msg.text;
    if (!msg.sender.empty()) {
        content = "[" + msg.channel + "/" + msg.sender + "] " + msg.text;
    }
    messages.push_back(agent::Message{.role = "user", .content = content});
    auto result = agent::RunAgentLoop(deps_.provider, deps_.tools, messages, {.max_turns = 8});
    if (result.ok) {
        deps_.sessions.Save(msg.session_key, result.messages);
        return result.final_text;
    }
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
    if (method == "GET" && (path == "/" || path == "/v1/status")) {
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
            auto* wh = dynamic_cast<channels::WebhookAdapter*>(adapter);
            if (!wh) {
                send(HttpResponse(400, "Bad Request",
                                  nlohmann::json{{"error", "adapter does not support HTTP inbound"},
                                                 {"id", chan_id},
                                                 {"status", adapter->Status()}}.dump()));
                return;
            }
            auto body = nlohmann::json::parse(BodyOf(req));
            auto deliver = [this](const channels::InboundMessage& m) { return DeliverInbound(m); };
            send(HttpResponse(200, "OK", wh->HandleInbound(body, deliver).dump(2)));
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
            return ok({{"tool", name}, {"result", nlohmann::json::parse(result)}});
        } catch (const std::exception& e) { return err(e.what()); }
    }

    if (method == "sessions.list") return ok({{"sessions", deps_.sessions.List()}});
    if (method == "sessions.get" || method == "sessions.preview" || method == "chat.history") {
        const std::string key = params.value("key", params.value("id", params.value("sessionKey", "")));
        if (key.empty()) return err("missing session key");
        nlohmann::json msgs = nlohmann::json::array();
        for (const auto& m : deps_.sessions.Load(key)) msgs.push_back(m.ToJson());
        return ok({{"key", key}, {"messages", msgs}});
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
        if (method == "sessions.compact" && msgs.size() > 4) {
            // keep system + last 3
            std::vector<agent::Message> kept;
            if (!msgs.empty() && msgs.front().role == "system") kept.push_back(msgs.front());
            auto start = msgs.size() > 3 ? msgs.size() - 3 : 0;
            for (size_t i = start; i < msgs.size(); ++i) kept.push_back(msgs[i]);
            deps_.sessions.Save(key, kept);
            return ok({{"key", key}, {"compacted", true}, {"messages", kept.size()}});
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
    if (method == "sessions.subscribe") {
        return ok({{"subscribed", true}, {"key", params.value("key", "")}, {"note", "event push not yet multiplexed"}});
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
        return ok({{"type", "object"},
                   {"properties",
                    {{"gateway", {{"type", "object"}}},
                     {"agents", {{"type", "object"}}},
                     {"session_dir", {{"type", "string"}}}}}});
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
        return ok({{"saved", true}, {"config", deps_.config.ToJson()}});
    }

    if (method == "channels.status") {
        return ok({{"channels", deps_.channels.ListJson()}, {"adapters", deps_.adapters.StatusJson()}});
    }
    if (method == "channels.start" || method == "channels.stop") {
        const std::string cid = params.value("id", params.value("channel", ""));
        auto* a = deps_.adapters.Get(cid);
        if (!a) return err("unknown adapter: " + cid);
        a->SetEnabled(method == "channels.start");
        return ok({{"id", cid}, {"enabled", a->Enabled()}, {"status", a->Status()}});
    }
    if (method == "channels.pairing.list" || method == "node.pair.list" || method == "device.pair.list") {
        return ok({{"pairing", deps_.pairing.ListJson()}});
    }
    if (method == "channels.pairing.approve") {
        return ok({{"approved", deps_.pairing.Approve(params.value("id", ""))}});
    }
    if (method == "channels.pairing.dismiss") {
        return ok({{"dismissed", deps_.pairing.Dismiss(params.value("id", ""))}});
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
        return ok({{"added", true}, {"job", {{"id", j.id}, {"cron", j.cron_expr}}}});
    }
    if (method == "cron.update") {
        cron::Job j;
        j.id = params.value("id", "");
        j.cron_expr = params.value("cron", params.value("expr", "* * * * *"));
        j.payload = params.value("payload", "");
        j.enabled = params.value("enabled", true);
        if (!deps_.cron.Update(j.id, j)) return err("cron job not found");
        return ok({{"updated", true}, {"id", j.id}});
    }
    if (method == "cron.remove") {
        return ok({{"removed", deps_.cron.Remove(params.value("id", ""))}});
    }
    if (method == "cron.run") {
        auto fired = deps_.cron.Tick(std::chrono::system_clock::now());
        return ok({{"fired", fired}, {"jobs", deps_.cron.ListJson()}});
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

    if (method == "mcp.app.listTools") {
        return ok({{"tools", nlohmann::json::array()},
                   {"note", "use embedded mcp::Client against an MCP server; gateway bridge TODO"}});
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
