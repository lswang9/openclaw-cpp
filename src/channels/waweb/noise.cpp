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

}  // namespace openclaw::channels::waweb
