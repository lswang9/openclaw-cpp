#include "openclaw/browser/cdp_client.hpp"
#include "openclaw/util.hpp"

#include <curl/curl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace openclaw::browser {
namespace {

size_t CurlWrite(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n);
    return s * n;
}

std::string HttpGet(const std::string& url, long timeout_s = 5) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "openclaw-cpp-cdp/1.6");
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) return {};
    return body;
}

std::string HttpPut(const std::string& url, long timeout_s = 5) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "openclaw-cpp-cdp/1.6");
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) return {};
    return body;
}

std::string ExtractWsUrl(const std::string& body) {
    if (body.empty()) return {};
    try {
        auto j = nlohmann::json::parse(body);
        if (j.is_object() && j.contains("webSocketDebuggerUrl"))
            return j["webSocketDebuggerUrl"].get<std::string>();
        if (j.is_array()) {
            for (auto& t : j) {
                if (t.value("type", "") == "page" && t.contains("webSocketDebuggerUrl"))
                    return t["webSocketDebuggerUrl"].get<std::string>();
            }
            for (auto& t : j) {
                if (t.contains("webSocketDebuggerUrl"))
                    return t["webSocketDebuggerUrl"].get<std::string>();
            }
        }
    } catch (...) {
    }
    return {};
}

std::string B64Encode(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(T[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(T[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// Minimal SHA1 for WS accept (same as gateway).
namespace sha1 {
uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
std::array<uint8_t, 20> hash(const std::string& msg) {
    uint64_t bitlen = msg.size() * 8;
    std::string data = msg;
    data.push_back(static_cast<char>(0x80));
    while ((data.size() % 64) != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<char>((bitlen >> (i * 8)) & 0xff));
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint8_t>(data[chunk + i * 4]) << 24) |
                   (static_cast<uint8_t>(data[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint8_t>(data[chunk + i * 4 + 2]) << 8) |
                   static_cast<uint8_t>(data[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
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
            uint32_t temp = rol(a, 5) + f + e + k + w[i];
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
    std::array<uint8_t, 20> out{};
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = (hs[i] >> 24) & 0xff;
        out[i * 4 + 1] = (hs[i] >> 16) & 0xff;
        out[i * 4 + 2] = (hs[i] >> 8) & 0xff;
        out[i * 4 + 3] = hs[i] & 0xff;
    }
    return out;
}
}  // namespace sha1

std::string WsAcceptKey(const std::string& key) {
    auto dig = sha1::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return B64Encode(std::string(reinterpret_cast<const char*>(dig.data()), dig.size()));
}

bool ParseHostPortPath(const std::string& url, std::string& host, int& port, std::string& path) {
    // ws://host:port/path or http://host:port/path
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    auto rest = url.substr(scheme_end + 3);
    auto slash = rest.find('/');
    std::string hp = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    auto colon = hp.rfind(':');
    if (colon != std::string::npos && hp.find(']') == std::string::npos) {
        host = hp.substr(0, colon);
        port = std::atoi(hp.substr(colon + 1).c_str());
    } else {
        host = hp;
        port = (url.rfind("https://", 0) == 0 || url.rfind("wss://", 0) == 0) ? 443 : 80;
    }
    if (host.empty() || port <= 0) return false;
    return true;
}

std::string MinimalPng() {
    static const unsigned char k[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
        0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
        0x42, 0x60, 0x82};
    return std::string(reinterpret_cast<const char*>(k), sizeof(k));
}

std::string B64Decode(const std::string& in) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1};
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

}  // namespace

std::string WsClientMaskFrame(const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    const auto len = payload.size();
    uint8_t mask_bit = 0x80;
    if (len < 126) {
        frame.push_back(static_cast<char>(mask_bit | static_cast<uint8_t>(len)));
    } else if (len <= 0xffff) {
        frame.push_back(static_cast<char>(mask_bit | 126));
        frame.push_back(static_cast<char>((len >> 8) & 0xff));
        frame.push_back(static_cast<char>(len & 0xff));
    } else {
        frame.push_back(static_cast<char>(mask_bit | 127));
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((len >> (i * 8)) & 0xff));
    }
    std::array<uint8_t, 4> mask{};
    std::random_device rd;
    for (int i = 0; i < 4; ++i) mask[i] = static_cast<uint8_t>(rd());
    for (int i = 0; i < 4; ++i) frame.push_back(static_cast<char>(mask[i]));
    for (size_t i = 0; i < payload.size(); ++i)
        frame.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]));
    return frame;
}

bool WsParseServerFrame(const std::string& buf, size_t& consumed, std::string& out_payload, uint8_t& opcode) {
    consumed = 0;
    if (buf.size() < 2) return false;
    opcode = static_cast<uint8_t>(buf[0]) & 0x0f;
    const auto b1 = static_cast<uint8_t>(buf[1]);
    const bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7f;
    size_t off = 2;
    if (len == 126) {
        if (buf.size() < 4) return false;
        len = (static_cast<uint8_t>(buf[2]) << 8) | static_cast<uint8_t>(buf[3]);
        off = 4;
    } else if (len == 127) {
        if (buf.size() < 10) return false;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(buf[2 + i]);
        off = 10;
        if (len > 32ull * 1024 * 1024) return false;  // sanity
    }
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
    consumed = off + static_cast<size_t>(len);
    return true;
}

struct CdpClient::Impl {
    CdpConfig cfg;
    mutable std::mutex mu;
    int fd = -1;
    std::string recv_buf;
    int next_id = 1;
    bool connected = false;
    bool mock = false;
    std::string ws_url;
    std::string last_url;
    std::string last_title;
    std::string mock_html = "<html><head><title>mock-cdp</title></head><body>ok</body></html>";

    // mock state
    std::string mock_eval_last;

    ~Impl() { CloseFd(); }

    void CloseFd() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        connected = false;
        recv_buf.clear();
    }

    bool SetDeadline(std::chrono::milliseconds ms) {
        if (fd < 0) return false;
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(ms.count() / 1000);
        tv.tv_usec = static_cast<suseconds_t>((ms.count() % 1000) * 1000);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        return true;
    }

    bool ConnectTcp(const std::string& host, int port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        std::string port_s = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0) return false;
        int s = -1;
        for (auto* p = res; p; p = p->ai_next) {
            s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (s < 0) continue;
            if (::connect(s, p->ai_addr, p->ai_addrlen) == 0) break;
            ::close(s);
            s = -1;
        }
        freeaddrinfo(res);
        if (s < 0) return false;
        fd = s;
        SetDeadline(cfg.timeout);
        return true;
    }

    bool WsHandshake(const std::string& host, int port, const std::string& path) {
        std::string key_raw(16, '\0');
        std::random_device rd;
        for (auto& c : key_raw) c = static_cast<char>(rd());
        std::string key = B64Encode(key_raw);
        std::ostringstream req;
        req << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << host << ":" << port << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n"
            << "\r\n";
        auto req_s = req.str();
        if (::send(fd, req_s.data(), req_s.size(), 0) <= 0) return false;
        std::string resp;
        char buf[4096];
        while (resp.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            resp.append(buf, static_cast<size_t>(n));
            if (resp.size() > 65536) return false;
        }
        if (resp.find("101") == std::string::npos) return false;
        auto expect = WsAcceptKey(key);
        if (resp.find(expect) == std::string::npos) return false;
        auto hdr_end = resp.find("\r\n\r\n");
        if (hdr_end + 4 < resp.size()) recv_buf = resp.substr(hdr_end + 4);
        return true;
    }

    bool SendText(const std::string& text) {
        auto frame = WsClientMaskFrame(text);
        size_t off = 0;
        while (off < frame.size()) {
            ssize_t n = ::send(fd, frame.data() + off, frame.size() - off, 0);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool RecvMessage(std::string& out, std::chrono::steady_clock::time_point deadline) {
        out.clear();
        std::string assembled;
        while (std::chrono::steady_clock::now() < deadline) {
            while (true) {
                size_t consumed = 0;
                std::string payload;
                uint8_t opcode = 0;
                if (!WsParseServerFrame(recv_buf, consumed, payload, opcode)) break;
                recv_buf.erase(0, consumed);
                if (opcode == 0x8) return false;  // close
                if (opcode == 0x9) {  // ping → pong
                    // build masked pong
                    std::string pong;
                    pong.push_back(static_cast<char>(0x8A));
                    // reuse mask frame builder for text only — send simple pong
                    auto frame = WsClientMaskFrame(payload);
                    frame[0] = static_cast<char>(0x8A);
                    (void)::send(fd, frame.data(), frame.size(), 0);
                    continue;
                }
                if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
                    assembled += payload;
                    // FIN bit is in first byte of frame — WsParseServerFrame doesn't return FIN.
                    // CDP sends complete text frames with FIN. Treat each text frame as a message.
                    if (opcode == 0x1 || opcode == 0x2) {
                        out = assembled;
                        return true;
                    }
                }
            }
            char buf[8192];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                // timeout or error — if we have partial, keep waiting until deadline
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                return false;
            }
            recv_buf.append(buf, static_cast<size_t>(n));
        }
        return false;
    }

    nlohmann::json MockCall(const std::string& method, const nlohmann::json& params) {
        if (method == "Page.navigate") {
            last_url = params.value("url", "about:blank");
            last_title = "mock-cdp";
            if (last_url.find("example.com") != std::string::npos) last_title = "Example Domain";
            return {{"id", 0}, {"result", {{"frameId", "mock-frame"}}}};
        }
        if (method == "Page.enable" || method == "Runtime.enable" || method == "Page.bringToFront") {
            return {{"id", 0}, {"result", nlohmann::json::object()}};
        }
        if (method == "Runtime.evaluate") {
            auto expr = params.value("expression", "");
            mock_eval_last = expr;
            nlohmann::json value;
            if (expr.find("document.title") != std::string::npos) value = last_title;
            else if (expr.find("location.href") != std::string::npos) value = last_url;
            else if (expr.find("document.documentElement.outerHTML") != std::string::npos)
                value = mock_html;
            else value = "mock:" + expr.substr(0, 80);
            return {{"id", 0},
                    {"result",
                     {{"result", {{"type", "string"}, {"value", value}}}}}};
        }
        if (method == "Page.captureScreenshot") {
            auto png = MinimalPng();
            return {{"id", 0}, {"result", {{"data", B64Encode(png)}}}};
        }
        return {{"id", 0}, {"result", nlohmann::json::object()}, {"mock", true}};
    }

    nlohmann::json CallLocked(const std::string& method, const nlohmann::json& params) {
        if (mock) return MockCall(method, params);
        if (!connected || fd < 0) return {{"ok", false}, {"error", "not connected"}};
        const int id = next_id++;
        nlohmann::json msg{{"id", id}, {"method", method}, {"params", params}};
        if (!SendText(msg.dump())) {
            CloseFd();
            return {{"ok", false}, {"error", "ws send failed"}};
        }
        auto deadline = std::chrono::steady_clock::now() + cfg.timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::string raw;
            if (!RecvMessage(raw, deadline)) break;
            try {
                auto j = nlohmann::json::parse(raw);
                if (j.contains("id") && j["id"] == id) return j;
                // event — ignore (could observe Page.loadEventFired)
                if (j.value("method", "") == "Page.loadEventFired" && method == "Page.navigate") {
                    // keep waiting for command result if not yet; result usually comes first
                }
            } catch (...) {
            }
        }
        return {{"ok", false}, {"error", "cdp timeout"}, {"method", method}};
    }

    std::string DiscoverWsUrl() {
        std::string ep = cfg.endpoint.empty() ? EnvOr("OPENCLAW_BROWSER_CDP_URL", "http://127.0.0.1:9222")
                                              : cfg.endpoint;
        if (ep.rfind("ws://", 0) == 0 || ep.rfind("wss://", 0) == 0) return ep;
        std::string base = ResolveHttpBase(ep);
        // Chrome 155+: /json/new requires PUT (GET returns an error string).
        if (auto ws = ExtractWsUrl(HttpPut(base + "/json/new?about:blank", 3)); !ws.empty()) return ws;
        if (auto ws = ExtractWsUrl(HttpGet(base + "/json/list", 3)); !ws.empty()) return ws;
        if (auto ws = ExtractWsUrl(HttpGet(base + "/json/version", 3)); !ws.empty()) return ws;
        return {};
    }
};

