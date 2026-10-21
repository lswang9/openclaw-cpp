#include "openclaw/channels/discord_voice.hpp"
#include "openclaw/channels/dave.hpp"
#include <map>

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
#include <cstdio>
#include <fstream>
#include <map>
#include <filesystem>
#ifdef OPENCLAW_HAVE_OPUS
#include <opus.h>
#endif

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

bool IsRtcp(const Bytes& p) { return p.size() >= 2 && (p[1] & 0x7f) >= 72 && (p[1] & 0x7f) <= 76; }

std::optional<Bytes> OpusPayload(const std::string& mode, const Bytes& key, const Bytes& p) {
    auto pt = DecryptRtp(mode, key, p);
    if (!pt) return std::nullopt;
    if (p[0] & 0x10) {
        const std::size_t eh = 12 + 4 * (p[0] & 0x0f);
        const std::size_t words = (std::size_t(p[eh + 2]) << 8) | p[eh + 3];
        if (pt->size() < 4 * words) return std::nullopt;
        pt->erase(pt->begin(), pt->begin() + static_cast<long>(4 * words));
    }
    return pt;
}

bool HaveOpus() {
#ifdef OPENCLAW_HAVE_OPUS
    return true;
#else
    return false;
#endif
}

std::vector<Bytes> EncodePcm(const Pcm& pcm, std::string* err) {
    std::vector<Bytes> out;
#ifdef OPENCLAW_HAVE_OPUS
    int e = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &e);
    if (!enc) { if (err) *err = "opus_encoder_create"; return out; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
    constexpr int kFrame = 960;
    std::vector<std::int16_t> frame(kFrame * 2);
    unsigned char buf[4000];
    for (std::size_t i = 0; i < pcm.size(); i += kFrame * 2) {
        std::fill(frame.begin(), frame.end(), 0);
        std::copy(pcm.begin() + static_cast<long>(i), pcm.begin() + static_cast<long>(std::min(pcm.size(), i + kFrame * 2)), frame.begin());
        int n = opus_encode(enc, frame.data(), kFrame, buf, sizeof buf);
        if (n < 0) { if (err) *err = opus_strerror(n); break; }
        out.emplace_back(buf, buf + n);
    }
    opus_encoder_destroy(enc);
#else
    (void)pcm; if (err) *err = "built without libopus";
#endif
    return out;
}

Pcm DecodeOpus(const std::vector<Bytes>& packets, std::string* err) {
    Pcm out;
#ifdef OPENCLAW_HAVE_OPUS
    int e = 0;
    OpusDecoder* dec = opus_decoder_create(48000, 2, &e);
    if (!dec) { if (err) *err = "opus_decoder_create"; return out; }
    std::vector<std::int16_t> buf(5760 * 2);
    for (auto& p : packets) {
        int n = opus_decode(dec, p.data(), static_cast<opus_int32>(p.size()), buf.data(), 5760, 0);
        if (n < 0) { if (err) *err = opus_strerror(n); continue; }
        out.insert(out.end(), buf.begin(), buf.begin() + n * 2);
    }
    opus_decoder_destroy(dec);
#else
    (void)packets; if (err) *err = "built without libopus";
#endif
    return out;
}

Bytes WavFromPcm(const Pcm& pcm) {
    Bytes o;
    auto s4 = [&](const char* t) { o.insert(o.end(), t, t + 4); };
    auto le = [&](std::uint32_t v, int n) { for (int i = 0; i < n; ++i) o.push_back((v >> (8 * i)) & 0xff); };
    const std::uint32_t data = static_cast<std::uint32_t>(pcm.size() * 2);
    s4("RIFF"); le(36 + data, 4); s4("WAVE"); s4("fmt "); le(16, 4); le(1, 2); le(2, 2); le(48000, 4); le(48000 * 4, 4); le(4, 2); le(16, 2);
    s4("data"); le(data, 4);
    for (auto v : pcm) le(static_cast<std::uint16_t>(v), 2);
    return o;
}

