#include "openclaw/channels/waweb/auth_store.hpp"

#include "openclaw/persist/state_db.hpp"

#include <nlohmann/json.hpp>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <random>

namespace openclaw::channels::waweb {
namespace {
using json = nlohmann::json;

std::string Hex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}
Bytes Unhex(const std::string& s) {
    Bytes b;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) b.push_back(static_cast<std::uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    return b;
}
Bytes Sha512(const Bytes& d) {
    Bytes o(64); unsigned int n = 0;
    EVP_Digest(d.data(), d.size(), o.data(), &n, EVP_sha512(), nullptr);
    return o;
}

// ---- minimal Ed25519 (affine, BIGNUM) for XEdDSA signing ----
struct Ed {
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM *p = BN_new(), *q = BN_new(), *d = BN_new(), *bx = BN_new(), *by = BN_new();
    Ed() {
        BN_set_word(p, 1); BN_lshift(p, p, 255); BN_sub_word(p, 19);
        BN_dec2bn(&q, "7237005577332262213973186563042994240857116359379907606001950938285454250989");
        BIGNUM *n = BN_new(), *den = BN_new();
        BN_set_word(n, 121665); BN_sub(n, p, n);
        BN_set_word(den, 121666);
        BIGNUM* inv = BN_mod_inverse(nullptr, den, p, ctx);
        BN_mod_mul(d, n, inv, p, ctx);
        BN_free(n); BN_free(den); BN_free(inv);
        BN_dec2bn(&bx, "15112221349535400772501151409588531511454012693041857206046113283949847762202");
        BN_dec2bn(&by, "46316835694926478169428394003475163141307993866256225615783033603165251855960");
    }
    ~Ed() { BN_free(p); BN_free(q); BN_free(d); BN_free(bx); BN_free(by); BN_CTX_free(ctx); }
    // (x1,y1) += (x2,y2)
    void Add(BIGNUM* x1, BIGNUM* y1, const BIGNUM* x2, const BIGNUM* y2) {
        BIGNUM *a = BN_new(), *b = BN_new(), *t = BN_new(), *one = BN_new(), *num = BN_new(), *den = BN_new(), *nx = BN_new(), *ny = BN_new();
        BN_one(one);
        BN_mod_mul(t, x1, x2, p, ctx); BN_mod_mul(t, t, y1, p, ctx); BN_mod_mul(t, t, y2, p, ctx); BN_mod_mul(t, t, d, p, ctx);
        BN_mod_mul(a, x1, y2, p, ctx); BN_mod_mul(b, y1, x2, p, ctx); BN_mod_add(num, a, b, p, ctx);
        BN_mod_add(den, one, t, p, ctx);
        BIGNUM* inv = BN_mod_inverse(nullptr, den, p, ctx);
        BN_mod_mul(nx, num, inv, p, ctx); BN_free(inv);
        BN_mod_mul(a, y1, y2, p, ctx); BN_mod_mul(b, x1, x2, p, ctx); BN_mod_add(num, a, b, p, ctx);
        BN_mod_sub(den, one, t, p, ctx);
        inv = BN_mod_inverse(nullptr, den, p, ctx);
        BN_mod_mul(ny, num, inv, p, ctx); BN_free(inv);
        BN_copy(x1, nx); BN_copy(y1, ny);
        BN_free(a); BN_free(b); BN_free(t); BN_free(one); BN_free(num); BN_free(den); BN_free(nx); BN_free(ny);
    }
    Bytes MulBase(const BIGNUM* k) {
        BIGNUM *rx = BN_new(), *ry = BN_new(), *px = BN_dup(bx), *py = BN_dup(by);
        BN_zero(rx); BN_one(ry);
        for (int i = 0; i < BN_num_bits(k); ++i) {
            if (BN_is_bit_set(k, i)) Add(rx, ry, px, py);
            BIGNUM *cx = BN_dup(px), *cy = BN_dup(py);
            Add(px, py, cx, cy); BN_free(cx); BN_free(cy);
        }
        Bytes out(32);
        BN_bn2lebinpad(ry, out.data(), 32);
        if (BN_is_odd(rx)) out[31] |= 0x80;
        BN_free(rx); BN_free(ry); BN_free(px); BN_free(py);
        return out;
    }
    BIGNUM* ModQ(const Bytes& le) {
        BIGNUM* v = BN_lebin2bn(le.data(), static_cast<int>(le.size()), nullptr);
        BN_nnmod(v, v, q, ctx);
        return v;
    }
};

json KP(const KeyPair& k) { return {{"pub", Hex(k.pub)}, {"priv", Hex(k.priv)}}; }
KeyPair KPf(const json& j) { return {Unhex(j.value("pub", "")), Unhex(j.value("priv", ""))}; }
std::filesystem::path Doc(const std::filesystem::path& dir, const char* n) { return dir / n; }
void BE(Bytes& b, std::uint64_t v, int n) { for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
BinaryNode Leaf(const std::string& tag, Bytes content) { BinaryNode n; n.tag = tag; n.content = std::move(content); n.has_content = true; return n; }
}  // namespace

Bytes SignXEdDsa(const Bytes& mont_priv, const Bytes& msg) {
    Ed e;
    Bytes k = mont_priv; k.resize(32);
    k[0] &= 248; k[31] &= 127; k[31] |= 64;
    BIGNUM* a = BN_lebin2bn(k.data(), 32, nullptr);
    Bytes A = e.MulBase(a);
    BN_nnmod(a, a, e.q, e.ctx);
    if (A[31] & 0x80) { BN_sub(a, e.q, a); A[31] &= 0x7F; }  // force sign bit 0
    Bytes abytes(32); BN_bn2lebinpad(a, abytes.data(), 32);
    Bytes Z(64); RAND_bytes(Z.data(), 64);
    Bytes h1(32, 0xFF); h1[0] = 0xFE;
    h1.insert(h1.end(), abytes.begin(), abytes.end()); h1.insert(h1.end(), msg.begin(), msg.end()); h1.insert(h1.end(), Z.begin(), Z.end());
    BIGNUM* r = e.ModQ(Sha512(h1));
    Bytes R = e.MulBase(r);
    Bytes h2 = R; h2.insert(h2.end(), A.begin(), A.end()); h2.insert(h2.end(), msg.begin(), msg.end());
    BIGNUM* h = e.ModQ(Sha512(h2));
    BIGNUM* s = BN_new();
    BN_mod_mul(s, h, a, e.q, e.ctx); BN_mod_add(s, s, r, e.q, e.ctx);
    Bytes sig = R; Bytes sb(32); BN_bn2lebinpad(s, sb.data(), 32);
    sig.insert(sig.end(), sb.begin(), sb.end());
    sig[63] = static_cast<std::uint8_t>((sig[63] & 0x7F) | (A[31] & 0x80));
    BN_free(a); BN_free(r); BN_free(h); BN_free(s);
    return sig;
}

AuthCreds InitAuthCreds() {
    AuthCreds c;
    c.noise_key = GenerateKeyPair();
    c.pairing_ephemeral = GenerateKeyPair();
    std::random_device rd;
    c.signal = signal::NewIdentity((rd() & 16383u) + 1, 0);
    c.signal.signed_prekey_id = 1;
    c.signed_prekey_sig = SignXEdDsa(c.signal.identity.priv, signal::SerializePub(c.signal.signed_prekey.pub));
    c.adv_secret = RandomBytes(32);
    return c;
}

bool SaveAuthCreds(const std::filesystem::path& dir, const AuthCreds& c) {
    json creds{{"noiseKey", KP(c.noise_key)}, {"pairingEphemeralKeyPair", KP(c.pairing_ephemeral)},
               {"signedIdentityKey", KP(c.signal.identity)}, {"registrationId", c.signal.registration_id},
               {"signedPreKey", {{"keyId", c.signal.signed_prekey_id}, {"keyPair", KP(c.signal.signed_prekey)}, {"signature", Hex(c.signed_prekey_sig)}}},
               {"advSecretKey", Base64(c.adv_secret)}, {"nextPreKeyId", c.next_prekey_id},
               {"firstUnuploadedPreKeyId", c.first_unuploaded_prekey_id}, {"me", c.me_jid}, {"platform", c.platform},
               {"registered", c.registered}};
    auto keys = json::parse(persist::StateRead(Doc(dir, "waweb-keys.json")).value_or("{}"), nullptr, false);
    if (!keys.is_object()) keys = json::object();
    json pk = json::object();
    for (auto& [id, kp] : c.signal.prekeys) pk[std::to_string(id)] = KP(kp);
    keys["pre-key"] = pk;
    return persist::StateWrite(Doc(dir, "waweb-creds.json"), creds.dump(2)) && persist::StateWrite(Doc(dir, "waweb-keys.json"), keys.dump(2));
}

std::optional<AuthCreds> LoadAuthCreds(const std::filesystem::path& dir) {
    auto t = persist::StateRead(Doc(dir, "waweb-creds.json"));
    if (!t) return std::nullopt;
    auto j = json::parse(*t, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    AuthCreds c;
    c.noise_key = KPf(j.value("noiseKey", json::object()));
    c.pairing_ephemeral = KPf(j.value("pairingEphemeralKeyPair", json::object()));
    c.signal.identity = KPf(j.value("signedIdentityKey", json::object()));
    c.signal.registration_id = j.value("registrationId", 0u);
    auto spk = j.value("signedPreKey", json::object());
    c.signal.signed_prekey_id = spk.value("keyId", 1u);
    c.signal.signed_prekey = KPf(spk.value("keyPair", json::object()));
    c.signed_prekey_sig = Unhex(spk.value("signature", ""));
    std::string adv = j.value("advSecretKey", "");
    { // base64 decode
        Bytes out((adv.size() * 3) / 4 + 3);
        int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(adv.data()), static_cast<int>(adv.size()));
        if (n > 0) { out.resize(static_cast<std::size_t>(n)); while (!adv.empty() && adv.back() == '=' && !out.empty()) { out.pop_back(); adv.pop_back(); } c.adv_secret = out; }
    }
    c.next_prekey_id = j.value("nextPreKeyId", 1u);
    c.first_unuploaded_prekey_id = j.value("firstUnuploadedPreKeyId", 1u);
    c.me_jid = j.value("me", ""); c.platform = j.value("platform", ""); c.registered = j.value("registered", false);
    auto keys = json::parse(persist::StateRead(Doc(dir, "waweb-keys.json")).value_or("{}"), nullptr, false);
    if (keys.is_object() && keys.contains("pre-key"))
        for (auto& [id, kp] : keys["pre-key"].items()) c.signal.prekeys[static_cast<std::uint32_t>(std::stoul(id))] = KPf(kp);
    return c;
}

bool SaveSession(const std::filesystem::path& dir, const std::string& addr, const signal::Session& s) {
    auto keys = json::parse(persist::StateRead(Doc(dir, "waweb-keys.json")).value_or("{}"), nullptr, false);
    if (!keys.is_object()) keys = json::object();
    json sk = json::object();
    for (auto& [k, v] : s.skipped) sk[k] = Hex(v);
    keys["session"][addr] = {{"localIdentity", Hex(s.local_identity)}, {"remoteIdentity", Hex(s.remote_identity)},
        {"rootKey", Hex(s.root_key)}, {"ourRatchet", KP(s.our_ratchet)}, {"theirRatchet", Hex(s.their_ratchet)},
        {"sendChain", Hex(s.send_chain)}, {"sendN", s.send_n}, {"prevN", s.prev_n}, {"recvChain", Hex(s.recv_chain)},
        {"recvN", s.recv_n}, {"skipped", sk}, {"pendingPreKey", s.pending_prekey}, {"pkReg", s.pk_reg},
        {"pkPreKeyId", s.pk_prekey_id}, {"pkSignedId", s.pk_signed_id}, {"pkBaseKey", Hex(s.pk_base_key)}, {"localReg", s.local_reg}};
    return persist::StateWrite(Doc(dir, "waweb-keys.json"), keys.dump(2));
}

std::optional<signal::Session> LoadSession(const std::filesystem::path& dir, const std::string& addr) {
    auto keys = json::parse(persist::StateRead(Doc(dir, "waweb-keys.json")).value_or("{}"), nullptr, false);
    if (!keys.is_object() || !keys.contains("session") || !keys["session"].contains(addr)) return std::nullopt;
    const auto& j = keys["session"][addr];
    signal::Session s;
    s.local_identity = Unhex(j.value("localIdentity", "")); s.remote_identity = Unhex(j.value("remoteIdentity", ""));
    s.root_key = Unhex(j.value("rootKey", "")); s.our_ratchet = KPf(j.value("ourRatchet", json::object()));
    s.their_ratchet = Unhex(j.value("theirRatchet", "")); s.send_chain = Unhex(j.value("sendChain", ""));
    s.send_n = j.value("sendN", 0u); s.prev_n = j.value("prevN", 0u); s.recv_chain = Unhex(j.value("recvChain", ""));
    s.recv_n = j.value("recvN", 0u);
    for (auto& [k, v] : j.value("skipped", json::object()).items()) s.skipped[k] = Unhex(v.get<std::string>());
    s.pending_prekey = j.value("pendingPreKey", false); s.pk_reg = j.value("pkReg", 0u);
    s.pk_prekey_id = j.value("pkPreKeyId", 0u); s.pk_signed_id = j.value("pkSignedId", 0u);
    s.pk_base_key = Unhex(j.value("pkBaseKey", "")); s.local_reg = j.value("localReg", 0u);
    return s;
}

BinaryNode BuildPreKeyUploadNode(AuthCreds& c, int count, const std::string& iq_id) {
    BinaryNode iq; iq.tag = "iq";
    iq.attrs = {{"xmlns", "encrypt"}, {"type", "set"}, {"to", "s.whatsapp.net"}, {"id", iq_id}};
    Bytes reg; BE(reg, c.signal.registration_id, 4);
    iq.children.push_back(Leaf("registration", reg));
    iq.children.push_back(Leaf("type", Bytes{0x05}));
    iq.children.push_back(Leaf("identity", c.signal.identity.pub));
    BinaryNode list; list.tag = "list"; list.has_content = true;
    for (int i = 0; i < count; ++i) {
        std::uint32_t id = c.next_prekey_id++;
        auto kp = GenerateKeyPair();
        c.signal.prekeys[id] = kp;
        BinaryNode key; key.tag = "key"; key.has_content = true;
        Bytes idb; BE(idb, id, 3);
        key.children = {Leaf("id", idb), Leaf("value", kp.pub)};
        list.children.push_back(key);
    }
    c.first_unuploaded_prekey_id = c.next_prekey_id;
    iq.children.push_back(list);
    BinaryNode sk; sk.tag = "skey"; sk.has_content = true;
    Bytes sid; BE(sid, c.signal.signed_prekey_id, 3);
    sk.children = {Leaf("id", sid), Leaf("value", c.signal.signed_prekey.pub), Leaf("signature", c.signed_prekey_sig)};
    iq.children.push_back(sk);
    iq.has_content = true;
    return iq;
}

std::optional<int> ParsePreKeyCount(const BinaryNode& n) {
    if (n.tag != "iq") return std::nullopt;
    for (auto& ch : n.children)
        if (ch.tag == "count")
            for (auto& [k, v] : ch.attrs) if (k == "value") return std::atoi(v.c_str());
    return std::nullopt;
}

int PreKeysToUpload(int server_count) { return server_count < 5 ? 30 : 0; }

}  // namespace openclaw::channels::waweb