CdpConfig CdpClient::FromEnv() {
    CdpConfig c;
    c.endpoint = EnvOr("OPENCLAW_BROWSER_CDP_URL", "http://127.0.0.1:9222");
    c.mock = MockEnabled();
    return c;
}

bool CdpClient::MockEnabled() {
    const char* v = std::getenv("OPENCLAW_BROWSER_CDP_MOCK");
    return v && (std::string(v) == "1" || std::string(v) == "true" || std::string(v) == "yes");
}

std::string CdpClient::ResolveHttpBase(const std::string& endpoint) {
    if (endpoint.rfind("http://", 0) == 0 || endpoint.rfind("https://", 0) == 0) {
        // strip trailing slash
        auto e = endpoint;
        while (!e.empty() && e.back() == '/') e.pop_back();
        return e;
    }
    if (endpoint.rfind("ws://", 0) == 0) {
        auto rest = endpoint.substr(5);
        auto slash = rest.find('/');
        return "http://" + (slash == std::string::npos ? rest : rest.substr(0, slash));
    }
    return "http://" + endpoint;
}

CdpClient::CdpClient(CdpConfig cfg) : impl_(std::make_unique<Impl>()) {
    if (cfg.endpoint.empty() && !cfg.mock) cfg = FromEnv();
    if (MockEnabled()) cfg.mock = true;
    impl_->cfg = std::move(cfg);
    impl_->mock = impl_->cfg.mock;
}