Pcm PcmFromAudioFile(const Bytes& d, std::string* err) {
    Pcm out;
    if (d.size() > 44 && std::memcmp(d.data(), "RIFF", 4) == 0 && std::memcmp(&d[8], "WAVE", 4) == 0) {
        std::size_t i = 12; int ch = 0, rate = 0, bits = 0, fmt = 0;
        while (i + 8 <= d.size()) {
            const std::uint32_t len = d[i + 4] | (d[i + 5] << 8) | (d[i + 6] << 16) | (std::uint32_t(d[i + 7]) << 24);
            if (std::memcmp(&d[i], "fmt ", 4) == 0 && len >= 16) {
                fmt = d[i + 8] | (d[i + 9] << 8); ch = d[i + 10] | (d[i + 11] << 8);
                rate = static_cast<int>(Le32(&d[i + 12])); bits = d[i + 22] | (d[i + 23] << 8);
            } else if (std::memcmp(&d[i], "data", 4) == 0) {
                if (fmt != 1 || bits != 16 || rate != 48000 || (ch != 1 && ch != 2)) break;  // → ffmpeg fallback
                const std::size_t end = std::min(d.size(), i + 8 + len);
                for (std::size_t k = i + 8; k + 1 < end; k += 2) {
                    auto v = static_cast<std::int16_t>(d[k] | (d[k + 1] << 8));
                    out.push_back(v); if (ch == 1) out.push_back(v);
                }
                return out;
            }
            i += 8 + len + (len & 1);
        }
    }
    // ffmpeg fallback (mp3 from TTS providers, other rates).
    auto tmp = std::filesystem::temp_directory_path() / ("oc-dv-" + std::to_string(::getpid()) + "-" + std::to_string(rand()));
    { std::ofstream f(tmp, std::ios::binary); f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size())); }
    const std::string cmd = "ffmpeg -nostdin -loglevel error -i '" + tmp.string() + "' -f s16le -ac 2 -ar 48000 - 2>/dev/null";
    if (FILE* pp = popen(cmd.c_str(), "r")) {
        std::int16_t buf[4096]; size_t n;
        while ((n = fread(buf, 2, 4096, pp)) > 0) out.insert(out.end(), buf, buf + n);
        pclose(pp);
    }
    std::filesystem::remove(tmp);
    if (out.empty() && err) *err = "unsupported audio (need Ogg Opus, 48 kHz s16 WAV, or ffmpeg on PATH)";
    return out;
}

