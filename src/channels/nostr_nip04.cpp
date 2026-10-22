#include "openclaw/channels/nostr_nip04.hpp"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>

#include <cctype>
#include <cstring>
#include <vector>

namespace openclaw::channels::nostr {
namespace {

using Bytes = std::vector<unsigned char>;

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Bytes HexDecode(const std::string& s) {
    if (s.size() % 2) return {};
    Bytes out(s.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const int a = HexVal(s[2 * i]), b = HexVal(s[2 * i + 1]);
        if (a < 0 || b < 0) return {};
        out[i] = static_cast<unsigned char>((a << 4) | b);
    }
    return out;
}

std::string HexEncode(const Bytes& b) {
    static const char* k = "0123456789abcdef";
    std::string o(b.size() * 2, '0');
    for (size_t i = 0; i < b.size(); ++i) { o[2 * i] = k[b[i] >> 4]; o[2 * i + 1] = k[b[i] & 15]; }
    return o;
}

std::string B64(const Bytes& b) {
    std::string o(4 * ((b.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&o[0]), b.data(), (int)b.size());
    o.resize(n > 0 ? (size_t)n : 0);
    return o;
}

Bytes B64Dec(const std::string& s) {
    Bytes o(s.size());
    const int n = EVP_DecodeBlock(o.data(), reinterpret_cast<const unsigned char*>(s.data()), (int)s.size());
    if (n < 0) return {};
    size_t pad = 0;
    if (!s.empty() && s.back() == '=') ++pad;
    if (s.size() > 1 && s[s.size() - 2] == '=') ++pad;
    o.resize((size_t)n > pad ? (size_t)n - pad : 0);
    return o;
}

// Minimal bech32 (BIP-0173) decode for nsec1 / npub1.
bool Bech32Decode(const std::string& in, std::string* hrp, Bytes* data5) {
    static const char* charset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    auto lower = in;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        else if (c < 33 || c > 126) return false;
    }
    const auto pos = lower.rfind('1');
    if (pos == std::string::npos || pos < 1 || pos + 7 > lower.size()) return false;
    *hrp = lower.substr(0, pos);
    data5->clear();
    for (size_t i = pos + 1; i < lower.size(); ++i) {
        const char* p = std::strchr(charset, lower[i]);
        if (!p) return false;
        data5->push_back(static_cast<unsigned char>(p - charset));
    }
    if (data5->size() < 6) return false;
    // Verify checksum (polymod)
    auto polymod = [](const Bytes& values) -> uint32_t {
        uint32_t chk = 1;
        for (auto v : values) {
            const uint32_t b = chk >> 25;
            chk = ((chk & 0x1ffffffu) << 5) ^ v;
            if (b & 1) chk ^= 0x3b6a57b2u;
            if (b & 2) chk ^= 0x26508e6du;
            if (b & 4) chk ^= 0x1ea119fau;
            if (b & 8) chk ^= 0x3d4233ddu;
            if (b & 16) chk ^= 0x2a1462b3u;
        }
        return chk;
    };
    Bytes values;
    for (char c : *hrp) { values.push_back(static_cast<unsigned char>(c >> 5)); }
    values.push_back(0);
    for (char c : *hrp) { values.push_back(static_cast<unsigned char>(c & 31)); }
    values.insert(values.end(), data5->begin(), data5->end());
    if (polymod(values) != 1) return false;
    data5->resize(data5->size() - 6);
    return true;
}

Bytes ConvertBits(const Bytes& in, int from, int to, bool pad) {
    uint32_t acc = 0; int bits = 0;
    const uint32_t maxv = (1u << to) - 1;
    Bytes out;
    for (auto v : in) {
        acc = (acc << from) | v;
        bits += from;
        while (bits >= to) { bits -= to; out.push_back(static_cast<unsigned char>((acc >> bits) & maxv)); }
    }
    if (pad && bits) out.push_back(static_cast<unsigned char>((acc << (to - bits)) & maxv));
    return out;
}

EC_KEY* KeyFromPriv(const Bytes& priv) {
    if (priv.size() != 32) return nullptr;
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!key) return nullptr;
    BIGNUM* bn = BN_bin2bn(priv.data(), 32, nullptr);
    if (!bn || EC_KEY_set_private_key(key, bn) != 1) { BN_free(bn); EC_KEY_free(key); return nullptr; }
    const EC_GROUP* g = EC_KEY_get0_group(key);
    EC_POINT* pub = EC_POINT_new(g);
    if (!pub || EC_POINT_mul(g, pub, bn, nullptr, nullptr, nullptr) != 1 ||
        EC_KEY_set_public_key(key, pub) != 1) {
        EC_POINT_free(pub); BN_free(bn); EC_KEY_free(key); return nullptr;
    }
    EC_POINT_free(pub); BN_free(bn);
    return key;
}

EC_KEY* KeyFromPubX(const Bytes& x) {
    if (x.size() != 32) return nullptr;
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!key) return nullptr;
    const EC_GROUP* g = EC_KEY_get0_group(key);
    Bytes compressed(33);
    compressed[0] = 0x02;  // even y; ECDH x is identical for 0x03
    std::memcpy(compressed.data() + 1, x.data(), 32);
    EC_POINT* pub = EC_POINT_new(g);
    if (!pub || EC_POINT_oct2point(g, pub, compressed.data(), compressed.size(), nullptr) != 1 ||
        EC_KEY_set_public_key(key, pub) != 1) {
        // try odd y
        if (pub) {
            compressed[0] = 0x03;
            if (EC_POINT_oct2point(g, pub, compressed.data(), compressed.size(), nullptr) != 1 ||
                EC_KEY_set_public_key(key, pub) != 1) {
                EC_POINT_free(pub); EC_KEY_free(key); return nullptr;
            }
        } else { EC_KEY_free(key); return nullptr; }
    }
    EC_POINT_free(pub);
    return key;
}

