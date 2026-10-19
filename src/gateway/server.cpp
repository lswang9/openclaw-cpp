#include "openclaw/gateway/server.hpp"
#include "openclaw/gateway/methods.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

namespace openclaw::gateway {
namespace {

constexpr const char* kVersion = "0.2.0-dev";
constexpr const char* kAlign = "OpenClaw 2026.10.5";

std::string HttpResponse(int code, const std::string& status, const std::string& body,
                         const std::string& content_type = "application/json") {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << ' ' << status << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "\r\n"
        << body;
    return oss.str();
}

bool StartsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string HeaderValue(const std::string& req, const std::string& name) {
    const std::string key = name + ":";
    auto pos = req.find(key);
    if (pos == std::string::npos) {
        // case-insensitive light search
        std::string lower = req;
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::string lkey = key;
        for (char& c : lkey) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        pos = lower.find(lkey);
        if (pos == std::string::npos) {
            return {};
        }
    }
    auto start = req.find(':', pos) + 1;
    while (start < req.size() && (req[start] == ' ' || req[start] == '\t')) {
        ++start;
    }
    auto end = req.find("\r\n", start);
    if (end == std::string::npos) {
        return req.substr(start);
    }
    return req.substr(start, end - start);
}

std::string BodyOf(const std::string& req) {
    auto pos = req.find("\r\n\r\n");
    if (pos == std::string::npos) {
        return {};
    }
    return req.substr(pos + 4);
}

// Minimal WebSocket accept key (RFC6455) — SHA1+base64 of key+GUID.
// We implement a tiny SHA1 to avoid new deps.
namespace sha1 {
using u32 = uint32_t;
using u8 = uint8_t;

inline u32 rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

std::array<u8, 20> hash(const std::string& data) {
    u32 h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::vector<u8> msg(data.begin(), data.end());
    const uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<u8>((bit_len >> (i * 8)) & 0xff));
    }
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        u32 w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (msg[chunk + i * 4] << 24) | (msg[chunk + i * 4 + 1] << 16) |
                   (msg[chunk + i * 4 + 2] << 8) | msg[chunk + i * 4 + 3];
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        u32 a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            u32 temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
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
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (uint8_t c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(T[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {
        out.push_back(T[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    while (out.size() % 4) {
        out.push_back('=');
    }
    return out;
}

std::string WsAccept(const std::string& key) {
    const auto dig = sha1::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return Base64(dig);
}

std::string WsFrameText(const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));  // FIN + text
    const auto len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 0xffff) {
        frame.push_back(126);
        frame.push_back(static_cast<char>((len >> 8) & 0xff));
        frame.push_back(static_cast<char>(len & 0xff));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((len >> (i * 8)) & 0xff));
        }
    }
    frame += payload;
    return frame;
}

bool ParseWsClientFrame(const std::string& buf, std::string& out_payload) {
    if (buf.size() < 2) {
        return false;
    }
    const auto b0 = static_cast<uint8_t>(buf[0]);
    const auto b1 = static_cast<uint8_t>(buf[1]);
    const bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7f;
    size_t off = 2;
    if (len == 126) {
        if (buf.size() < 4) {
            return false;
        }
        len = (static_cast<uint8_t>(buf[2]) << 8) | static_cast<uint8_t>(buf[3]);
        off = 4;
    } else if (len == 127) {
        return false;  // oversized for stub
    }
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (buf.size() < off + 4) {
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            mask[i] = static_cast<uint8_t>(buf[off + i]);
        }
        off += 4;
    }
    if (buf.size() < off + len) {
        return false;
    }
    out_payload.resize(static_cast<size_t>(len));
    for (uint64_t i = 0; i < len; ++i) {
        auto c = static_cast<uint8_t>(buf[off + static_cast<size_t>(i)]);
        if (masked) {
            c ^= mask[i % 4];
        }
        out_payload[static_cast<size_t>(i)] = static_cast<char>(c);
    }
    (void)b0;
    return true;
}

}  // namespace

Server::Server(ServerOptions opts, provider::Provider& provider, tools::Registry& registry,
               session::Store& sessions, channels::Registry& channels, config::Snapshot config)
    : opts_(std::move(opts)),
      provider_(provider),
      registry_(registry),
      sessions_(sessions),
      channels_(channels),
      config_(std::move(config)) {}

