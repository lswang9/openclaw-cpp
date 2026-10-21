#include "openclaw/channels/discord_voice.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace openclaw::channels::discord_voice {
namespace {
void Be16(Bytes& o, std::uint16_t v) { o.push_back(v >> 8); o.push_back(v & 0xff); }
void Be32(Bytes& o, std::uint32_t v) { for (int s = 24; s >= 0; s -= 8) o.push_back((v >> s) & 0xff); }
std::uint32_t Rd32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }
std::uint32_t Rotl(std::uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }
void Qr(std::uint32_t* x, int a, int b, int c, int d) {
    x[a] += x[b]; x[d] = Rotl(x[d] ^ x[a], 16); x[c] += x[d]; x[b] = Rotl(x[b] ^ x[c], 12);
    x[a] += x[b]; x[d] = Rotl(x[d] ^ x[a], 8);  x[c] += x[d]; x[b] = Rotl(x[b] ^ x[c], 7);
}
std::uint32_t Le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
// RTP header length incl. CSRCs + extension header (rtpsize: extension body is encrypted, header 4 bytes is AAD).
std::size_t AadLen(const Bytes& p) {
    if (p.size() < 12) return 0;
    std::size_t n = 12 + 4 * (p[0] & 0x0f);
    if ((p[0] & 0x10) && p.size() >= n + 4) n += 4;
    return n;
}
bool Aead(bool enc, const EVP_CIPHER* c, const Bytes& key, const Bytes& nonce, const Bytes& aad, const Bytes& in, Bytes& out) {
    EVP_CIPHER_CTX* x = EVP_CIPHER_CTX_new();
    int l = 0, f = 0;
    bool ok = EVP_CipherInit_ex(x, c, nullptr, nullptr, nullptr, enc) == 1 &&
              EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) == 1 &&
              EVP_CipherInit_ex(x, nullptr, nullptr, key.data(), nonce.data(), enc) == 1 &&
              EVP_CipherUpdate(x, nullptr, &l, aad.data(), static_cast<int>(aad.size())) == 1;
    if (!ok) { EVP_CIPHER_CTX_free(x); return false; }
    Bytes data = in, tag;
    if (!enc) { if (data.size() < 16) { EVP_CIPHER_CTX_free(x); return false; } tag.assign(data.end() - 16, data.end()); data.resize(data.size() - 16); }
    out.assign(data.size() + 16, 0);
    ok = EVP_CipherUpdate(x, out.data(), &l, data.data(), static_cast<int>(data.size())) == 1;
    if (ok && !enc) ok = EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_SET_TAG, 16, tag.data()) == 1;
    ok = ok && EVP_CipherFinal_ex(x, out.data() + l, &f) == 1;
    out.resize(static_cast<std::size_t>(l + f));
    if (ok && enc) { Bytes t(16); ok = EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_GET_TAG, 16, t.data()) == 1; out.insert(out.end(), t.begin(), t.end()); }
    EVP_CIPHER_CTX_free(x);
    return ok;
}
std::optional<Bytes> Crypt(bool enc, const std::string& mode, const Bytes& key, const Bytes& aad, const Bytes& in, std::uint32_t ctr) {
    if (key.size() != 32) return std::nullopt;
    Bytes out;
    if (mode == "aead_aes256_gcm_rtpsize") {
        Bytes nonce(12, 0); nonce[0] = ctr >> 24; nonce[1] = ctr >> 16; nonce[2] = ctr >> 8; nonce[3] = ctr;
        if (!Aead(enc, EVP_aes_256_gcm(), key, nonce, aad, in, out)) return std::nullopt;
        return out;
    }
    if (mode == "aead_xchacha20_poly1305_rtpsize") {
        Bytes n24(24, 0); n24[0] = ctr >> 24; n24[1] = ctr >> 16; n24[2] = ctr >> 8; n24[3] = ctr;
        auto sub = HChaCha20(key, Bytes(n24.begin(), n24.begin() + 16));
        Bytes n12(12, 0); std::copy(n24.begin() + 16, n24.end(), n12.begin() + 4);
        if (!Aead(enc, EVP_chacha20_poly1305(), sub, n12, aad, in, out)) return std::nullopt;
        return out;
    }
    return std::nullopt;
}
}  // namespace

Bytes HChaCha20(const Bytes& k, const Bytes& n) {
    std::uint32_t x[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) x[4 + i] = Le32(&k[4 * i]);
    for (int i = 0; i < 4; ++i) x[12 + i] = Le32(&n[4 * i]);
    for (int r = 0; r < 10; ++r) {
        Qr(x, 0, 4, 8, 12); Qr(x, 1, 5, 9, 13); Qr(x, 2, 6, 10, 14); Qr(x, 3, 7, 11, 15);
        Qr(x, 0, 5, 10, 15); Qr(x, 1, 6, 11, 12); Qr(x, 2, 7, 8, 13); Qr(x, 3, 4, 9, 14);
    }
    Bytes o;
    for (int i : {0, 1, 2, 3, 12, 13, 14, 15}) for (int b = 0; b < 4; ++b) o.push_back((x[i] >> (8 * b)) & 0xff);
    return o;
}