std::vector<Bytes> PacketsFromAudio(const Bytes& d, std::string* err) {
    if (d.size() > 4 && std::memcmp(d.data(), "OggS", 4) == 0) {
        auto p = OggOpusPackets(d, err);
        if (!p.empty()) return p;
    }
    auto pcm = PcmFromAudioFile(d, err);
    if (pcm.empty()) return {};
    return EncodePcm(pcm, err);
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
    auto send_bin = [&](int op, const Bytes& payload) {
        std::string s(1, static_cast<char>(op)); s.append(payload.begin(), payload.end());
        size_t n = 0; curl_ws_send(c, s.data(), s.size(), &n, 0, CURLWS_BINARY);
    };
    std::uint64_t group = 0;
    try { group = rq.channel_id.empty() ? 0 : std::stoull(rq.channel_id); } catch (...) {}
    const std::uint16_t max_dave = rq.disable_dave ? 0 : dave::MaxProtocolVersion();
    dave::Session ds(rq.user_id, group);
    std::map<std::uint32_t, std::string> users;  // ssrc → user_id (op 5 Speaking)
    auto flush = [&](const std::vector<dave::Outgoing>& outs) {
        for (auto& o : outs) { if (o.binary) send_bin(o.op, o.payload); else send(o.json); }
    };
    std::string acc;
    int seq_ack = -1;
    int close_code = 0;
    // Handles one complete WS message: DAVE/roster ops are consumed here; returns the JSON for other ops.
    auto handle = [&](bool binary, const std::string& data) -> std::optional<nlohmann::json> {
        if (binary) {
            if (data.size() < 3) return std::nullopt;
            seq_ack = (static_cast<unsigned char>(data[0]) << 8) | static_cast<unsigned char>(data[1]);
            const int op = static_cast<unsigned char>(data[2]);
            flush(ds.OnBinary(op, Bytes(data.begin() + 3, data.end())));
            return std::nullopt;
        }
        nlohmann::json j;
        try { j = nlohmann::json::parse(data); } catch (...) { return std::nullopt; }
        if (j.contains("seq") && j["seq"].is_number()) seq_ack = j["seq"];
        const int op = j.value("op", -1);
        if (op == 5 && j.contains("d")) { users[j["d"].value("ssrc", 0u)] = j["d"].value("user_id", std::string()); ds.UserConnected(j["d"].value("user_id", std::string())); }
        if (op == 11 || op == 13 || (op >= 21 && op <= 24)) { flush(ds.OnJson(op, j.value("d", nlohmann::json::object()))); return std::nullopt; }
        return j;
    };
    auto pump_once = [&](int want, std::optional<nlohmann::json>* got) -> bool {  // false on close/error
        char buf[65536];
        size_t n = 0; const curl_ws_frame* m = nullptr;
        auto r = curl_ws_recv(c, buf, sizeof buf, &n, &m);
        if (r == CURLE_AGAIN) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); return true; }
        if (r != CURLE_OK) return false;
        if (m && (m->flags & CURLWS_CLOSE)) {
            if (n >= 2) close_code = (static_cast<unsigned char>(buf[0]) << 8) | static_cast<unsigned char>(buf[1]);
            return false;
        }
        acc.append(buf, n);
        if (m && m->bytesleft > 0) return true;
        const bool binary = m && (m->flags & CURLWS_BINARY);
        auto j = handle(binary, acc);
        acc.clear();
        if (j && j->value("op", -1) == want && got) *got = j;
        return true;
    };
    auto recv_op = [&](int want, int timeout_ms) -> std::optional<nlohmann::json> {
        const auto t0 = std::chrono::steady_clock::now();
        std::optional<nlohmann::json> got;
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(timeout_ms)) {
            if (!pump_once(want, &got)) return std::nullopt;
            if (got) return got;
        }
        return std::nullopt;
    };
    auto fail = [&](const std::string& e) {
        rep["error"] = e;
        if (close_code) rep["close_code"] = close_code;
        if (close_code == 4017) {  // Discord: "E2EE/DAVE protocol required" — mandatory for non-stage calls since 2026-03-01
            rep["error"] = max_dave ? "voice server rejected DAVE negotiation (close 4017)"
                                    : "voice server requires DAVE end-to-end encryption (close 4017); this build has no libdave "
                                      "(run scripts/build-dave.sh and rebuild) — see docs/DAVE.md";
            rep["dave_required"] = true;
        }
        rep["dave"] = ds.Stats();
        curl_easy_cleanup(c);
        return rep;
    };
    auto hello = recv_op(8, 5000);
    if (!hello) return fail("no Hello (op 8)");
    rep["heartbeat_interval"] = (*hello)["d"].value("heartbeat_interval", 0.0);
    send({{"op", 0}, {"d", {{"server_id", rq.server_id}, {"user_id", rq.user_id}, {"session_id", rq.session_id}, {"token", rq.token}, {"max_dave_protocol_version", max_dave}}}});
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
    const int dave_v = (*sd)["d"].value("dave_protocol_version", 0);
    if (dave_v > 0) {
        if (!max_dave) { close(fd); close_code = 4017; return fail("dave"); }
        if (!group) { close(fd); return fail("DAVE needs the voice channel id (MLS group id)"); }
        flush(ds.Start(static_cast<std::uint16_t>(dave_v)));
        // Wait for the MLS group (proposals → commit/welcome → execute) before sending media.
        const auto t0 = std::chrono::steady_clock::now();
        while (!ds.Ready() && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(rq.dave_timeout_ms)) {
            if (!pump_once(-2, nullptr)) break;
        }
        if (!ds.Ready()) { close(fd); return fail("DAVE group not established: " + ds.LastError()); }
    }
    rep["dave_protocol_version"] = dave_v;
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
    std::size_t dave_fail = 0;
    for (int i = 0; i < 5; ++i) frames.push_back(kOpusSilence);
    for (auto& f : frames) {
        if (hbi > 0 && std::chrono::steady_clock::now() - last_hb > std::chrono::milliseconds(static_cast<int>(hbi))) {
            send({{"op", 3}, {"d", {{"t", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()}, {"seq_ack", seq_ack}}}});
            last_hb = std::chrono::steady_clock::now();
        }
        // pump DAVE control traffic (transitions, new members) between frames
        for (int k = 0; k < 4; ++k) if (!pump_once(-2, nullptr)) break;
        Bytes frame;
        if (!ds.Encrypt(ssrc, f, &frame)) { ++dave_fail; ++seq; ts += 960; next += std::chrono::milliseconds(rq.frame_ms); continue; }
        auto pkt = EncryptRtp(mode, key, RtpHeader(seq, ts, ssrc), frame, nonce++);
        if (pkt) { sendto(fd, pkt->data(), pkt->size(), 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa); ++sent; }
        ++seq; ts += 960;
        next += std::chrono::milliseconds(rq.frame_ms);
        std::this_thread::sleep_until(next);
    }
    send({{"op", 5}, {"d", {{"speaking", 0}, {"delay", 0}, {"ssrc", ssrc}}}});
    if (rq.listen_ms > 0) {
        std::map<std::uint32_t, std::vector<Bytes>> streams;
        std::size_t bad = 0, dave_bad = 0;
        timeval stv{0, 50000}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &stv, sizeof stv);
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(rq.listen_ms);
        Bytes pb(2048);
        char wbuf[65536];
        while (std::chrono::steady_clock::now() < until) {
            // drain WS: op 5 Speaking maps ssrc → user_id; DAVE control traffic keeps the group current
            for (int k = 0; k < 16; ++k) if (!pump_once(-2, nullptr)) break;
            auto n = recv(fd, pb.data(), pb.size(), 0);
            if (n <= 0) continue;
            Bytes pkt(pb.begin(), pb.begin() + n);
            if (pkt.size() < 12 || (pkt[0] >> 6) != 2 || IsRtcp(pkt)) continue;
            auto op = OpusPayload(mode, key, pkt);
            if (!op) { ++bad; continue; }
            if (*op == kOpusSilence) continue;
            Bytes plain;
            const auto who = users.count(Rd32(&pkt[8])) ? users[Rd32(&pkt[8])] : std::string();
            if (!ds.Decrypt(who, *op, &plain)) { ++dave_bad; continue; }
            if (plain == kOpusSilence) continue;
            streams[Rd32(&pkt[8])].push_back(plain);
        }
        nlohmann::json recv_rep = nlohmann::json::array();
        for (auto& [s, pk] : streams) {
            std::string derr;
            auto pcm = DecodeOpus(pk, &derr);
            nlohmann::json e{{"ssrc", s}, {"user_id", users.count(s) ? users[s] : ""}, {"packets", pk.size()}, {"samples", pcm.size() / 2}};
            if (!derr.empty()) e["decode_error"] = derr;
            if (!rq.receive_dir.empty() && !pcm.empty()) {
                std::filesystem::create_directories(rq.receive_dir);
                auto path = std::filesystem::path(rq.receive_dir) / ("ssrc-" + std::to_string(s) + ".wav");
                auto wav = WavFromPcm(pcm);
                std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
                e["wav"] = path.string();
            }
            recv_rep.push_back(e);
        }
        rep["received"] = recv_rep;
        rep["undecryptable"] = bad;
        rep["dave_undecryptable"] = dave_bad;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    close(fd);
    curl_easy_cleanup(c);
    rep["ok"] = true; rep["mode"] = mode; rep["ssrc"] = ssrc; rep["packets_sent"] = sent; rep["audio_frames"] = rq.packets.size();
    rep["dave"] = ds.Stats(); rep["dave"]["encrypt_failures"] = dave_fail;
    return rep;
}

}  // namespace openclaw::channels::discord_voice