Server::~Server() {
    Stop();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

void Server::Stop() {
    running_.store(false);
    if (listen_fd_ >= 0) {
        // unblock accept
        shutdown(listen_fd_, SHUT_RDWR);
    }
}

void Server::Run() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("socket() failed");
    }
    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opts_.port);
    if (inet_pton(AF_INET, opts_.bind_host.c_str(), &addr.sin_addr) != 1) {
        close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("invalid bind host: " + opts_.bind_host);
    }
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("bind() failed on " + opts_.bind_host + ":" +
                                 std::to_string(opts_.port));
    }
    if (listen(listen_fd_, 64) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("listen() failed");
    }

    running_.store(true);
    std::cerr << "[gateway] listening on http://" << opts_.bind_host << ':' << opts_.port
              << " (ws upgrade enabled) align=" << kAlign << " ver=" << kVersion << '\n';

    while (running_.load()) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        const int client = accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (client < 0) {
            if (!running_.load()) {
                break;
            }
            continue;
        }
        try {
            HandleClient(client);
        } catch (const std::exception& e) {
            if (opts_.verbose) {
                std::cerr << "[gateway] client error: " << e.what() << '\n';
            }
        }
        close(client);
    }

    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    running_.store(false);
}

void Server::HandleClient(int client_fd) {
    std::string req;
    char buf[8192];
    while (req.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return;
        }
        req.append(buf, static_cast<size_t>(n));
        if (req.size() > 1 << 20) {
            return;
        }
    }
    // If Content-Length, read remaining body
    const auto cl_hdr = HeaderValue(req, "Content-Length");
    if (!cl_hdr.empty()) {
        const size_t want = static_cast<size_t>(std::stoul(cl_hdr));
        auto body_pos = req.find("\r\n\r\n") + 4;
        while (req.size() < body_pos + want) {
            const ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            req.append(buf, static_cast<size_t>(n));
        }
    }

    const auto upgrade = HeaderValue(req, "Upgrade");
    std::string up_lower = upgrade;
    for (char& c : up_lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (up_lower.find("websocket") != std::string::npos) {
        HandleWebSocket(client_fd, req);
        return;
    }
    HandleHttp(client_fd, req);
}

