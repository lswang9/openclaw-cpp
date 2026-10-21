#include "openclaw/channels/waweb/signal.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace openclaw::channels::waweb::signal {
namespace {
constexpr std::uint8_t kVersion = 0x33;
constexpr std::uint32_t kMaxSkip = 2000;
Bytes Cat(std::initializer_list<const Bytes*> parts) { Bytes o; for (auto* p : parts) o.insert(o.end(), p->begin(), p->end()); return o; }
std::string Hex(const Bytes& b) { static const char* x = "0123456789abcdef"; std::string o; for (auto c : b) { o += x[c >> 4]; o += x[c & 15]; } return o; }
void Var(Bytes& o, std::uint64_t v) { while (v >= 0x80) { o.push_back(static_cast<std::uint8_t>(v | 0x80)); v >>= 7; } o.push_back(static_cast<std::uint8_t>(v)); }
void FieldB(Bytes& o, int n, const Bytes& v) { Var(o, (std::uint64_t(n) << 3) | 2); Var(o, v.size()); o.insert(o.end(), v.begin(), v.end()); }
void FieldV(Bytes& o, int n, std::uint64_t v) { Var(o, std::uint64_t(n) << 3); Var(o, v); }
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
Bytes Unpub(const Bytes& s) { return (s.size() == 33 && s[0] == 5) ? Bytes(s.begin() + 1, s.end()) : s; }
struct MsgKeys { Bytes cipher, mac, iv; };
MsgKeys Derive(const Bytes& seed) {
    auto k = Hkdf(seed, 80, Bytes(32, 0), "WhisperMessageKeys");
    return {Bytes(k.begin(), k.begin() + 32), Bytes(k.begin() + 32, k.begin() + 64), Bytes(k.begin() + 64, k.end())};
}
Bytes ChainStep(Bytes& ck) {  // returns message-key seed, advances chain
    Bytes one{1}, two{2};
    auto seed = HmacSha256(ck, one);
    ck = HmacSha256(ck, two);
    return seed;
}
void RootStep(Bytes& root, const Bytes& their, const Bytes& our_priv, Bytes& chain_out) {
    auto d = Hkdf(SharedKey(our_priv, their), 64, root, "WhisperRatchet");
    root.assign(d.begin(), d.begin() + 32);
    chain_out.assign(d.begin() + 32, d.end());
}
Bytes Aes(bool enc, const MsgKeys& k, const Bytes& in, bool& ok) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    Bytes out(in.size() + 32); int l = 0, f = 0;
    ok = EVP_CipherInit_ex(c, EVP_aes_256_cbc(), nullptr, k.cipher.data(), k.iv.data(), enc ? 1 : 0) == 1 &&
         EVP_CipherUpdate(c, out.data(), &l, in.data(), static_cast<int>(in.size())) == 1 &&
         EVP_CipherFinal_ex(c, out.data() + l, &f) == 1;
    EVP_CIPHER_CTX_free(c);
    out.resize(ok ? static_cast<std::size_t>(l + f) : 0);
    return out;
}
Bytes Mac(const MsgKeys& k, const Bytes& sender_ik, const Bytes& receiver_ik, const Bytes& ser) {
    auto a = SerializePub(sender_ik), b = SerializePub(receiver_ik);
    auto m = HmacSha256(k.mac, Cat({&a, &b, &ser}));
    m.resize(8);
    return m;
}
Bytes BuildMessage(Session& s, const Bytes& pt) {
    auto seed = ChainStep(s.send_chain);
    auto k = Derive(seed);
    bool ok; auto ct = Aes(true, k, pt, ok);
    Bytes pb; FieldB(pb, 1, SerializePub(s.our_ratchet.pub)); FieldV(pb, 2, s.send_n); FieldV(pb, 3, s.prev_n); FieldB(pb, 4, ct);
    Bytes ser{kVersion}; ser.insert(ser.end(), pb.begin(), pb.end());
    auto mac = Mac(k, s.local_identity, s.remote_identity, ser);
    ser.insert(ser.end(), mac.begin(), mac.end());
    ++s.send_n;
    return ser;
}
}  // namespace

Bytes SerializePub(const Bytes& p) { Bytes o{5}; o.insert(o.end(), p.begin(), p.end()); return o; }

