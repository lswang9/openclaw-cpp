#include <cstdlib>
#include <iterator>
#include <unordered_map>
#include <openssl/bn.h>
#include "openclaw/channels/waweb/noise.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <stdexcept>

namespace openclaw::channels::waweb {

Bytes FromString(const std::string& s) { return Bytes(s.begin(), s.end()); }

Bytes RandomBytes(std::size_t n) {
    Bytes b(n);
    if (n && RAND_bytes(b.data(), static_cast<int>(n)) != 1) throw std::runtime_error("RAND_bytes failed");
    return b;
}

KeyPair GenerateKeyPair() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    EVP_PKEY* k = nullptr;
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_keygen(ctx, &k);
    EVP_PKEY_CTX_free(ctx);
    KeyPair kp{Bytes(32), Bytes(32)};
    size_t l = 32;
    EVP_PKEY_get_raw_public_key(k, kp.pub.data(), &l);
    l = 32;
    EVP_PKEY_get_raw_private_key(k, kp.priv.data(), &l);
    EVP_PKEY_free(k);
    return kp;
}

Bytes SharedKey(const Bytes& priv, const Bytes& peer_pub) {
    EVP_PKEY* a = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size());
    EVP_PKEY* b = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_pub.data(), peer_pub.size());
    if (!a || !b) { EVP_PKEY_free(a); EVP_PKEY_free(b); throw std::runtime_error("bad x25519 key"); }
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(a, nullptr);
    Bytes out(32);
    size_t l = 32;
    bool ok = EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, b) == 1 && EVP_PKEY_derive(ctx, out.data(), &l) == 1;
    EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(a); EVP_PKEY_free(b);
    if (!ok) throw std::runtime_error("x25519 derive failed");
    return out;
}

Bytes Sha256(const Bytes& d) {
    Bytes o(32);
    unsigned int l = 32;
    EVP_Digest(d.data(), d.size(), o.data(), &l, EVP_sha256(), nullptr);
    return o;
}

Bytes HmacSha256(const Bytes& key, const Bytes& data) {
    Bytes o(32);
    unsigned int l = 32;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data.data(), data.size(), o.data(), &l);
    return o;
}

Bytes Hkdf(const Bytes& ikm, std::size_t len, const Bytes& salt, const std::string& info) {
    // RFC 5869 by hand (extract + expand) so empty salt/info behave exactly like libsignal/Baileys.
    Bytes prk = HmacSha256(salt.empty() ? Bytes(32, 0) : salt, ikm);
    Bytes out, t;
    for (std::uint8_t i = 1; out.size() < len; ++i) {
        Bytes in = t;
        in.insert(in.end(), info.begin(), info.end());
        in.push_back(i);
        t = HmacSha256(prk, in);
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(len);
    return out;
}

std::string Base64(const Bytes& b) {
    std::string o(4 * ((b.size() + 2) / 3) + 1, '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(o.data()), b.data(), static_cast<int>(b.size()));
    o.resize(n);
    return o;
}

namespace {
Bytes Iv(std::uint32_t c) {
    Bytes iv(12, 0);
    iv[8] = c >> 24; iv[9] = (c >> 16) & 0xff; iv[10] = (c >> 8) & 0xff; iv[11] = c & 0xff;
    return iv;
}
Bytes GcmEncrypt(const Bytes& key, const Bytes& iv, const Bytes& aad, const Bytes& pt) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    Bytes out(pt.size() + 16);
    int l = 0, t = 0;
    EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_EncryptInit_ex(c, nullptr, nullptr, key.data(), iv.data());
    if (!aad.empty()) EVP_EncryptUpdate(c, nullptr, &l, aad.data(), static_cast<int>(aad.size()));
    EVP_EncryptUpdate(c, out.data(), &l, pt.data(), static_cast<int>(pt.size()));
    EVP_EncryptFinal_ex(c, out.data() + l, &t);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out.data() + pt.size());
    EVP_CIPHER_CTX_free(c);
    return out;
}
std::optional<Bytes> GcmDecrypt(const Bytes& key, const Bytes& iv, const Bytes& aad, const Bytes& ct) {
    if (ct.size() < 16) return std::nullopt;
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    const std::size_t n = ct.size() - 16;
    Bytes out(n + 16);
    int l = 0, t = 0;
    EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_DecryptInit_ex(c, nullptr, nullptr, key.data(), iv.data());
    if (!aad.empty()) EVP_DecryptUpdate(c, nullptr, &l, aad.data(), static_cast<int>(aad.size()));
    EVP_DecryptUpdate(c, out.data(), &l, ct.data(), static_cast<int>(n));
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, const_cast<std::uint8_t*>(ct.data() + n));
    int ok = EVP_DecryptFinal_ex(c, out.data() + l, &t);
    EVP_CIPHER_CTX_free(c);
    if (ok != 1) return std::nullopt;
    out.resize(n);
    return out;
}
}  // namespace