void Server::HandleHttp(int client_fd, const std::string& req) {
    std::istringstream first(req);
    std::string method, path, version;
    first >> method >> path >> version;

    if (opts_.verbose) {
        std::cerr << "[gateway] " << method << ' ' << path << '\n';
    }

    auto send = [&](const std::string& resp) {
        (void)::send(client_fd, resp.data(), resp.size(), 0);
    };

    if (method == "GET" && (path == "/health" || path == "/healthz")) {
        send(HttpResponse(200, "OK", nlohmann::json{{"ok", true}, {"service", "openclaw-cpp-gateway"}}.dump()));
        return;
    }

    if (method == "GET" && (path == "/" || path == "/v1/status")) {
        nlohmann::json j{
            {"service", "openclaw-cpp"},
            {"version", kVersion},
            {"align_target", kAlign},
            {"gateway",
             {{"bind", opts_.bind_host},
              {"port", opts_.port},
              {"http", true},
              {"websocket", true}}},
            {"tools", registry_.Names()},
            {"sessions", sessions_.List()},
            {"channels", channels_.ListJson()},
            {"methods", AdvertisedMethods()},
            {"events", AdvertisedEvents()},
        };
        send(HttpResponse(200, "OK", j.dump(2)));
        return;
    }

    if (method == "GET" && path == "/v1/models") {
        nlohmann::json j{
            {"object", "list"},
            {"data",
             nlohmann::json::array({
                 {{"id", "deepseek-chat"}, {"object", "model"}, {"owned_by", "deepseek"}},
             })},
        };
        send(HttpResponse(200, "OK", j.dump()));
        return;
    }

    if (method == "POST" && path == "/tools/invoke") {
        try {
            const auto body = nlohmann::json::parse(BodyOf(req));
            const std::string name = body.value("tool", body.value("name", ""));
            nlohmann::json args = body.contains("args") ? body["args"] : body.value("arguments", nlohmann::json::object());
            if (name.empty()) {
                send(HttpResponse(400, "Bad Request",
                                  nlohmann::json{{"error", "missing tool name"}}.dump()));
                return;
            }
            const auto result = registry_.Invoke(name, args);
            send(HttpResponse(200, "OK",
                              nlohmann::json{{"ok", true}, {"tool", name}, {"result", nlohmann::json::parse(result)}}.dump()));
        } catch (const std::exception& e) {
            send(HttpResponse(400, "Bad Request", nlohmann::json{{"error", e.what()}}.dump()));
        }
        return;
    }

    if (method == "POST" && path == "/v1/chat/completions") {
        try {
            const auto body = nlohmann::json::parse(BodyOf(req));
            std::vector<agent::Message> messages;
            if (body.contains("messages") && body["messages"].is_array()) {
                for (const auto& m : body["messages"]) {
                    messages.push_back(agent::Message::FromJson(m));
                }
            }
            if (messages.empty()) {
                send(HttpResponse(400, "Bad Request",
                                  nlohmann::json{{"error", "messages required"}}.dump()));
                return;
            }

            const std::string session_id = body.value("session_id", "");
            if (!session_id.empty()) {
                auto prior = sessions_.Load(session_id);
                prior.insert(prior.end(), messages.begin(), messages.end());
                messages = std::move(prior);
            }

            agent::LoopOptions lopts;
            lopts.max_turns = body.value("max_turns", 8);
            auto result = agent::RunAgentLoop(provider_, registry_, messages, lopts);

            if (!session_id.empty() && result.ok) {
                sessions_.Save(session_id, result.messages);
            }

            if (!result.ok) {
                const int code = (result.error.find("402") != std::string::npos) ? 402 : 502;
                send(HttpResponse(code, code == 402 ? "Payment Required" : "Bad Gateway",
                                  nlohmann::json{{"error", {{"message", result.error}, {"type", "api_error"}}}}.dump()));
                return;
            }

            nlohmann::json resp{
                {"id", "chatcmpl-openclaw-cpp"},
                {"object", "chat.completion"},
                {"model", body.value("model", "deepseek-chat")},
                {"choices",
                 nlohmann::json::array({
                     {{"index", 0},
                      {"finish_reason", "stop"},
                      {"message", {{"role", "assistant"}, {"content", result.final_text}}}},
                 })},
                {"usage", {{"turns", result.turns}}},
            };
            send(HttpResponse(200, "OK", resp.dump()));
        } catch (const std::exception& e) {
            send(HttpResponse(400, "Bad Request", nlohmann::json{{"error", e.what()}}.dump()));
        }
        return;
    }

    if (method == "GET" && path == "/v1/methods") {
        send(HttpResponse(200, "OK", nlohmann::json{{"methods", AdvertisedMethods()}, {"events", AdvertisedEvents()}}.dump(2)));
        return;
    }

    if (method == "GET" && path == "/v1/channels") {
        send(HttpResponse(200, "OK", nlohmann::json{{"channels", channels_.ListJson()}}.dump(2)));
        return;
    }

    if (method == "GET" && path == "/v1/config") {
        send(HttpResponse(200, "OK", config_.ToJson().dump(2)));
        return;
    }

    send(HttpResponse(404, "Not Found",
                      nlohmann::json{{"error", "not found"}, {"path", path}}.dump()));
}

void Server::HandleWebSocket(int client_fd, const std::string& req) {
    const auto key = HeaderValue(req, "Sec-WebSocket-Key");
    if (key.empty()) {
        const auto resp = HttpResponse(400, "Bad Request",
                                       nlohmann::json{{"error", "missing Sec-WebSocket-Key"}}.dump());
        (void)::send(client_fd, resp.data(), resp.size(), 0);
        return;
    }
    const std::string accept = WsAccept(key);
    std::ostringstream hs;
    hs << "HTTP/1.1 101 Switching Protocols\r\n"
       << "Upgrade: websocket\r\n"
       << "Connection: Upgrade\r\n"
       << "Sec-WebSocket-Accept: " << accept << "\r\n"
       << "\r\n";
    const auto hs_s = hs.str();
    (void)::send(client_fd, hs_s.data(), hs_s.size(), 0);

    // Hello event (OpenClaw-ish greeting; full protocol TBD)
    const auto hello = nlohmann::json{
        {"type", "event"},
        {"event", "connect.challenge"},
        {"payload",
         {{"service", "openclaw-cpp"},
          {"version", kVersion},
          {"align_target", kAlign},
          {"note", "WS control protocol subset — expanding toward OpenClaw Gateway RPC"}}},
    }.dump();
    {
        const auto frame = WsFrameText(hello);
        (void)::send(client_fd, frame.data(), frame.size(), 0);
    }

    char buf[8192];
    while (running_.load()) {
        const ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        std::string raw(buf, static_cast<size_t>(n));
        // opcode close?
        if (!raw.empty() && (static_cast<uint8_t>(raw[0]) & 0x0f) == 0x8) {
            break;
        }
        std::string payload;
        if (!ParseWsClientFrame(raw, payload)) {
            continue;
        }
        nlohmann::json reply;
        try {
            const auto msg = nlohmann::json::parse(payload);
            reply = DispatchRpc(msg);
        } catch (...) {
            reply = {{"type", "res"}, {"ok", false}, {"error", "invalid JSON"}, {"echo", payload}};
        }
        const auto frame = WsFrameText(reply.dump());
        if (::send(client_fd, frame.data(), frame.size(), 0) <= 0) {
            break;
        }
    }
}


