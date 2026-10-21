#ifdef OPENCLAW_HAVE_ZLIB
#include <zlib.h>
#endif
#include "openclaw/channels/feishu_frame.hpp"

namespace openclaw::channels::feishu_frame {
namespace {
void Varint(std::string& o, std::uint64_t v) { while (v >= 0x80) { o.push_back(static_cast<char>((v & 0x7f) | 0x80)); v >>= 7; } o.push_back(static_cast<char>(v)); }
void Tag(std::string& o, int field, int wt) { Varint(o, (static_cast<std::uint64_t>(field) << 3) | static_cast<unsigned>(wt)); }
void Bytes(std::string& o, int field, const std::string& s) { Tag(o, field, 2); Varint(o, s.size()); o += s; }
bool ReadVarint(const std::string& s, std::size_t& i, std::uint64_t& v) {
    v = 0;
    for (int sh = 0; sh < 64 && i < s.size(); sh += 7) {
        auto b = static_cast<std::uint8_t>(s[i++]);
        v |= static_cast<std::uint64_t>(b & 0x7f) << sh;
        if (!(b & 0x80)) return true;
    }
    return false;
}
bool Skip(const std::string& s, std::size_t& i, int wt) {
    std::uint64_t v;
    switch (wt) {
        case 0: return ReadVarint(s, i, v);
        case 1: i += 8; return i <= s.size();
        case 2: if (!ReadVarint(s, i, v) || i + v > s.size()) return false; i += v; return true;
        case 5: i += 4; return i <= s.size();
        default: return false;
    }
}
}  // namespace

std::string Encode(const Frame& f) {
    std::string o;
    Tag(o, 1, 0); Varint(o, f.seq_id);
    Tag(o, 2, 0); Varint(o, f.log_id);
    Tag(o, 3, 0); Varint(o, static_cast<std::uint64_t>(static_cast<std::int64_t>(f.service)));
    Tag(o, 4, 0); Varint(o, static_cast<std::uint64_t>(static_cast<std::int64_t>(f.method)));
    for (auto& [k, v] : f.headers) { std::string h; Bytes(h, 1, k); Bytes(h, 2, v); Bytes(o, 5, h); }
    if (!f.payload_encoding.empty()) Bytes(o, 6, f.payload_encoding);
    if (!f.payload_type.empty()) Bytes(o, 7, f.payload_type);
    if (!f.payload.empty()) Bytes(o, 8, f.payload);
    if (!f.log_id_new.empty()) Bytes(o, 9, f.log_id_new);
    return o;
}

std::optional<Frame> Decode(const std::string& s) {
    Frame f;
    std::size_t i = 0;
    while (i < s.size()) {
        std::uint64_t key;
        if (!ReadVarint(s, i, key)) return std::nullopt;
        const int field = static_cast<int>(key >> 3), wt = static_cast<int>(key & 7);
        if (wt == 0 && field >= 1 && field <= 4) {
            std::uint64_t v; if (!ReadVarint(s, i, v)) return std::nullopt;
            if (field == 1) f.seq_id = v; else if (field == 2) f.log_id = v;
            else if (field == 3) f.service = static_cast<std::int32_t>(v); else f.method = static_cast<std::int32_t>(v);
            continue;
        }
        if (wt == 2 && field >= 5 && field <= 9) {
            std::uint64_t n; if (!ReadVarint(s, i, n) || i + n > s.size()) return std::nullopt;
            std::string b = s.substr(i, n); i += n;
            if (field == 5) {
                std::string k, v; std::size_t j = 0;
                while (j < b.size()) {
                    std::uint64_t hk; if (!ReadVarint(b, j, hk)) return std::nullopt;
                    if ((hk & 7) != 2) { if (!Skip(b, j, static_cast<int>(hk & 7))) return std::nullopt; continue; }
                    std::uint64_t hn; if (!ReadVarint(b, j, hn) || j + hn > b.size()) return std::nullopt;
                    ((hk >> 3) == 1 ? k : v) = b.substr(j, hn); j += hn;
                }
                f.headers.emplace_back(k, v);
            } else if (field == 6) f.payload_encoding = b; else if (field == 7) f.payload_type = b;
            else if (field == 8) f.payload = b; else f.log_id_new = b;
            continue;
        }
        if (!Skip(s, i, wt)) return std::nullopt;
    }
    return f;
}

Frame Ping(std::int32_t service_id) {
    Frame f; f.service = service_id; f.method = kControl; f.headers = {{"type", "ping"}};
    return f;
}

static std::string B64(const std::string& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; int val = 0, bits = -6;
    for (unsigned char c : in) { val = (val << 8) + c; bits += 8; while (bits >= 0) { o.push_back(k[(val >> bits) & 0x3F]); bits -= 6; } }
    if (bits > -6) o.push_back(k[((val << 8) >> (bits + 8)) & 0x3F]);
    while (o.size() % 4) o.push_back('=');
    return o;
}

Frame Response(const Frame& in, int code, long long biz_rt_ms, const std::string& data) {
    Frame r = in;
    r.SetHeader("biz_rt", std::to_string(biz_rt_ms));
    // Go SDK marshals Response{StatusCode,Headers,Data []byte}: Data is base64 in JSON.
    r.payload = "{\"code\":" + std::to_string(code) + ",\"headers\":null,\"data\":" + (data.empty() ? std::string("null") : "\"" + B64(data) + "\"") + "}";
    r.payload_encoding.clear();
    return r;
}

std::optional<std::string> Gunzip(const std::string& in) {
#ifdef OPENCLAW_HAVE_ZLIB
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return std::nullopt;
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    std::string out; char buf[16384]; int rc;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf); zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&zs); return std::nullopt; }
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (out.size() > (64u << 20)) { inflateEnd(&zs); return std::nullopt; }
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
#else
    (void)in; return std::nullopt;
#endif
}

std::optional<std::string> Reassembler::Add(const Frame& f) {
    int sum = 1, seq = 0;
    try { if (!f.Header("sum").empty()) sum = std::stoi(f.Header("sum")); if (!f.Header("seq").empty()) seq = std::stoi(f.Header("seq")); } catch (...) {}
    if (sum <= 1) return f.payload;
    if (seq < 0 || seq >= sum || sum > 1024) return std::nullopt;
    auto& v = parts_[f.Header("message_id")];
    if (v.size() != static_cast<std::size_t>(sum)) v.assign(static_cast<std::size_t>(sum), std::nullopt);
    v[static_cast<std::size_t>(seq)] = f.payload;
    std::string all;
    for (auto& p : v) { if (!p) return std::nullopt; all += *p; }
    parts_.erase(f.Header("message_id"));
    return all;
}

}  // namespace openclaw::channels::feishu_frame