CdpClient::~CdpClient() = default;

bool CdpClient::Connected() const {
    std::lock_guard lock(impl_->mu);
    return impl_->connected || impl_->mock;
}

void CdpClient::Disconnect() {
    std::lock_guard lock(impl_->mu);
    impl_->CloseFd();
    if (impl_->mock) impl_->connected = false;
}

nlohmann::json CdpClient::Status() const {
    std::lock_guard lock(impl_->mu);
    return {{"engine", impl_->mock ? "cdp-mock" : "cdp"},
            {"connected", impl_->connected || (impl_->mock && impl_->connected)},
            {"mock", impl_->mock},
            {"endpoint", impl_->cfg.endpoint},
            {"ws_url", impl_->ws_url},
            {"last_url", impl_->last_url},
            {"last_title", impl_->last_title},
            {"impl", impl_->mock ? "real-sim" : "real"}};
}

nlohmann::json CdpClient::Connect() {
    std::lock_guard lock(impl_->mu);
    if (MockEnabled()) {
        impl_->mock = true;
        impl_->cfg.mock = true;
    }
    if (impl_->mock) {
        impl_->connected = true;
        impl_->ws_url = "mock://cdp";
        return {{"ok", true}, {"mock", true}, {"ws_url", impl_->ws_url}, {"impl", "real-sim"}};
    }
    if (impl_->connected && impl_->fd >= 0) return {{"ok", true}, {"ws_url", impl_->ws_url}, {"reused", true}};
    impl_->CloseFd();
    auto ws = impl_->DiscoverWsUrl();
    if (ws.empty()) {
        return {{"ok", false},
                {"error", "cdp discover failed"},
                {"hint", "start Chrome: google-chrome --headless=new --remote-debugging-port=9222 "
                         "--user-data-dir=/tmp/oc-chrome-cdp"},
                {"endpoint", impl_->cfg.endpoint}};
    }
    std::string host;
    int port = 0;
    std::string path;
    if (!ParseHostPortPath(ws, host, port, path)) {
        return {{"ok", false}, {"error", "bad ws url"}, {"ws_url", ws}};
    }
    if (!impl_->ConnectTcp(host, port)) {
        return {{"ok", false}, {"error", "tcp connect failed"}, {"host", host}, {"port", port}};
    }
    if (!impl_->WsHandshake(host, port, path)) {
        impl_->CloseFd();
        return {{"ok", false}, {"error", "ws handshake failed"}, {"ws_url", ws}};
    }
    impl_->ws_url = ws;
    impl_->connected = true;
    // enable domains
    (void)impl_->CallLocked("Page.enable", {});
    (void)impl_->CallLocked("Runtime.enable", {});
    return {{"ok", true}, {"ws_url", ws}, {"impl", "real"}, {"engine", "cdp"}};
}

