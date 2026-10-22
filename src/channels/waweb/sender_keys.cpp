#include "openclaw/channels/waweb/sender_keys.hpp"

#include "openclaw/persist/state_db.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace openclaw::channels::waweb {
namespace {
using json = nlohmann::json;
constexpr std::uint8_t kVer = 0x33;
constexpr std::uint32_t kMaxSkip = 2000;
Bytes Cat(std::initializer_list<Bytes> ps) { Bytes o; for (auto& p : ps) o.insert(o.end(), p.begin(), p.end()); return o; }
void Var(Bytes& o, std::uint64_t v) { while (v >= 0x80) { o.push_back(static_cast<std::uint8_t>(v | 0x80)); v >>= 7; } o.push_back(static_cast<std::uint8_t>(v)); }
void FB(Bytes& o, int n, const Bytes& v) { Var(o, (std::uint64_t(n) << 3) | 2); Var(o, v.size()); o.insert(o.end(), v.begin(), v.end()); }
void FV(Bytes& o, int n, std::uint64_t v) { Var(o, std::uint64_t(n) << 3); Var(o, v); }
struct Pb { std::map<int, Bytes> b; std::map<int, std::uint64_t> v; };
std::optional<Pb> Parse(const Bytes& d) {
    Pb p; std::size_t i = 0;
    auto var = [&](std::uint64_t& v) { v = 0; for (int sh = 0; i < d.size() && sh < 64; sh += 7) { auto c = d[i++]; v |= std::uint64_t(c & 0x7f) << sh; if (!(c & 0x80)) return true; } return false; };
    while (i < d.size()) {
        std::uint64_t k; if (!var(k)) return std::nullopt;
        int f = static_cast<int>(k >> 3), wt = static_cast<int>(k & 7);
        if (wt == 0) { std::uint64_t v; if (!var(v)) return std::nullopt; p.v[f] = v; }
        else if (wt == 2) { std::uint64_t n; if (!var(n) || i + n > d.size()) return std::nullopt; p.b[f] = Bytes(d.begin() + i, d.begin() + i + n); i += n; }
        else return std::nullopt;
    }
    return p;
}
Bytes DeriveMsgSeed(const Bytes& ck) { return HmacSha256(ck, Bytes{0x01}); }
Bytes NextChain(const Bytes& ck) { return HmacSha256(ck, Bytes{0x02}); }
struct Mk { Bytes iv, cipher; };
Mk Expand(const Bytes& seed) {
    auto k = Hkdf(seed, 64, Bytes(32, 0), "WhisperGroup");
    Mk m; m.iv.assign(k.begin(), k.begin() + 16);
    m.cipher.assign(k.begin() + 16, k.begin() + 48);  // der[0][16:]+der[1][:16]
    return m;
}
Bytes Aes(bool enc, const Mk& k, const Bytes& in, bool& ok) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    Bytes out(in.size() + 32); int l = 0, f = 0;
    ok = EVP_CipherInit_ex(c, EVP_aes_256_cbc(), nullptr, k.cipher.data(), k.iv.data(), enc ? 1 : 0) == 1 &&
         EVP_CipherUpdate(c, out.data(), &l, in.data(), static_cast<int>(in.size())) == 1 &&
         EVP_CipherFinal_ex(c, out.data() + l, &f) == 1;
    EVP_CIPHER_CTX_free(c);
    out.resize(ok ? static_cast<std::size_t>(l + f) : 0);
    return out;
}
std::string Hex(const Bytes& b) {
    static const char* x = "0123456789abcdef"; std::string o;
    for (auto c : b) { o += x[c >> 4]; o += x[c & 15]; } return o;
}
Bytes Unhex(const std::string& s) {
    Bytes b; for (std::size_t i = 0; i + 1 < s.size(); i += 2) b.push_back(static_cast<std::uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16))); return b;
}
json KP(const KeyPair& k) { return {{"pub", Hex(k.pub)}, {"priv", Hex(k.priv)}}; }
KeyPair KPf(const json& j) { return {Unhex(j.value("pub", "")), Unhex(j.value("priv", ""))}; }
std::filesystem::path KeysPath(const std::filesystem::path& dir) { return dir / "waweb-keys.json"; }
}  // namespace

SenderKeyState CreateSenderKey(std::uint32_t key_id) {
    SenderKeyState s; s.key_id = key_id; s.iteration = 0; s.chain_key = RandomBytes(32); s.signing = GenerateKeyPair();
    return s;
}

Bytes EncodeSkdm(const SenderKeyState& s) {
    Bytes pb; FV(pb, 1, s.key_id); FV(pb, 2, s.iteration); FB(pb, 3, s.chain_key); FB(pb, 4, s.signing.pub);
    Bytes o{kVer}; o.insert(o.end(), pb.begin(), pb.end()); return o;
}

std::optional<SenderKeyState> DecodeSkdm(const Bytes& raw, std::string* err) {
    if (raw.size() < 2 || raw[0] != kVer) { if (err) *err = "bad skdm version"; return std::nullopt; }
    auto p = Parse(Bytes(raw.begin() + 1, raw.end()));
    if (!p || !p->b.count(3) || !p->b.count(4)) { if (err) *err = "bad skdm protobuf"; return std::nullopt; }
    SenderKeyState s; s.key_id = static_cast<std::uint32_t>(p->v[1]); s.iteration = static_cast<std::uint32_t>(p->v[2]);
    s.chain_key = p->b[3]; s.signing.pub = p->b[4];
    if (s.chain_key.size() != 32 || s.signing.pub.size() != 32) { if (err) *err = "bad skdm key sizes"; return std::nullopt; }
    return s;
}