NoiseState::NoiseState(const Bytes& prologue) {
    Bytes mode(kNoiseMode, kNoiseMode + 32);
    hash_ = mode;  // exactly 32 bytes → used as-is
    salt_ = mode;
    enc_key_ = dec_key_ = mode;
    Authenticate(prologue);
}

void NoiseState::Authenticate(const Bytes& data) {
    if (finished_) return;
    Bytes in = hash_;
    in.insert(in.end(), data.begin(), data.end());
    hash_ = Sha256(in);
}

void NoiseState::MixIntoKey(const Bytes& ikm) {
    auto k = Hkdf(ikm, 64, salt_);
    salt_.assign(k.begin(), k.begin() + 32);
    enc_key_.assign(k.begin() + 32, k.end());
    dec_key_ = enc_key_;
    read_counter_ = write_counter_ = 0;
}

Bytes NoiseState::Encrypt(const Bytes& pt) {
    auto ct = GcmEncrypt(enc_key_, Iv(write_counter_++), finished_ ? Bytes{} : hash_, pt);
    Authenticate(ct);
    return ct;
}

std::optional<Bytes> NoiseState::Decrypt(const Bytes& ct) {
    // During the handshake both directions share one counter (single CipherState), like Baileys.
    std::uint32_t& ctr = finished_ ? read_counter_ : write_counter_;
    auto pt = GcmDecrypt(dec_key_, Iv(ctr++), finished_ ? Bytes{} : hash_, ct);
    if (!pt) return std::nullopt;
    Authenticate(ct);
    return pt;
}

void NoiseState::FinishInit() {
    auto k = Hkdf(Bytes{}, 64, salt_);
    Bytes a(k.begin(), k.begin() + 32), b(k.begin() + 32, k.end());
    enc_key_ = initiator ? a : b;
    dec_key_ = initiator ? b : a;
    hash_.clear();
    read_counter_ = write_counter_ = 0;
    finished_ = true;
}