nlohmann::json CdpClient::Call(const std::string& method, const nlohmann::json& params) {
    std::lock_guard lock(impl_->mu);
    if (!impl_->connected && !impl_->mock) {
        return {{"ok", false}, {"error", "not connected"}};
    }
    if (impl_->mock && !impl_->connected) impl_->connected = true;
    return impl_->CallLocked(method, params);
}

nlohmann::json CdpClient::Navigate(const std::string& url) {
    auto c = Connect();
    if (!c.value("ok", false)) return c;
    std::lock_guard lock(impl_->mu);
    auto r = impl_->CallLocked("Page.navigate", {{"url", url}});
    if (r.contains("error") && !r.contains("result")) {
        return {{"ok", false}, {"error", r}, {"url", url}};
    }
    impl_->last_url = url;
    // brief wait for load in live mode
    if (!impl_->mock) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (std::chrono::steady_clock::now() < deadline) {
            std::string raw;
            auto slice = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(200));
            if (!impl_->RecvMessage(raw, slice)) break;
            try {
                auto j = nlohmann::json::parse(raw);
                if (j.value("method", "") == "Page.loadEventFired") break;
            } catch (...) {
            }
        }
    }
    // title
    auto title_r = impl_->CallLocked("Runtime.evaluate",
                                     {{"expression", "document.title"}, {"returnByValue", true}});
    std::string title;
    try {
        title = title_r.at("result").at("result").at("value").get<std::string>();
    } catch (...) {
    }
    impl_->last_title = title;
    auto href_r = impl_->CallLocked("Runtime.evaluate",
                                    {{"expression", "location.href"}, {"returnByValue", true}});
    std::string href = url;
    try {
        href = href_r.at("result").at("result").at("value").get<std::string>();
        impl_->last_url = href;
    } catch (...) {
    }
    return {{"ok", true},
            {"impl", impl_->mock ? "real-sim" : "real"},
            {"engine", impl_->mock ? "cdp-mock" : "cdp"},
            {"url", href},
            {"title", title},
            {"navigate", r.value("result", nlohmann::json::object())}};
}