Bytes IpDiscoveryRequest(std::uint32_t ssrc) {
    Bytes o; Be16(o, 1); Be16(o, 70); Be32(o, ssrc);
    o.resize(74, 0);
    return o;
}

std::optional<Discovered> ParseIpDiscoveryResponse(const Bytes& b) {
    if (b.size() < 74 || b[0] != 0 || b[1] != 2) return std::nullopt;
    Discovered d;
    d.ip.assign(reinterpret_cast<const char*>(&b[8]), strnlen(reinterpret_cast<const char*>(&b[8]), 64));
    d.port = static_cast<std::uint16_t>((b[72] << 8) | b[73]);
    return d;
}

Bytes RtpHeader(std::uint16_t seq, std::uint32_t ts, std::uint32_t ssrc) {
    Bytes h{0x80, 0x78}; Be16(h, seq); Be32(h, ts); Be32(h, ssrc);
    return h;
}

std::optional<Bytes> EncryptRtp(const std::string& mode, const Bytes& key, const Bytes& header, const Bytes& opus, std::uint32_t ctr) {
    auto ct = Crypt(true, mode, key, header, opus, ctr);
    if (!ct) return std::nullopt;
    Bytes p = header; p.insert(p.end(), ct->begin(), ct->end()); Be32(p, ctr);
    return p;
}

std::optional<Bytes> DecryptRtp(const std::string& mode, const Bytes& key, const Bytes& p) {
    const auto al = AadLen(p);
    if (!al || p.size() < al + 16 + 4) return std::nullopt;
    const std::uint32_t ctr = Rd32(&p[p.size() - 4]);
    Bytes aad(p.begin(), p.begin() + al), ct(p.begin() + al, p.end() - 4);
    return Crypt(false, mode, key, aad, ct, ctr);
}

std::vector<Bytes> OggOpusPackets(const Bytes& b, std::string* err) {
    std::vector<Bytes> out;
    Bytes cur;
    std::size_t i = 0;
    int header_pkts = 0;
    while (i + 27 <= b.size()) {
        if (std::memcmp(&b[i], "OggS", 4) != 0) { if (err) *err = "bad capture pattern"; return {}; }
        const int nseg = b[i + 26];
        if (i + 27 + nseg > b.size()) { if (err) *err = "truncated page"; return {}; }
        const std::uint8_t* lace = &b[i + 27];
        std::size_t data = i + 27 + nseg;
        for (int s = 0; s < nseg; ++s) {
            if (data + lace[s] > b.size()) { if (err) *err = "truncated segment"; return {}; }
            cur.insert(cur.end(), b.begin() + data, b.begin() + data + lace[s]);
            data += lace[s];
            if (lace[s] < 255) {
                const bool is_head = cur.size() >= 8 && (std::memcmp(cur.data(), "OpusHead", 8) == 0 || std::memcmp(cur.data(), "OpusTags", 8) == 0);
                if (is_head) ++header_pkts; else if (!cur.empty()) out.push_back(cur);
                cur.clear();
            }
        }
        i = data;
    }
    if (header_pkts == 0) { if (err) *err = "not an Ogg Opus stream"; return {}; }
    return out;
}

