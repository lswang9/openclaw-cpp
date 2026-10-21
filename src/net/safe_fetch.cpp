#include "openclaw/net/safe_fetch.hpp"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netdb.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace openclaw::net {

bool IsPrivateAddress(const std::string& ip) {
    in_addr a4{};
    if (inet_pton(AF_INET, ip.c_str(), &a4) == 1) {
        std::uint32_t v = ntohl(a4.s_addr);
        auto in = [&](std::uint32_t net, int bits) { return (v >> (32 - bits)) == (net >> (32 - bits)); };
        return in(0x00000000, 8) || in(0x0A000000, 8) || in(0x7F000000, 8) || in(0xA9FE0000, 16) || in(0xAC100000, 12) ||
               in(0xC0A80000, 16) || in(0x64400000, 10) || in(0xC0000000, 24) || in(0xC6120000, 15) || v >= 0xE0000000;
    }
    in6_addr a6{};
    if (inet_pton(AF_INET6, ip.c_str(), &a6) == 1) {
        const auto* b = a6.s6_addr;
        static const std::uint8_t zero[16] = {};
        if (std::memcmp(b, zero, 15) == 0 && (b[15] == 0 || b[15] == 1)) return true;  // :: and ::1
        if ((b[0] & 0xfe) == 0xfc) return true;                                        // fc00::/7
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;                        // fe80::/10
        if (b[0] == 0xff) return true;                                                 // multicast
        static const std::uint8_t v4map[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (std::memcmp(b, v4map, 12) == 0) {
            char s[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, b + 12, s, sizeof s);
            return IsPrivateAddress(s);
        }
        return false;
    }
    return true;  // unparsable → treat as unsafe
}

namespace {
struct Sink { std::string* out; std::size_t max; bool truncated = false; };
size_t Write(char* p, size_t s, size_t n, void* ud) {
    auto* k = static_cast<Sink*>(ud);
    if (k->out->size() + s * n > k->max) { k->out->append(p, k->max - k->out->size()); k->truncated = true; return 0; }
    k->out->append(p, s * n);
    return s * n;
}
}  // namespace

FetchResult SafeFetch(const std::string& url_in, std::size_t max_bytes, bool allow_private, int timeout_sec) {
    FetchResult r;
    std::string url = url_in;
    for (int hop = 0; hop < 4; ++hop) {
        CURLU* u = curl_url();
        if (curl_url_set(u, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) { curl_url_cleanup(u); r.error = "invalid URL"; return r; }
        char *scheme = nullptr, *host = nullptr, *port = nullptr;
        curl_url_get(u, CURLUPART_SCHEME, &scheme, 0);
        curl_url_get(u, CURLUPART_HOST, &host, 0);
        curl_url_get(u, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT);
        std::string sch = scheme ? scheme : "", h = host ? host : "", pt = port ? port : "";
        curl_free(scheme); curl_free(host); curl_free(port); curl_url_cleanup(u);
        if (sch != "http" && sch != "https") { r.error = "only http/https URLs allowed"; return r; }
        if (h.size() > 2 && h.front() == '[') h = h.substr(1, h.size() - 2);
        addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(h.c_str(), nullptr, &hints, &res) != 0 || !res) { r.error = "cannot resolve host " + h; return r; }
        std::string pinned;
        bool blocked = false;
        for (auto* ai = res; ai; ai = ai->ai_next) {
            char s[INET6_ADDRSTRLEN] = {};
            if (ai->ai_family == AF_INET) inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr, s, sizeof s);
            else if (ai->ai_family == AF_INET6) inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(ai->ai_addr)->sin6_addr, s, sizeof s);
            else continue;
            if (IsPrivateAddress(s)) blocked = true;
            if (pinned.empty()) pinned = s;
        }
        freeaddrinfo(res);
        if (blocked && !allow_private) { r.error = "private/loopback address blocked: " + h; return r; }
        CURL* c = curl_easy_init();
        std::string body;
        Sink sink{&body, max_bytes};
        std::string resolve = h + ":" + pt + ":" + (pinned.find(':') != std::string::npos ? "[" + pinned + "]" : pinned);
        curl_slist* rs = curl_slist_append(nullptr, resolve.c_str());
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_RESOLVE, rs);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "openclaw-cpp-fetch/1");
        curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
        auto rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        char* ct = nullptr;
        curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
        r.content_type = ct ? ct : "";
        char* loc = nullptr;
        curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &loc);
        std::string next = loc ? loc : "";
        curl_slist_free_all(rs);
        curl_easy_cleanup(c);
        if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && sink.truncated)) { r.error = curl_easy_strerror(rc); return r; }
        if (code >= 300 && code < 400 && !next.empty()) { url = next; continue; }
        r.ok = code >= 200 && code < 300;
        r.status = code;
        r.body = std::move(body);
        r.truncated = sink.truncated;
        r.final_url = url;
        if (!r.ok) r.error = "HTTP " + std::to_string(code);
        return r;
    }
    r.error = "too many redirects";
    return r;
}

}  // namespace openclaw::net