nlohmann::json CdpClient::Evaluate(const std::string& expression) {
    auto c = Connect();
    if (!c.value("ok", false)) return c;
    std::lock_guard lock(impl_->mu);
    auto r = impl_->CallLocked("Runtime.evaluate",
                               {{"expression", expression}, {"returnByValue", true}, {"awaitPromise", true}});
    if (!r.contains("result")) return {{"ok", false}, {"error", r}, {"expression", expression}};
    nlohmann::json value = nullptr;
    try {
        auto rr = r.at("result").at("result");
        if (rr.contains("value")) value = rr["value"];
        else if (rr.contains("description")) value = rr["description"];
        else value = rr;
    } catch (...) {
        value = r["result"];
    }
    return {{"ok", true},
            {"impl", impl_->mock ? "real-sim" : "real"},
            {"engine", impl_->mock ? "cdp-mock" : "cdp"},
            {"expression", expression},
            {"value", value}};
}

nlohmann::json CdpClient::ScreenshotPng(const std::filesystem::path& out_path) {
    auto c = Connect();
    if (!c.value("ok", false)) return c;
    std::lock_guard lock(impl_->mu);
    auto r = impl_->CallLocked("Page.captureScreenshot", {{"format", "png"}, {"fromSurface", true}});
    if (!r.contains("result") || !r["result"].contains("data")) {
        return {{"ok", false}, {"error", r}};
    }
    std::string b64 = r["result"]["data"].get<std::string>();
    auto png = B64Decode(b64);
    std::filesystem::path path = out_path;
    if (path.empty()) {
        path = std::filesystem::temp_directory_path() /
               ("openclaw-cdp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                ".png");
    }
    {
        std::ofstream out(path, std::ios::binary);
        out.write(png.data(), static_cast<std::streamsize>(png.size()));
    }
    return {{"ok", true},
            {"impl", impl_->mock ? "real-sim" : "real"},
            {"engine", impl_->mock ? "cdp-mock" : "cdp"},
            {"path", path.string()},
            {"bytes", png.size()},
            {"content_type", "image/png"}};
}

nlohmann::json CdpClient::TitleAndUrl() {
    auto c = Connect();
    if (!c.value("ok", false)) return c;
    std::lock_guard lock(impl_->mu);
    auto t = impl_->CallLocked("Runtime.evaluate", {{"expression", "document.title"}, {"returnByValue", true}});
    auto u = impl_->CallLocked("Runtime.evaluate", {{"expression", "location.href"}, {"returnByValue", true}});
    std::string title, url;
    try {
        title = t.at("result").at("result").at("value").get<std::string>();
    } catch (...) {
    }
    try {
        url = u.at("result").at("result").at("value").get<std::string>();
    } catch (...) {
    }
    impl_->last_title = title;
    if (!url.empty()) impl_->last_url = url;
    return {{"ok", true},
            {"impl", impl_->mock ? "real-sim" : "real"},
            {"engine", impl_->mock ? "cdp-mock" : "cdp"},
            {"title", title},
            {"url", url}};
}

}  // namespace openclaw::browser