nlohmann::json Play(const PlayRequest& rq) {
    nlohmann::json rep{{"ok", false}};
    CURL* c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, rq.endpoint.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    if (auto rc = curl_easy_perform(c); rc != CURLE_OK) { curl_easy_cleanup(c); rep["error"] = std::string("voice ws connect failed: ") + curl_easy_strerror(rc); return rep; }
    auto send = [&](const nlohmann::json& j) { auto s = j.dump(); size_t n = 0; curl_ws_send(c, s.data(), s.size(), &n, 0, CURLWS_TEXT); };
    std::string acc;
    int seq_ack = -1;
    auto recv_op = [&](int want, int timeout_ms) -> std::optional<nlohmann::json> {
        const auto t0 = std::chrono::steady_clock::now();
        char buf[65536];
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(timeout_ms)) {
            size_t n = 0; const curl_ws_frame* m = nullptr;
            auto r = curl_ws_recv(c, buf, sizeof buf, &n, &m);
            if (r == CURLE_AGAIN) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            if (r != CURLE_OK) return std::nullopt;
            acc.append(buf, n);
            if (m && m->bytesleft > 0) continue;
            nlohmann::json j;
            try { j = nlohmann::json::parse(acc); } catch (...) { acc.clear(); continue; }
            acc.clear();
            if (j.contains("seq") && j["seq"].is_number()) seq_ack = j["seq"];
            if (j.value("op", -1) == want) return j;
        }
        return std::nullopt;
    };
    auto fail = [&](const std::string& e) { rep["error"] = e; curl_easy_cleanup(c); return rep; };
    auto hello = recv_op(8, 5000);
    if (!hello) return fail("no Hello (op 8)");
    rep["heartbeat_interval"] = (*hello)["d"].value("heartbeat_interval", 0.0);
    send({{"op", 0}, {"d", {{"server_id", rq.server_id}, {"user_id", rq.user_id}, {"session_id", rq.session_id}, {"token", rq.token}, {"max_dave_protocol_version", 0}}}});
    auto ready = recv_op(2, 5000);
    if (!ready) return fail("no Ready (op 2)");
    const auto& rd = (*ready)["d"];
    const std::uint32_t ssrc = rd.value("ssrc", 0u);
    const auto ip = rd.value("ip", std::string());
    const int port = rd.value("port", 0);
    std::string mode;
    for (auto& m : rd.value("modes", nlohmann::json::array())) if (m == "aead_aes256_gcm_rtpsize") mode = m;
    if (mode.empty()) for (auto& m : rd.value("modes", nlohmann::json::array())) if (m == "aead_xchacha20_poly1305_rtpsize") mode = m;
    if (mode.empty()) return fail("no supported encryption mode");
    // UDP
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) != 1) { addrinfo* ai = nullptr; if (getaddrinfo(ip.c_str(), nullptr, nullptr, &ai) == 0 && ai) { sa.sin_addr = reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr; freeaddrinfo(ai); } }
    timeval tv{3, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    auto dreq = IpDiscoveryRequest(ssrc);
    sendto(fd, dreq.data(), dreq.size(), 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    Bytes resp(128);
    auto rn = recv(fd, resp.data(), resp.size(), 0);
    if (rn <= 0) { close(fd); return fail("ip discovery timeout"); }
    resp.resize(static_cast<std::size_t>(rn));
    auto disc = ParseIpDiscoveryResponse(resp);
    if (!disc) { close(fd); return fail("bad ip discovery response"); }
    rep["external"] = {{"ip", disc->ip}, {"port", disc->port}};
    send({{"op", 1}, {"d", {{"protocol", "udp"}, {"data", {{"address", disc->ip}, {"port", disc->port}, {"mode", mode}}}}}});
    auto sd = recv_op(4, 5000);
    if (!sd) { close(fd); return fail("no Session Description (op 4)"); }
    Bytes key;
    for (auto& v : (*sd)["d"].value("secret_key", nlohmann::json::array())) key.push_back(static_cast<std::uint8_t>(v.get<int>()));
    if (key.size() != 32) { close(fd); return fail("bad secret_key"); }
    send({{"op", 5}, {"d", {{"speaking", 1}, {"delay", 0}, {"ssrc", ssrc}}}});
    std::uint16_t seq = static_cast<std::uint16_t>(rand());
    std::uint32_t ts = static_cast<std::uint32_t>(rand()), nonce = 0;
    std::size_t sent = 0;
    auto next = std::chrono::steady_clock::now();
    auto last_hb = std::chrono::steady_clock::now();
    const double hbi = rep["heartbeat_interval"].get<double>();
    auto frames = rq.packets;
    for (int i = 0; i < 5; ++i) frames.push_back(kOpusSilence);
    for (auto& f : frames) {
        if (hbi > 0 && std::chrono::steady_clock::now() - last_hb > std::chrono::milliseconds(static_cast<int>(hbi))) {
            send({{"op", 3}, {"d", {{"t", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()}, {"seq_ack", seq_ack}}}});
            last_hb = std::chrono::steady_clock::now();
        }
        auto pkt = EncryptRtp(mode, key, RtpHeader(seq, ts, ssrc), f, nonce++);
        if (pkt) { sendto(fd, pkt->data(), pkt->size(), 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa); ++sent; }
        ++seq; ts += 960;
        next += std::chrono::milliseconds(rq.frame_ms);
        std::this_thread::sleep_until(next);
    }
    send({{"op", 5}, {"d", {{"speaking", 0}, {"delay", 0}, {"ssrc", ssrc}}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    close(fd);
    curl_easy_cleanup(c);
    rep["ok"] = true; rep["mode"] = mode; rep["ssrc"] = ssrc; rep["packets_sent"] = sent; rep["audio_frames"] = rq.packets.size();
    return rep;
}

}  // namespace openclaw::channels::discord_voice