Bytes NoiseState::EncodeFrame(const Bytes& payload) {
    Bytes data = finished_ ? Encrypt(payload) : payload;
    Bytes out;
    if (!sent_intro_ && initiator) { out = kWaHeader; sent_intro_ = true; }
    if (data.size() >= (1u << 24)) throw std::runtime_error("frame too large");
    out.push_back((data.size() >> 16) & 0xff);
    out.push_back((data.size() >> 8) & 0xff);
    out.push_back(data.size() & 0xff);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<Bytes> NoiseState::DecodeFrames(const Bytes& chunk) {
    inbuf_.insert(inbuf_.end(), chunk.begin(), chunk.end());
    std::vector<Bytes> out;
    if (!initiator && !sent_intro_) {  // responder: strip the client's intro header once
        if (inbuf_.size() < kWaHeader.size()) return out;
        if (!std::equal(kWaHeader.begin(), kWaHeader.end(), inbuf_.begin())) throw std::runtime_error("bad WA intro header");
        inbuf_.erase(inbuf_.begin(), inbuf_.begin() + kWaHeader.size());
        sent_intro_ = true;
    }
    while (inbuf_.size() >= 3) {
        std::size_t n = (std::size_t(inbuf_[0]) << 16) | (std::size_t(inbuf_[1]) << 8) | inbuf_[2];
        if (inbuf_.size() < 3 + n) break;
        Bytes f(inbuf_.begin() + 3, inbuf_.begin() + 3 + n);
        inbuf_.erase(inbuf_.begin(), inbuf_.begin() + 3 + n);
        if (finished_) {
            auto pt = Decrypt(f);
            if (!pt) throw std::runtime_error("frame decrypt failed");
            out.push_back(std::move(*pt));
        } else out.push_back(std::move(f));
    }
    return out;
}

// ---- protobuf (varint + length-delimited only) ----
namespace {
void Varint(Bytes& o, std::uint64_t v) { while (v >= 0x80) { o.push_back(static_cast<std::uint8_t>(v | 0x80)); v >>= 7; } o.push_back(static_cast<std::uint8_t>(v)); }
void Field(Bytes& o, int num, const Bytes& v) { Varint(o, (std::uint64_t(num) << 3) | 2); Varint(o, v.size()); o.insert(o.end(), v.begin(), v.end()); }
bool ReadVarint(const Bytes& b, std::size_t& i, std::uint64_t& v) {
    v = 0;
    for (int s = 0; i < b.size() && s < 64; s += 7) { auto c = b[i++]; v |= std::uint64_t(c & 0x7f) << s; if (!(c & 0x80)) return true; }
    return false;
}
// Iterate length-delimited fields; skips varint/fixed fields.
bool Fields(const Bytes& b, std::vector<std::pair<int, Bytes>>& out) {
    std::size_t i = 0;
    while (i < b.size()) {
        std::uint64_t key, len;
        if (!ReadVarint(b, i, key)) return false;
        int wt = key & 7, num = static_cast<int>(key >> 3);
        if (wt == 2) {
            if (!ReadVarint(b, i, len) || i + len > b.size()) return false;
            out.emplace_back(num, Bytes(b.begin() + i, b.begin() + i + len));
            i += len;
        } else if (wt == 0) { if (!ReadVarint(b, i, len)) return false; }
        else if (wt == 1) i += 8;
        else if (wt == 5) i += 4;
        else return false;
    }
    return i == b.size();
}
}  // namespace

Bytes EncodeHandshake(int which, const HelloParts& p) {
    Bytes inner;
    if (!p.ephemeral.empty()) Field(inner, 1, p.ephemeral);
    if (!p.static_.empty()) Field(inner, 2, p.static_);
    if (!p.payload.empty()) Field(inner, 3, p.payload);
    Bytes out;
    Field(out, which, inner);
    return out;
}

std::optional<std::pair<int, HelloParts>> DecodeHandshake(const Bytes& b) {
    std::vector<std::pair<int, Bytes>> top;
    if (!Fields(b, top) || top.empty()) return std::nullopt;
    for (auto& [num, inner] : top) {
        if (num < 2 || num > 4) continue;
        std::vector<std::pair<int, Bytes>> f;
        if (!Fields(inner, f)) return std::nullopt;
        HelloParts p;
        for (auto& [n, v] : f) { if (n == 1) p.ephemeral = v; else if (n == 2) p.static_ = v; else if (n == 3) p.payload = v; }
        return std::make_pair(num, p);
    }
    return std::nullopt;
}

// ---- XX roles ----
Bytes ClientHandshake::ClientHello() {
    noise.initiator = true;
    ephemeral = GenerateKeyPair();
    if (static_.pub.empty()) static_ = GenerateKeyPair();
    noise.Authenticate(ephemeral.pub);
    return EncodeHandshake(2, {ephemeral.pub, {}, {}});
}

std::optional<Bytes> ClientHandshake::OnServerHello(const Bytes& msg, const Bytes& client_payload, Bytes* srv_static, Bytes* cert) {
    auto d = DecodeHandshake(msg);
    if (!d || d->first != 3 || d->second.ephemeral.size() != 32) return std::nullopt;
    const auto& sh = d->second;
    noise.Authenticate(sh.ephemeral);
    noise.MixIntoKey(SharedKey(ephemeral.priv, sh.ephemeral));            // ee
    auto s = noise.Decrypt(sh.static_);
    if (!s || s->size() != 32) return std::nullopt;
    noise.MixIntoKey(SharedKey(ephemeral.priv, *s));                       // es
    auto c = noise.Decrypt(sh.payload);
    if (!c) return std::nullopt;
    if (srv_static) *srv_static = *s;
    if (cert) *cert = *c;
    auto enc_static = noise.Encrypt(static_.pub);
    noise.MixIntoKey(SharedKey(static_.priv, sh.ephemeral));               // se
    auto enc_payload = noise.Encrypt(client_payload);
    // Caller frames/sends ClientFinish, then calls noise.FinishInit() (Baileys order).
    return EncodeHandshake(4, {{}, enc_static, enc_payload});
}

std::optional<Bytes> ServerHandshake::OnClientHello(const Bytes& msg, const Bytes& cert_payload) {
    noise.initiator = false;
    auto d = DecodeHandshake(msg);
    if (!d || d->first != 2 || d->second.ephemeral.size() != 32) return std::nullopt;
    client_ephemeral = d->second.ephemeral;
    noise.Authenticate(client_ephemeral);
    ephemeral = GenerateKeyPair();
    if (static_.pub.empty()) static_ = GenerateKeyPair();
    noise.Authenticate(ephemeral.pub);
    noise.MixIntoKey(SharedKey(ephemeral.priv, client_ephemeral));         // ee
    auto enc_static = noise.Encrypt(static_.pub);
    noise.MixIntoKey(SharedKey(static_.priv, client_ephemeral));           // es
    auto enc_cert = noise.Encrypt(cert_payload);
    return EncodeHandshake(3, {ephemeral.pub, enc_static, enc_cert});
}

std::optional<Bytes> ServerHandshake::OnClientFinish(const Bytes& msg, Bytes* client_static) {
    auto d = DecodeHandshake(msg);
    if (!d || d->first != 4) return std::nullopt;
    auto s = noise.Decrypt(d->second.static_);
    if (!s || s->size() != 32) return std::nullopt;
    noise.MixIntoKey(SharedKey(ephemeral.priv, *s));                       // se
    auto p = noise.Decrypt(d->second.payload);
    if (!p) return std::nullopt;
    if (client_static) *client_static = *s;
    noise.FinishInit();
    return p;
}

// ---- media ----
MediaKeys ExpandMediaKey(const Bytes& media_key, MediaType t) {
    const char* info = t == MediaType::Video ? "WhatsApp Video Keys" : t == MediaType::Audio ? "WhatsApp Audio Keys"
                     : t == MediaType::Document ? "WhatsApp Document Keys" : "WhatsApp Image Keys";
    auto k = Hkdf(media_key, 112, {}, info);
    return {Bytes(k.begin(), k.begin() + 16), Bytes(k.begin() + 16, k.begin() + 48), Bytes(k.begin() + 48, k.begin() + 80),
            Bytes(k.begin() + 80, k.end())};
}

Bytes EncryptMedia(const Bytes& plain, const Bytes& media_key, MediaType t) {
    auto k = ExpandMediaKey(media_key, t);
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    Bytes enc(plain.size() + 16);
    int l = 0, f = 0;
    EVP_EncryptInit_ex(c, EVP_aes_256_cbc(), nullptr, k.cipher_key.data(), k.iv.data());
    EVP_EncryptUpdate(c, enc.data(), &l, plain.data(), static_cast<int>(plain.size()));
    EVP_EncryptFinal_ex(c, enc.data() + l, &f);
    EVP_CIPHER_CTX_free(c);
    enc.resize(l + f);
    Bytes macin = k.iv;
    macin.insert(macin.end(), enc.begin(), enc.end());
    auto mac = HmacSha256(k.mac_key, macin);
    enc.insert(enc.end(), mac.begin(), mac.begin() + 10);
    return enc;
}

std::optional<Bytes> DecryptMedia(const Bytes& file, const Bytes& media_key, MediaType t) {
    if (file.size() < 10 + 16) return std::nullopt;
    auto k = ExpandMediaKey(media_key, t);
    Bytes enc(file.begin(), file.end() - 10), mac(file.end() - 10, file.end());
    Bytes macin = k.iv;
    macin.insert(macin.end(), enc.begin(), enc.end());
    auto want = HmacSha256(k.mac_key, macin);
    if (CRYPTO_memcmp(want.data(), mac.data(), 10) != 0) return std::nullopt;
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    Bytes out(enc.size() + 16);
    int l = 0, f = 0;
    EVP_DecryptInit_ex(c, EVP_aes_256_cbc(), nullptr, k.cipher_key.data(), k.iv.data());
    EVP_DecryptUpdate(c, out.data(), &l, enc.data(), static_cast<int>(enc.size()));
    int ok = EVP_DecryptFinal_ex(c, out.data() + l, &f);
    EVP_CIPHER_CTX_free(c);
    if (ok != 1) return std::nullopt;
    out.resize(l + f);
    return out;
}

std::string QrPayload(const std::string& ref, const Bytes& noise_pub, const Bytes& identity_pub, const Bytes& adv_secret) {
    return ref + "," + Base64(noise_pub) + "," + Base64(identity_pub) + "," + Base64(adv_secret);
}


// ---- cert / binary / signal stubs ----

std::optional<CertChainInfo> DecodeCertChain(const Bytes& payload) {
    std::vector<std::pair<int, Bytes>> top;
    if (!Fields(payload, top)) return std::nullopt;
    CertChainInfo info;
    auto take = [&](const Bytes& cert, Bytes& details, Bytes& sig) {
        std::vector<std::pair<int, Bytes>> f;
        if (!Fields(cert, f)) return false;
        for (auto& [n, v] : f) { if (n == 1) details = v; else if (n == 2) sig = v; }
        return !details.empty() && !sig.empty();
    };
    for (auto& [n, v] : top) {
        if (n == 1) { if (!take(v, info.leaf_details, info.leaf_signature)) return std::nullopt; }
        else if (n == 2) { if (!take(v, info.intermediate_details, info.intermediate_signature)) return std::nullopt; }
    }
    info.structurally_ok = !info.leaf_details.empty() && !info.leaf_signature.empty();
    return info;
}

namespace {
// ---- XEdDSA verification (Signal curve25519_verify): Montgomery u → Edwards y=(u-1)/(u+1), sign bit from sig[63] ----
bool XEdDsaVerify(const Bytes& mont_pub, const Bytes& msg, const Bytes& sig) {
    if (mont_pub.size() != 32 || sig.size() != 64) return false;
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM *p = BN_new(), *u = nullptr, *one = BN_new(), *num = BN_new(), *den = BN_new(), *inv = nullptr, *y = BN_new();
    BN_set_word(p, 1); BN_lshift(p, p, 255); BN_sub_word(p, 19);
    Bytes ub = mont_pub; ub[31] &= 0x7F;
    u = BN_lebin2bn(ub.data(), 32, nullptr);
    BN_one(one);
    bool ok = BN_cmp(u, p) < 0;
    if (ok) {
        BN_mod_sub(num, u, one, p, ctx);
        BN_mod_add(den, u, one, p, ctx);
        inv = BN_mod_inverse(nullptr, den, p, ctx);
        ok = inv != nullptr && BN_mod_mul(y, num, inv, p, ctx) == 1;
    }
    Bytes ed(32), s2 = sig;
    if (ok) {
        BN_bn2lebinpad(y, ed.data(), 32);
        ed[31] = static_cast<std::uint8_t>((ed[31] & 0x7F) | (sig[63] & 0x80));
        s2[63] &= 0x7F;
    }
    BN_free(p); BN_free(u); BN_free(one); BN_free(num); BN_free(den); BN_free(inv); BN_free(y); BN_CTX_free(ctx);
    if (!ok) return false;
    EVP_PKEY* k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, ed.data(), 32);
    if (!k) return false;
    EVP_MD_CTX* m = EVP_MD_CTX_new();
    bool v = EVP_DigestVerifyInit(m, nullptr, nullptr, nullptr, k) == 1 &&
             EVP_DigestVerify(m, s2.data(), s2.size(), msg.data(), msg.size()) == 1;
    EVP_MD_CTX_free(m);
    EVP_PKEY_free(k);
    return v;
}

}  // namespace