SenderKeyState ProcessSkdm(const Bytes& raw, std::string* err) {
    auto s = DecodeSkdm(raw, err); return s ? *s : SenderKeyState{};
}

SkmEnvelope SenderKeyEncrypt(SenderKeyState& s, const Bytes& plaintext) {
    auto seed = DeriveMsgSeed(s.chain_key);
    auto mk = Expand(seed);
    bool ok; auto ct = Aes(true, mk, plaintext, ok);
    Bytes pb; FV(pb, 1, s.key_id); FV(pb, 2, s.iteration); FB(pb, 3, ct);
    Bytes ser{kVer}; ser.insert(ser.end(), pb.begin(), pb.end());
    auto sig = SignXEdDsa(s.signing.priv, ser);
    ser.insert(ser.end(), sig.begin(), sig.end());
    SkmEnvelope e; e.body = ser; e.key_id = s.key_id; e.iteration = s.iteration;
    s.chain_key = NextChain(s.chain_key); ++s.iteration;
    return e;
}

std::optional<Bytes> SenderKeyDecrypt(SenderKeyState& s, const Bytes& skm, std::string* err) {
    if (skm.size() < 1 + 64 || skm[0] != kVer) { if (err) *err = "bad skm version"; return std::nullopt; }
    Bytes ser(skm.begin(), skm.end() - 64), sig(skm.end() - 64, skm.end());
    if (!VerifyXEdDsa(s.signing.pub, ser, sig)) { if (err) *err = "bad skm signature"; return std::nullopt; }
    auto p = Parse(Bytes(ser.begin() + 1, ser.end()));
    if (!p || !p->b.count(3)) { if (err) *err = "bad skm protobuf"; return std::nullopt; }
    auto key_id = static_cast<std::uint32_t>(p->v[1]);
    auto iteration = static_cast<std::uint32_t>(p->v[2]);
    const auto& ct = p->b[3];
    if (key_id != s.key_id) { if (err) *err = "skm key id mismatch"; return std::nullopt; }

    Bytes seed;
    if (iteration < s.iteration) {
        auto it = s.skipped.find(iteration);
        if (it == s.skipped.end()) { if (err) *err = "duplicate or missing skipped key"; return std::nullopt; }
        seed = it->second; s.skipped.erase(it);
    } else {
        if (iteration - s.iteration > kMaxSkip) { if (err) *err = "message too far in the future"; return std::nullopt; }
        while (s.iteration < iteration) {
            s.skipped[s.iteration] = DeriveMsgSeed(s.chain_key);
            if (s.skipped.size() > kMaxSkip) s.skipped.erase(s.skipped.begin());
            s.chain_key = NextChain(s.chain_key); ++s.iteration;
        }
        seed = DeriveMsgSeed(s.chain_key);
        s.chain_key = NextChain(s.chain_key); ++s.iteration;
    }
    auto mk = Expand(seed);
    bool ok; auto pt = Aes(false, mk, ct, ok);
    if (!ok) { if (err) *err = "aes decrypt failed"; return std::nullopt; }
    return pt;
}

bool SaveSenderKey(const std::filesystem::path& dir, const SenderKeyRecord& r) {
    auto keys = json::parse(persist::StateRead(KeysPath(dir)).value_or("{}"), nullptr, false);
    if (!keys.is_object()) keys = json::object();
    json states = json::array();
    for (auto& st : r.states) {
        json skipped = json::object();
        for (auto& [i, seed] : st.skipped) skipped[std::to_string(i)] = Hex(seed);
        states.push_back({{"keyId", st.key_id}, {"iteration", st.iteration}, {"chainKey", Hex(st.chain_key)},
                          {"signing", KP(st.signing)}, {"skipped", skipped}});
    }
    keys["sender-key"][r.group_id + "|" + r.sender_addr] = {{"groupId", r.group_id}, {"sender", r.sender_addr}, {"states", states}};
    return persist::StateWrite(KeysPath(dir), keys.dump(2));
}

std::optional<SenderKeyRecord> LoadSenderKey(const std::filesystem::path& dir, const std::string& group_id,
                                             const std::string& sender_addr) {
    auto keys = json::parse(persist::StateRead(KeysPath(dir)).value_or("{}"), nullptr, false);
    if (!keys.is_object() || !keys.contains("sender-key")) return std::nullopt;
    const auto id = group_id + "|" + sender_addr;
    if (!keys["sender-key"].contains(id)) return std::nullopt;
    const auto& j = keys["sender-key"][id];
    SenderKeyRecord r; r.group_id = group_id; r.sender_addr = sender_addr;
    for (auto& stj : j.value("states", json::array())) {
        SenderKeyState st; st.key_id = stj.value("keyId", 0u); st.iteration = stj.value("iteration", 0u);
        st.chain_key = Unhex(stj.value("chainKey", "")); st.signing = KPf(stj.value("signing", json::object()));
        for (auto& [k, v] : stj.value("skipped", json::object()).items()) st.skipped[static_cast<std::uint32_t>(std::stoul(k))] = Unhex(v.get<std::string>());
        r.states.push_back(st);
    }
    return r;
}

}  // namespace openclaw::channels::waweb