PreKeyBundle Identity::Bundle(std::uint32_t pid) const {
    PreKeyBundle b;
    b.registration_id = registration_id; b.identity_pub = identity.pub; b.signed_prekey_id = signed_prekey_id; b.signed_prekey_pub = signed_prekey.pub;
    if (auto it = prekeys.find(pid); it != prekeys.end()) { b.prekey_id = pid; b.prekey_pub = it->second.pub; }
    return b;
}

Identity NewIdentity(std::uint32_t reg, int n) {
    Identity i; i.identity = GenerateKeyPair(); i.registration_id = reg; i.signed_prekey = GenerateKeyPair();
    for (int k = 1; k <= n; ++k) i.prekeys[static_cast<std::uint32_t>(k)] = GenerateKeyPair();
    return i;
}

std::optional<Session> InitiateOutgoing(const Identity& self, const PreKeyBundle& bob, std::string* err) {
    if (bob.identity_pub.size() != 32 || bob.signed_prekey_pub.size() != 32) { if (err) *err = "bad bundle"; return std::nullopt; }
    if (!bob.signed_prekey_sig.empty() && !VerifyXEdDsa(bob.identity_pub, SerializePub(bob.signed_prekey_pub), bob.signed_prekey_sig)) {
        if (err) *err = "signed prekey signature invalid"; return std::nullopt;
    }
    auto base = GenerateKeyPair();
    Bytes master(32, 0xFF);
    auto d1 = SharedKey(self.identity.priv, bob.signed_prekey_pub), d2 = SharedKey(base.priv, bob.identity_pub), d3 = SharedKey(base.priv, bob.signed_prekey_pub);
    master = Cat({&master, &d1, &d2, &d3});
    if (!bob.prekey_pub.empty()) { auto d4 = SharedKey(base.priv, bob.prekey_pub); master = Cat({&master, &d4}); }
    auto dk = Hkdf(master, 64, Bytes(32, 0), "WhisperText");
    Session s;
    s.local_identity = self.identity.pub; s.remote_identity = bob.identity_pub; s.local_reg = self.registration_id;
    s.root_key.assign(dk.begin(), dk.begin() + 32);
    s.recv_chain.assign(dk.begin() + 32, dk.end());  // libsignal: initial chain becomes receiver chain for their SPK
    s.their_ratchet = bob.signed_prekey_pub;
    s.our_ratchet = GenerateKeyPair();
    RootStep(s.root_key, s.their_ratchet, s.our_ratchet.priv, s.send_chain);
    s.pending_prekey = true; s.pk_reg = self.registration_id; s.pk_prekey_id = bob.prekey_id; s.pk_signed_id = bob.signed_prekey_id; s.pk_base_key = base.pub;
    return s;
}

Envelope Encrypt(Session& s, const Bytes& pt) {
    auto inner = BuildMessage(s, pt);
    if (!s.pending_prekey) return {"msg", inner};
    Bytes pb;
    if (s.pk_prekey_id) FieldV(pb, 1, s.pk_prekey_id);
    FieldB(pb, 2, SerializePub(s.pk_base_key)); FieldB(pb, 3, SerializePub(s.local_identity)); FieldB(pb, 4, inner);
    FieldV(pb, 5, s.pk_reg); FieldV(pb, 6, s.pk_signed_id);
    Bytes out{kVersion}; out.insert(out.end(), pb.begin(), pb.end());
    return {"pkmsg", out};
}