const Bytes& WaRootPublicKey() {
    // WhatsApp noise root (issuer serial 0); same constant as whatsmeow handshake.go WACertPubKey.
    static const Bytes k{0x14, 0x23, 0x75, 0x57, 0x4d, 0x0a, 0x58, 0x71, 0x66, 0xaa, 0xe7, 0x1e, 0xbe, 0x51, 0x64, 0x37,
                         0xc4, 0xa2, 0x8b, 0x73, 0xe3, 0x69, 0x5c, 0x6c, 0xe1, 0xf7, 0xf9, 0x54, 0x5d, 0xa8, 0xee, 0x6b};
    return k;
}

bool VerifyXEdDsa(const Bytes& mont_pub, const Bytes& msg, const Bytes& sig) { return XEdDsaVerify(mont_pub, msg, sig); }

std::optional<CertDetails> DecodeCertDetails(const Bytes& raw) {
    // CertChain.NoiseCertificate.Details {serial=1 u32, issuerSerial=2 u32, key=3 bytes, notBefore=4 u64, notAfter=5 u64}
    CertDetails d;
    std::size_t i = 0;
    auto varint = [&](std::uint64_t& v) {
        v = 0;
        for (int sh = 0; i < raw.size() && sh < 64; sh += 7) {
            std::uint8_t c = raw[i++];
            v |= std::uint64_t(c & 0x7F) << sh;
            if (!(c & 0x80)) return true;
        }
        return false;
    };
    while (i < raw.size()) {
        std::uint64_t key;
        if (!varint(key)) return std::nullopt;
        const int field = static_cast<int>(key >> 3), wt = static_cast<int>(key & 7);
        if (wt == 0) {
            std::uint64_t v;
            if (!varint(v)) return std::nullopt;
            if (field == 1) d.serial = static_cast<std::uint32_t>(v);
            else if (field == 2) d.issuer_serial = static_cast<std::uint32_t>(v);
            else if (field == 4) d.not_before = v;
            else if (field == 5) d.not_after = v;
        } else if (wt == 2) {
            std::uint64_t n;
            if (!varint(n) || i + n > raw.size()) return std::nullopt;
            if (field == 3) d.key.assign(raw.begin() + static_cast<std::ptrdiff_t>(i), raw.begin() + static_cast<std::ptrdiff_t>(i + n));
            i += n;
        } else return std::nullopt;
    }
    return d;
}