nlohmann::json Server::DispatchRpc(const nlohmann::json& msg) {
    const std::string method = msg.value("method", msg.value("type", ""));
    const auto params = msg.contains("params") ? msg["params"] : nlohmann::json::object();
    const auto id = msg.contains("id") ? msg["id"] : nlohmann::json();

    auto ok = [&](nlohmann::json payload) {
        nlohmann::json r{{"type", "res"}, {"ok", true}, {"payload", std::move(payload)}};
        if (!id.is_null()) {
            r["id"] = id;
        }
        return r;
    };
    auto err = [&](const std::string& e) {
        nlohmann::json r{{"type", "res"}, {"ok", false}, {"error", e}};
        if (!id.is_null()) {
            r["id"] = id;
        }
        return r;
    };

    if (method == "health" || method == "status" || method == "gateway.status") {
        return ok({
            {"runtime", "running"},
            {"version", kVersion},
            {"align_target", kAlign},
            {"tools", registry_.Names()},
            {"sessions", sessions_.List()},
            {"channels", channels_.ListJson()},
        });
    }
    if (method == "methods.list") {
        return ok({{"methods", AdvertisedMethods()}});
    }
    if (method == "events.list") {
        return ok({{"events", AdvertisedEvents()}});
    }
    if (method == "tools.list") {
        return ok({{"tools", registry_.Names()}});
    }
    if (method == "tools.invoke") {
        const std::string name = params.value("tool", params.value("name", ""));
        if (name.empty()) {
            return err("missing tool name");
        }
        const auto args = params.contains("args") ? params["args"] : params.value("arguments", nlohmann::json::object());
        try {
            const auto result = registry_.Invoke(name, args);
            return ok({{"tool", name}, {"result", nlohmann::json::parse(result)}});
        } catch (const std::exception& e) {
            return err(e.what());
        }
    }
    if (method == "sessions.list") {
        return ok({{"sessions", sessions_.List()}});
    }
    if (method == "sessions.get") {
        const std::string key = params.value("key", params.value("id", ""));
        if (key.empty()) {
            return err("missing session key");
        }
        nlohmann::json msgs = nlohmann::json::array();
        for (const auto& m : sessions_.Load(key)) {
            msgs.push_back(m.ToJson());
        }
        return ok({{"key", key}, {"messages", msgs}});
    }
    if (method == "sessions.delete") {
        const std::string key = params.value("key", params.value("id", ""));
        if (key.empty()) {
            return err("missing session key");
        }
        const bool removed = sessions_.Remove(key);
        return ok({{"key", key}, {"removed", removed}});
    }
    if (method == "config.get") {
        return ok(config_.ToJson());
    }
    if (method == "channels.status") {
        return ok({{"channels", channels_.ListJson()}});
    }
    if (method == "chat.send") {
        const std::string text = params.value("message", params.value("text", ""));
        const std::string session_key = params.value("sessionKey", params.value("session_id", "default"));
        if (text.empty()) {
            return err("missing message");
        }
        std::vector<agent::Message> messages = sessions_.Load(session_key);
        messages.push_back(agent::Message{.role = "user", .content = text});
        auto result = agent::RunAgentLoop(provider_, registry_, messages, {.max_turns = 8});
        if (!result.ok) {
            return err(result.error);
        }
        sessions_.Save(session_key, result.messages);
        return ok({{"sessionKey", session_key}, {"text", result.final_text}, {"turns", result.turns}});
    }

    return err("method not implemented yet: " + method);
}

}  // namespace openclaw::gateway