Bytes EcdhSharedX(const Bytes& priv, const Bytes& peer_x) {
    EC_KEY* a = KeyFromPriv(priv);
    EC_KEY* b = KeyFromPubX(peer_x);
    if (!a || !b) { EC_KEY_free(a); EC_KEY_free(b); return {}; }
    Bytes secret(32);
    const int n = ECDH_compute_key(secret.data(), secret.size(), EC_KEY_get0_public_key(b), a, nullptr);
    EC_KEY_free(a); EC_KEY_free(b);
    if (n != 32) return {};
    return secret;
}

Bytes Aes256Cbc(const Bytes& key, const Bytes& iv, const Bytes& in, bool enc) {
    if (key.size() != 32 || iv.size() != 16) return {};
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    Bytes out(in.size() + 16);
    int l = 0, t = 0;
    bool ok = EVP_CipherInit_ex(c, EVP_aes_256_cbc(), nullptr, key.data(), iv.data(), enc ? 1 : 0) == 1 &&
              EVP_CipherUpdate(c, out.data(), &l, in.data(), (int)in.size()) == 1 &&
              EVP_CipherFinal_ex(c, out.data() + l, &t) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) return {};
    out.resize((size_t)(l + t));
    return out;
}

}  // namespace

std::string NormalizeKey(const std::string& in) {
    std::string s = in;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    if (s.size() >= 5 && (s.rfind("nsec1", 0) == 0 || s.rfind("NSEC1", 0) == 0 ||
                          s.rfind("npub1", 0) == 0 || s.rfind("NPUB1", 0) == 0)) {
        std::string hrp; Bytes data5;
        if (!Bech32Decode(s, &hrp, &data5)) return {};
        if (hrp != "nsec" && hrp != "npub") return {};
        Bytes raw = ConvertBits(data5, 5, 8, false);
        if (raw.size() != 32) return {};
        return HexEncode(raw);
    }
    if (s.size() == 64 && HexDecode(s).size() == 32) {
        for (char& c : s) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }
    return {};
}

std::string PubkeyHex(const std::string& priv_hex) {
    // NormalizeKey already lowercased; call again safely
    const auto nh = NormalizeKey(priv_hex);
    const auto p = HexDecode(nh);
    if (p.size() != 32) return {};
    EC_KEY* key = KeyFromPriv(p);
    if (!key) return {};
    const EC_GROUP* g = EC_KEY_get0_group(key);
    const EC_POINT* pub = EC_KEY_get0_public_key(key);
    Bytes out(32);
    BIGNUM* x = BN_new();
    if (!EC_POINT_get_affine_coordinates(g, pub, x, nullptr, nullptr) || BN_num_bytes(x) > 32) {
        BN_free(x); EC_KEY_free(key); return {};
    }
    BN_bn2binpad(x, out.data(), 32);
    BN_free(x); EC_KEY_free(key);
    return HexEncode(out);
}

std::string Encrypt(const std::string& priv_hex, const std::string& peer_pub_hex, const std::string& text) {
    const auto priv = HexDecode(NormalizeKey(priv_hex));
    const auto peer = HexDecode(NormalizeKey(peer_pub_hex));
    if (priv.size() != 32 || peer.size() != 32) return {};
    const auto key = EcdhSharedX(priv, peer);
    if (key.size() != 32) return {};
    Bytes iv(16);
    if (RAND_bytes(iv.data(), 16) != 1) return {};
    const Bytes pt(text.begin(), text.end());
    const auto ct = Aes256Cbc(key, iv, pt, true);
    if (ct.empty()) return {};
    return B64(ct) + "?iv=" + B64(iv);
}

std::optional<std::string> Decrypt(const std::string& priv_hex, const std::string& peer_pub_hex,
                                   const std::string& ciphertext) {
    const auto priv = HexDecode(NormalizeKey(priv_hex));
    const auto peer = HexDecode(NormalizeKey(peer_pub_hex));
    if (priv.size() != 32 || peer.size() != 32) return std::nullopt;
    const auto sep = ciphertext.find("?iv=");
    if (sep == std::string::npos) return std::nullopt;
    const auto ct = B64Dec(ciphertext.substr(0, sep));
    const auto iv = B64Dec(ciphertext.substr(sep + 4));
    if (ct.empty() || iv.size() != 16) return std::nullopt;
    const auto key = EcdhSharedX(priv, peer);
    if (key.size() != 32) return std::nullopt;
    const auto pt = Aes256Cbc(key, iv, ct, false);
    if (pt.empty()) return std::nullopt;
    return std::string(pt.begin(), pt.end());
}

}  // namespace openclaw::channels::nostr