CertChainInfo VerifyCertChain(const Bytes& payload, const Bytes& wa_root_pub, const Bytes& server_static, std::uint64_t now_s) {
    CertChainInfo info;
    auto d = DecodeCertChain(payload);
    if (!d) { info.error = "undecodable CertChain"; return info; }
    info = *d;
    const Bytes& root = wa_root_pub.empty() ? WaRootPublicKey() : wa_root_pub;
    auto fail = [&](const std::string& e) { info.signature_checked = true; info.signature_valid = false; info.error = e; return info; };
    if (info.intermediate_details.empty() || info.intermediate_signature.size() != 64 || info.leaf_signature.size() != 64)
        return fail("missing parts of noise certificate");
    if (!XEdDsaVerify(root, info.intermediate_details, info.intermediate_signature)) return fail("intermediate signature invalid");
    auto im = DecodeCertDetails(info.intermediate_details);
    if (!im || im->issuer_serial != 0 || im->key.size() != 32) return fail("bad intermediate details");
    if (!XEdDsaVerify(im->key, info.leaf_details, info.leaf_signature)) return fail("leaf signature invalid");
    auto lf = DecodeCertDetails(info.leaf_details);
    if (!lf || lf->issuer_serial != im->serial) return fail("leaf issuer serial mismatch");
    if (!server_static.empty() && lf->key != server_static) return fail("cert key does not match server static");
    if (now_s) {
        for (auto* c : {&*im, &*lf})
            if ((c->not_before && now_s < c->not_before) || (c->not_after && now_s > c->not_after)) return fail("certificate not within validity");
    }
    info.signature_checked = true;
    info.signature_valid = true;
    info.intermediate = *im;
    info.leaf = *lf;
    return info;
}