std::optional<Bytes> Decrypt(Session& s, const Bytes& msg, std::string* err) {
    auto fail = [&](const char* e) -> std::optional<Bytes> { if (err) *err = e; return std::nullopt; };
    if (msg.size() < 1 + 8 || msg[0] != kVersion) return fail("bad version");
    Bytes ser(msg.begin(), msg.end() - 8), mac(msg.end() - 8, msg.end());
    auto p = Parse(Bytes(ser.begin() + 1, ser.end()));
    if (!p || !p->b.count(1) || !p->b.count(4) || !p->v.count(2)) return fail("bad message");
    const auto their = Unpub(p->b[1]);
    const auto n = static_cast<std::uint32_t>(p->v[2]);
    const auto prev = static_cast<std::uint32_t>(p->v.count(3) ? p->v[3] : 0);
    Session work = s;  // commit only on success
    Bytes seed;
    const auto key = Hex(their) + ":" + std::to_string(n);
    if (auto it = work.skipped.find(key); it != work.skipped.end()) { seed = it->second; work.skipped.erase(it); }
    else {
        if (their != work.their_ratchet) {
            if (!work.recv_chain.empty()) {  // stash rest of the old chain up to prev
                if (prev > work.recv_n + kMaxSkip) return fail("too many skipped");
                while (work.recv_n < prev) { work.skipped[Hex(work.their_ratchet) + ":" + std::to_string(work.recv_n)] = ChainStep(work.recv_chain); ++work.recv_n; }
            }
            work.their_ratchet = their;
            RootStep(work.root_key, their, work.our_ratchet.priv, work.recv_chain);
            work.recv_n = 0;
            work.prev_n = work.send_n;
            work.our_ratchet = GenerateKeyPair();
            RootStep(work.root_key, their, work.our_ratchet.priv, work.send_chain);
            work.send_n = 0;
        }
        if (n < work.recv_n) return fail("duplicate or old message");
        if (n > work.recv_n + kMaxSkip) return fail("too many skipped");
        while (work.recv_n < n) { work.skipped[Hex(their) + ":" + std::to_string(work.recv_n)] = ChainStep(work.recv_chain); ++work.recv_n; }
        seed = ChainStep(work.recv_chain); ++work.recv_n;
    }
    auto k = Derive(seed);
    auto want = Mac(k, work.remote_identity, work.local_identity, ser);
    if (CRYPTO_memcmp(want.data(), mac.data(), 8) != 0) return fail("bad mac");
    bool ok; auto pt = Aes(false, k, p->b[4], ok);
    if (!ok) return fail("decrypt failed");
    work.pending_prekey = false;  // a decrypted reply confirms the session
    s = std::move(work);
    return pt;
}

std::optional<Bytes> DecryptPreKey(Identity& self, std::optional<Session>& out, const Bytes& pk, std::string* err) {
    auto fail = [&](const char* e) -> std::optional<Bytes> { if (err) *err = e; return std::nullopt; };
    if (pk.size() < 2 || pk[0] != kVersion) return fail("bad version");
    auto p = Parse(Bytes(pk.begin() + 1, pk.end()));
    if (!p || !p->b.count(2) || !p->b.count(3) || !p->b.count(4)) return fail("bad prekey message");
    if (p->v.count(6) && p->v[6] != self.signed_prekey_id) return fail("unknown signed prekey");
    const auto base = Unpub(p->b[2]), alice_ik = Unpub(p->b[3]);
    if (out && out->pk_base_key == base && out->remote_identity == alice_ik) return Decrypt(*out, p->b[4], err);  // repeat pkmsg
    const KeyPair* opk = nullptr;
    std::uint32_t pid = p->v.count(1) ? static_cast<std::uint32_t>(p->v[1]) : 0;
    if (pid) { auto it = self.prekeys.find(pid); if (it == self.prekeys.end()) return fail("unknown or used prekey"); opk = &it->second; }
    Bytes master(32, 0xFF);
    auto d1 = SharedKey(self.signed_prekey.priv, alice_ik), d2 = SharedKey(self.identity.priv, base), d3 = SharedKey(self.signed_prekey.priv, base);
    master = Cat({&master, &d1, &d2, &d3});
    if (opk) { auto d4 = SharedKey(opk->priv, base); master = Cat({&master, &d4}); }
    auto dk = Hkdf(master, 64, Bytes(32, 0), "WhisperText");
    Session s;
    s.local_identity = self.identity.pub; s.remote_identity = alice_ik; s.local_reg = self.registration_id;
    s.root_key.assign(dk.begin(), dk.begin() + 32);
    s.send_chain.assign(dk.begin() + 32, dk.end());  // bob's initial sending chain on his SPK ratchet
    s.our_ratchet = self.signed_prekey;
    s.pk_base_key = base;
    auto pt = Decrypt(s, p->b[4], err);
    if (!pt) return std::nullopt;
    if (pid) self.prekeys.erase(pid);  // one-time prekey consumed
    out = std::move(s);
    return pt;
}

}  // namespace openclaw::channels::waweb::signal