// ---- WABinary (Baileys-compatible, token dictionary v3) ----
namespace {
#include "openclaw/channels/waweb/wa_tokens.inc"
enum : std::uint8_t {
    LIST_EMPTY = 0, DICTIONARY_0 = 236, INTEROP_JID = 245, FB_JID = 246, AD_JID = 247, LIST_8 = 248, LIST_16 = 249,
    JID_PAIR = 250, HEX_8 = 251, BINARY_8 = 252, BINARY_20 = 253, BINARY_32 = 254, NIBBLE_8 = 255
};
struct TokenRef { int dict; std::uint8_t index; };
const std::unordered_map<std::string, TokenRef>& TokenMap() {
    static const auto m = [] {
        std::unordered_map<std::string, TokenRef> t;
        for (std::size_t i = 0; i < std::size(kSingleByteTokens); ++i) t[kSingleByteTokens[i]] = {-1, static_cast<std::uint8_t>(i)};
        for (int d = 0; d < 4; ++d)
            for (std::size_t j = 0; j < kDoubleByteTokenCounts[d]; ++j) t[kDoubleByteTokens[d][j]] = {d, static_cast<std::uint8_t>(j)};
        return t;
    }();
    return m;
}
struct Enc {
    Bytes& o;
    void Byte(std::uint32_t v) { o.push_back(static_cast<std::uint8_t>(v & 0xff)); }
    void Len(std::size_t n) {
        if (n >= (1u << 20)) { Byte(BINARY_32); for (int s = 24; s >= 0; s -= 8) Byte(static_cast<std::uint32_t>(n >> s)); }
        else if (n >= 256) { Byte(BINARY_20); Byte((n >> 16) & 0x0f); Byte(n >> 8); Byte(n); }
        else { Byte(BINARY_8); Byte(static_cast<std::uint32_t>(n)); }
    }
    void Raw(const std::string& s) { Len(s.size()); o.insert(o.end(), s.begin(), s.end()); }
    static bool IsNibble(const std::string& s) {
        if (s.empty() || s.size() > 127) return false;
        for (char c : s) if (!((c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
        return true;
    }
    static bool IsHex(const std::string& s) {
        if (s.empty() || s.size() > 127) return false;
        for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
        return true;
    }
    static int PackN(char c) { return c == '-' ? 10 : c == '.' ? 11 : c == 0 ? 15 : c - '0'; }
    static int PackH(char c) { return c == 0 ? 15 : (c >= 'A' ? 10 + c - 'A' : c - '0'); }
    void Packed(const std::string& s, bool nibble) {
        Byte(nibble ? NIBBLE_8 : HEX_8);
        std::uint32_t rl = static_cast<std::uint32_t>((s.size() + 1) / 2);
        if (s.size() % 2) rl |= 128;
        Byte(rl);
        auto pk = [&](char a, char b) { return nibble ? (PackN(a) << 4) | PackN(b) : (PackH(a) << 4) | PackH(b); };
        for (std::size_t i = 0; i + 1 < s.size(); i += 2) Byte(static_cast<std::uint32_t>(pk(s[i], s[i + 1])));
        if (s.size() % 2) Byte(static_cast<std::uint32_t>(pk(s.back(), 0)));
    }
    void Str(const std::string& s) {
        if (s.empty()) { Raw(s); return; }
        auto& tm = TokenMap();
        if (auto it = tm.find(s); it != tm.end()) {
            if (it->second.dict >= 0) Byte(DICTIONARY_0 + it->second.dict);
            Byte(it->second.index);
            return;
        }
        if (IsNibble(s)) { Packed(s, true); return; }
        if (IsHex(s)) { Packed(s, false); return; }
        if (auto at = s.find('@'); at != std::string::npos) {
            std::string server = s.substr(at + 1), uc = s.substr(0, at), user = uc, device;
            if (auto c = uc.find(':'); c != std::string::npos) { user = uc.substr(0, c); device = uc.substr(c + 1); if (auto c2 = device.find(':'); c2 != std::string::npos) device.resize(c2); }
            std::string agent;
            if (auto u = user.find('_'); u != std::string::npos) { agent = user.substr(u + 1); user.resize(u); }
            int domain = server == "lid" ? 1 : server == "hosted" ? 128 : server == "hosted.lid" ? 129 : (agent.empty() ? 0 : std::atoi(agent.c_str()));
            if (!device.empty()) {
                Byte(AD_JID); Byte(static_cast<std::uint32_t>(domain)); Byte(static_cast<std::uint32_t>(std::atoi(device.c_str())));
                Str(user);
            } else {
                Byte(JID_PAIR);
                if (user.empty()) Byte(LIST_EMPTY); else Str(user);
                Str(server);
            }
            return;
        }
        Raw(s);
    }
    void ListStart(std::size_t n) {
        if (n == 0) Byte(LIST_EMPTY);
        else if (n < 256) { Byte(LIST_8); Byte(static_cast<std::uint32_t>(n)); }
        else { Byte(LIST_16); Byte(static_cast<std::uint32_t>(n >> 8)); Byte(static_cast<std::uint32_t>(n)); }
    }
    void Node(const BinaryNode& n) {
        const bool has_content = !n.children.empty() || n.has_content;
        ListStart(2 * n.attrs.size() + 1 + (has_content ? 1 : 0));
        Str(n.tag);
        for (auto& [k, v] : n.attrs) { Str(k); Str(v); }
        if (!n.children.empty()) { ListStart(n.children.size()); for (auto& c : n.children) Node(c); }
        else if (n.has_content) {
            if (n.content_is_string) Str(std::string(n.content.begin(), n.content.end()));
            else { Len(n.content.size()); o.insert(o.end(), n.content.begin(), n.content.end()); }
        }
    }
};
struct Dec {
    const Bytes& b;
    std::size_t i = 0;
    std::string err;
    bool Need(std::size_t n) { if (i + n > b.size()) { err = "end of stream"; return false; } return true; }
    std::uint32_t Byte() { return Need(1) ? b[i++] : 0; }
    std::uint32_t Int(int n) { std::uint32_t v = 0; if (!Need(n)) return 0; for (int k = 0; k < n; ++k) v = (v << 8) | b[i++]; return v; }
    std::string Chars(std::size_t n) { if (!Need(n)) return {}; std::string s(b.begin() + static_cast<std::ptrdiff_t>(i), b.begin() + static_cast<std::ptrdiff_t>(i + n)); i += n; return s; }
    std::string Packed(std::uint32_t tag) {
        std::uint32_t start = Byte();
        std::string v;
        for (std::uint32_t k = 0; k < (start & 127) && err.empty(); ++k) {
            std::uint32_t c = Byte();
            for (std::uint32_t nb : {(c & 0xf0) >> 4, c & 0x0f}) {
                if (tag == NIBBLE_8) {
                    if (nb <= 9) v += static_cast<char>('0' + nb);
                    else if (nb == 10) v += '-';
                    else if (nb == 11) v += '.';
                    else if (nb == 15) v += '\0';
                    else { err = "invalid nibble"; return v; }
                } else v += static_cast<char>(nb < 10 ? '0' + nb : 'A' + nb - 10);
            }
        }
        if (start >> 7) v.pop_back();
        return v;
    }
    std::string Str(std::uint32_t tag) {
        if (tag >= 1 && tag < std::size(kSingleByteTokens)) return kSingleByteTokens[tag];
        if (tag >= DICTIONARY_0 && tag <= DICTIONARY_0 + 3) {
            std::uint32_t idx = Byte(), d = tag - DICTIONARY_0;
            if (idx >= kDoubleByteTokenCounts[d]) { err = "invalid double token"; return {}; }
            return kDoubleByteTokens[d][idx];
        }
        switch (tag) {
            case LIST_EMPTY: return {};
            case BINARY_8: return Chars(Byte());
            case BINARY_20: { std::uint32_t n = ((Byte() & 15) << 16); n += Byte() << 8; n += Byte(); return Chars(n); }
            case BINARY_32: return Chars(Int(4));
            case JID_PAIR: {
                std::string u = Str(Byte()), s = Str(Byte());
                if (s.empty()) { err = "invalid jid pair"; return {}; }
                return u + "@" + s;
            }
            case AD_JID: {
                std::uint32_t domain = Byte(), device = Byte();
                std::string user = Str(Byte());
                std::string server = domain == 1 ? "lid" : domain == 128 ? "hosted" : domain == 129 ? "hosted.lid" : "s.whatsapp.net";
                return user + (device ? ":" + std::to_string(device) : std::string()) + "@" + server;
            }
            case FB_JID: { std::string u = Str(Byte()); std::uint32_t dev = Int(2); std::string s = Str(Byte()); return u + ":" + std::to_string(dev) + "@" + s; }
            case INTEROP_JID: {
                std::string u = Str(Byte()); std::uint32_t dev = Int(2), integ = Int(2);
                std::string s = "interop";
                if (i < b.size()) { auto save = i; auto e = err; s = Str(Byte()); if (!err.empty()) { i = save; err = e; s = "interop"; } }
                return std::to_string(integ) + "-" + u + ":" + std::to_string(dev) + "@" + s;
            }
            case HEX_8: case NIBBLE_8: return Packed(tag);
            default: err = "invalid string tag " + std::to_string(tag); return {};
        }
    }
    std::size_t ListSize(std::uint32_t tag) {
        if (tag == LIST_EMPTY) return 0;
        if (tag == LIST_8) return Byte();
        if (tag == LIST_16) return Int(2);
        err = "invalid list tag " + std::to_string(tag);
        return 0;
    }
    std::optional<BinaryNode> Node(int depth = 0) {
        if (depth > 64) { err = "too deep"; return std::nullopt; }
        std::size_t size = ListSize(Byte());
        if (!err.empty() || size == 0) { if (err.empty()) err = "invalid node"; return std::nullopt; }
        BinaryNode n;
        n.tag = Str(Byte());
        if (n.tag.empty() || !err.empty()) { if (err.empty()) err = "invalid node"; return std::nullopt; }
        for (std::size_t a = 0; a < (size - 1) / 2 && err.empty(); ++a) {
            std::string k = Str(Byte());
            std::string v = Str(Byte());
            n.attrs.emplace_back(k, v);
        }
        if (size % 2 == 0 && err.empty()) {
            std::uint32_t tag = Byte();
            if (tag == LIST_EMPTY || tag == LIST_8 || tag == LIST_16) {
                std::size_t cnt = ListSize(tag);
                for (std::size_t c = 0; c < cnt && err.empty(); ++c) {
                    auto ch = Node(depth + 1);
                    if (!ch) return std::nullopt;
                    n.children.push_back(std::move(*ch));
                }
            } else {
                n.has_content = true;
                std::string s;
                if (tag == BINARY_8) s = Chars(Byte());
                else if (tag == BINARY_20) { std::uint32_t l = ((Byte() & 15) << 16); l += Byte() << 8; l += Byte(); s = Chars(l); }
                else if (tag == BINARY_32) s = Chars(Int(4));
                else { s = Str(tag); n.content_is_string = true; }
                n.content.assign(s.begin(), s.end());
            }
        }
        if (!err.empty()) return std::nullopt;
        return n;
    }
};
}  // namespace

Bytes EncodeBinaryNode(const BinaryNode& n, bool with_frame_flag) {
    Bytes o;
    if (with_frame_flag) o.push_back(0);
    Enc e{o};
    e.Node(n);
    return o;
}

std::optional<BinaryNode> DecodeBinaryNode(const Bytes& b, bool with_frame_flag, std::string* error) {
    if (with_frame_flag) {
        if (b.empty()) { if (error) *error = "empty"; return std::nullopt; }
        if (b[0] & 2) { if (error) *error = "zlib-compressed frame not supported"; return std::nullopt; }
        Bytes rest(b.begin() + 1, b.end());
        return DecodeBinaryNode(rest, false, error);
    }
    Dec d{b};
    auto n = d.Node();
    if (!n && error) *error = d.err;
    return n;
}

SignalIdentity GenerateSignalIdentity() {
    SignalIdentity id;
    id.identity = GenerateKeyPair();
    id.signed_prekey = GenerateKeyPair();
    id.registration_id = (RandomBytes(4)[0] << 24) | (RandomBytes(1)[0] << 16) | (RandomBytes(1)[0] << 8) | RandomBytes(1)[0];
    id.registration_id &= 0x3fff;
    // spk_signature left empty — Ed25519/XEdDSA not linked in this stub layer.
    return id;
}

SignalSession SignalX3dhInitiate(const SignalIdentity& self, const Bytes& peer_ik, const Bytes& peer_spk,
                                 const Bytes& peer_opk) {
    SignalSession s;
    s.peer_identity = peer_ik;
    auto ek = GenerateKeyPair();
    Bytes ikm = SharedKey(self.identity.priv, peer_spk);
    auto a = SharedKey(ek.priv, peer_ik);
    auto b = SharedKey(ek.priv, peer_spk);
    ikm.insert(ikm.end(), a.begin(), a.end());
    ikm.insert(ikm.end(), b.begin(), b.end());
    if (!peer_opk.empty()) {
        auto c = SharedKey(ek.priv, peer_opk);
        ikm.insert(ikm.end(), c.begin(), c.end());
    }
    auto root = Hkdf(ikm, 96, {}, "OpenClawWASignalStubV1");
    s.root_key.assign(root.begin(), root.begin() + 32);
    s.send_chain.assign(root.begin() + 32, root.begin() + 64);
    s.recv_chain.assign(root.begin() + 64, root.end());
    s.established = true;
    return s;
}

Bytes SignalRatchetEncrypt(SignalSession& s, const Bytes& plaintext) {
    if (!s.established) throw std::runtime_error("signal session not established");
    // Derive message key then advance send chain (stub — not the Signal Double Ratchet).
    auto mk = Hkdf(s.send_chain, 64, s.root_key, "stub-msg-key");
    Bytes ct(plaintext.size() + 8);
    for (std::size_t i = 0; i < plaintext.size(); ++i) ct[i] = plaintext[i] ^ mk[i % 32];
    // trailer: first 8 bytes of mk[32..] as a tag stand-in
    for (int i = 0; i < 8; ++i) ct[plaintext.size() + i] = mk[32 + i];
    s.send_chain.assign(mk.begin() + 32, mk.end());
    ++s.send_n;
    return ct;
}

std::optional<Bytes> SignalRatchetDecrypt(SignalSession& s, const Bytes& ciphertext) {
    if (!s.established || ciphertext.size() < 8) return std::nullopt;
    auto mk = Hkdf(s.recv_chain, 64, s.root_key, "stub-msg-key");
    const std::size_t n = ciphertext.size() - 8;
    for (int i = 0; i < 8; ++i) if (ciphertext[n + i] != mk[32 + i]) return std::nullopt;
    Bytes pt(n);
    for (std::size_t i = 0; i < n; ++i) pt[i] = ciphertext[i] ^ mk[i % 32];
    s.recv_chain.assign(mk.begin() + 32, mk.end());
    ++s.recv_n;
    return pt;
}


}  // namespace openclaw::channels::waweb
