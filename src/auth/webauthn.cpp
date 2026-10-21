#include "openclaw/auth/webauthn.hpp"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <optional>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/objects.h>
#include <cstdlib>

#include <chrono>
#include <memory>
#include <vector>
#include <fstream>
#include <sys/stat.h>

namespace openclaw::auth::webauthn {
namespace {
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string Rand(std::size_t n) { std::string s(n, '\0'); RAND_bytes(reinterpret_cast<unsigned char*>(s.data()), static_cast<int>(n)); return s; }
std::string Sha256(const std::string& s) {
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), d);
    return std::string(reinterpret_cast<char*>(d), sizeof d);
}
std::string Bin(const nlohmann::json& j) {
    if (!j.is_binary()) return {};
    const auto& b = j.get_binary();
    return std::string(b.begin(), b.end());
}
bool Get(const std::string& in, std::size_t& i, std::uint64_t n, std::string& out) {
    if (i + n > in.size()) return false;
    out = in.substr(i, n); i += n; return true;
}
std::optional<nlohmann::json> Item(const std::string& in, std::size_t& i, int depth) {
    if (depth > 16 || i >= in.size()) return std::nullopt;
    const auto ib = static_cast<std::uint8_t>(in[i++]);
    const int major = ib >> 5, ai = ib & 0x1f;
    std::uint64_t v = 0;
    if (ai < 24) v = static_cast<std::uint64_t>(ai);
    else if (ai >= 24 && ai <= 27) {
        const int n = 1 << (ai - 24);
        if (i + static_cast<std::size_t>(n) > in.size()) return std::nullopt;
        for (int k = 0; k < n; ++k) v = (v << 8) | static_cast<std::uint8_t>(in[i++]);
    } else return std::nullopt;  // indefinite lengths not used by authenticators
    switch (major) {
        case 0: return nlohmann::json(v);
        case 1: return nlohmann::json(-1 - static_cast<std::int64_t>(v));
        case 2: { std::string s; if (!Get(in, i, v, s)) return std::nullopt; return nlohmann::json::binary(std::vector<std::uint8_t>(s.begin(), s.end())); }
        case 3: { std::string s; if (!Get(in, i, v, s)) return std::nullopt; return nlohmann::json(s); }
        case 4: {
            if (v > 4096) return std::nullopt;
            auto a = nlohmann::json::array();
            for (std::uint64_t k = 0; k < v; ++k) { auto e = Item(in, i, depth + 1); if (!e) return std::nullopt; a.push_back(*e); }
            return a;
        }
        case 5: {
            if (v > 4096) return std::nullopt;
            auto m = nlohmann::json::object();
            for (std::uint64_t k = 0; k < v; ++k) {
                auto key = Item(in, i, depth + 1); if (!key) return std::nullopt;
                auto val = Item(in, i, depth + 1); if (!val) return std::nullopt;
                std::string ks = key->is_string() ? key->get<std::string>() : key->is_number_integer() ? std::to_string(key->get<std::int64_t>()) : key->dump();
                m[ks] = *val;
            }
            return m;
        }
        case 7:
            if (ai == 20) return nlohmann::json(false);
            if (ai == 21) return nlohmann::json(true);
            if (ai == 22 || ai == 23) return nlohmann::json(nullptr);
            return std::nullopt;
        default: return std::nullopt;  // tags unused in WebAuthn structures
    }
}
// Verify sig over msg with a COSE key (stored JSON form).
bool VerifySig(const nlohmann::json& key, const std::string& msg, const std::string& sig) {
    const int alg = key.value("alg", 0);
    EVP_PKEY* pk = nullptr;
    if (alg == -7) {  // ES256
        const std::string pt = "\x04" + B64UrlDecode(key.value("x", "")) + B64UrlDecode(key.value("y", ""));
        if (pt.size() != 65) return false;
        OSSL_PARAM_BLD* b = OSSL_PARAM_BLD_new();
        OSSL_PARAM_BLD_push_utf8_string(b, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
        OSSL_PARAM_BLD_push_octet_string(b, OSSL_PKEY_PARAM_PUB_KEY, pt.data(), pt.size());
        OSSL_PARAM* ps = OSSL_PARAM_BLD_to_param(b);
        EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        if (EVP_PKEY_fromdata_init(c) <= 0 || EVP_PKEY_fromdata(c, &pk, EVP_PKEY_PUBLIC_KEY, ps) <= 0) pk = nullptr;
        EVP_PKEY_CTX_free(c); OSSL_PARAM_free(ps); OSSL_PARAM_BLD_free(b);
    } else if (alg == -8) {  // EdDSA (Ed25519)
        const auto x = B64UrlDecode(key.value("x", ""));
        if (x.size() != 32) return false;
        pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(x.data()), x.size());
    } else if (alg == -257) {  // RS256
        const auto n = B64UrlDecode(key.value("n", "")), e = B64UrlDecode(key.value("e", ""));
        if (n.size() < 256) return false;  // >= 2048-bit
        BIGNUM* bn = BN_bin2bn(reinterpret_cast<const unsigned char*>(n.data()), static_cast<int>(n.size()), nullptr);
        BIGNUM* be = BN_bin2bn(reinterpret_cast<const unsigned char*>(e.data()), static_cast<int>(e.size()), nullptr);
        OSSL_PARAM_BLD* b = OSSL_PARAM_BLD_new();
        OSSL_PARAM_BLD_push_BN(b, OSSL_PKEY_PARAM_RSA_N, bn);
        OSSL_PARAM_BLD_push_BN(b, OSSL_PKEY_PARAM_RSA_E, be);
        OSSL_PARAM* ps = OSSL_PARAM_BLD_to_param(b);
        EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
        if (EVP_PKEY_fromdata_init(c) <= 0 || EVP_PKEY_fromdata(c, &pk, EVP_PKEY_PUBLIC_KEY, ps) <= 0) pk = nullptr;
        EVP_PKEY_CTX_free(c); OSSL_PARAM_free(ps); OSSL_PARAM_BLD_free(b); BN_free(bn); BN_free(be);
    }
    if (!pk) return false;
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(md, nullptr, alg == -8 ? nullptr : EVP_sha256(), nullptr, pk) == 1 &&
              EVP_DigestVerify(md, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                               reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1;
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pk);
    return ok;
}
// COSE_Key map → stored JSON key. Only supported algorithms.
std::optional<nlohmann::json> CoseToKey(const nlohmann::json& m) {
    if (!m.is_object()) return std::nullopt;
    const auto kty = m.value("1", 0), alg = m.value("3", 0);
    if (kty == 2 && alg == -7 && m.value("-1", 0) == 1) return nlohmann::json{{"alg", -7}, {"x", B64UrlEncode(Bin(m["-2"]))}, {"y", B64UrlEncode(Bin(m["-3"]))}};
    if (kty == 1 && alg == -8 && m.value("-1", 0) == 6) return nlohmann::json{{"alg", -8}, {"x", B64UrlEncode(Bin(m["-2"]))}};
    if (kty == 3 && alg == -257) return nlohmann::json{{"alg", -257}, {"n", B64UrlEncode(Bin(m["-1"]))}, {"e", B64UrlEncode(Bin(m["-2"]))}};
    return std::nullopt;
}
struct AuthData { std::string rp_hash; std::uint8_t flags = 0; std::uint32_t count = 0; std::string aaguid, cred_id; nlohmann::json cose; };
std::optional<AuthData> ParseAuthData(const std::string& a, bool want_cred) {
    if (a.size() < 37) return std::nullopt;
    AuthData d;
    d.rp_hash = a.substr(0, 32);
    d.flags = static_cast<std::uint8_t>(a[32]);
    for (int k = 0; k < 4; ++k) d.count = (d.count << 8) | static_cast<std::uint8_t>(a[33 + k]);
    if (want_cred) {
        if (!(d.flags & 0x40) || a.size() < 55) return std::nullopt;
        const std::size_t len = (static_cast<std::uint8_t>(a[53]) << 8) | static_cast<std::uint8_t>(a[54]);
        if (len == 0 || len > 1023 || a.size() < 55 + len) return std::nullopt;
        d.aaguid = a.substr(37, 16);
        d.cred_id = a.substr(55, len);
        std::size_t used = 0;
        auto cose = DecodeCbor(a.substr(55 + len), &used);
        if (!cose) return std::nullopt;
        d.cose = *cose;
    }
    return d;
}
std::string Policy() {
    const char* p = std::getenv("OPENCLAW_WEBAUTHN_ATTESTATION");
    std::string s = p && *p ? p : "none";
    return (s == "required" || s == "trusted") ? s : "none";
}
struct X509Del { void operator()(X509* x) const { X509_free(x); } };
using X509Ptr = std::unique_ptr<X509, X509Del>;
X509Ptr ParseDer(const std::string& der) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.data());
    return X509Ptr(d2i_X509(nullptr, &p, static_cast<long>(der.size())));
}
bool VerifyWithPkey(EVP_PKEY* pk, bool sha256, const std::string& msg, const std::string& sig) {
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(md, nullptr, sha256 ? EVP_sha256() : nullptr, nullptr, pk) == 1 &&
              EVP_DigestVerify(md, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                               reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1;
    EVP_MD_CTX_free(md);
    return ok;
}
std::string SubjectField(X509* x, int nid) {
    char buf[256] = {0};
    if (X509_NAME_get_text_by_NID(X509_get_subject_name(x), nid, buf, sizeof buf) < 0) return {};
    return buf;
}
// Chain x5c to OPENCLAW_WEBAUTHN_TRUST_ANCHORS (PEM bundle). "" = trusted, else the reason.
std::string ChainToAnchors(const std::vector<X509Ptr>& chain) {
    const char* f = std::getenv("OPENCLAW_WEBAUTHN_TRUST_ANCHORS");
    if (!f || !*f) return "no trust anchors configured (OPENCLAW_WEBAUTHN_TRUST_ANCHORS)";
    X509_STORE* st = X509_STORE_new();
    if (X509_STORE_load_file(st, f) != 1) { X509_STORE_free(st); return "cannot load trust anchors"; }
    STACK_OF(X509)* inter = sk_X509_new_null();
    for (size_t i = 1; i < chain.size(); ++i) sk_X509_push(inter, chain[i].get());
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, st, chain[0].get(), inter);
    const bool ok = X509_verify_cert(ctx) == 1;
    std::string why = ok ? "" : X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx));
    X509_STORE_CTX_free(ctx); sk_X509_free(inter); X509_STORE_free(st);
    return why;
}
// ---- DER (minimal TLV reader for attestation extensions) ----
struct Tlv { int cls = 0; bool cons = false; unsigned tag = 0; std::string val; };
bool ReadTlv(const std::string& s, std::size_t& i, Tlv& o) {
    if (i >= s.size()) return false;
    unsigned char b = static_cast<unsigned char>(s[i++]);
    o.cls = b >> 6; o.cons = (b & 0x20) != 0; o.tag = b & 0x1f;
    if (o.tag == 0x1f) { o.tag = 0; for (int n = 0; n < 4; ++n) { if (i >= s.size()) return false; unsigned char c = static_cast<unsigned char>(s[i++]); o.tag = (o.tag << 7) | (c & 0x7f); if (!(c & 0x80)) break; } }
    if (i >= s.size()) return false;
    std::size_t len = static_cast<unsigned char>(s[i++]);
    if (len & 0x80) { int n = len & 0x7f; if (n == 0 || n > 4) return false; len = 0; for (int k = 0; k < n; ++k) { if (i >= s.size()) return false; len = (len << 8) | static_cast<unsigned char>(s[i++]); } }
    if (i + len > s.size()) return false;
    o.val = s.substr(i, len); i += len;
    return true;
}
std::vector<Tlv> Children(const std::string& s) { std::vector<Tlv> v; std::size_t i = 0; Tlv t; while (ReadTlv(s, i, t)) v.push_back(t); return v; }
long long DerInt(const std::string& v) { long long x = 0; for (unsigned char c : v) x = (x << 8) | c; return x; }
std::optional<std::string> ExtData(X509* x, const char* oid_txt, bool* critical = nullptr) {
    ASN1_OBJECT* oid = OBJ_txt2obj(oid_txt, 1);
    const int idx = X509_get_ext_by_OBJ(x, oid, -1);
    ASN1_OBJECT_free(oid);
    if (idx < 0) return std::nullopt;
    X509_EXTENSION* e = X509_get_ext(x, idx);
    if (critical) *critical = X509_EXTENSION_get_critical(e) != 0;
    ASN1_OCTET_STRING* v = X509_EXTENSION_get_data(e);
    return std::string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(v)), ASN1_STRING_length(v));
}
// Does the certificate's public key equal the credential public key (stored JSON form)?
bool SameKey(EVP_PKEY* pk, const nlohmann::json& key) {
    if (!pk) return false;
    const int alg = key.value("alg", 0);
    if (alg == -7) {
        unsigned char buf[133]; size_t n = 0;
        if (EVP_PKEY_get_base_id(pk) != EVP_PKEY_EC || EVP_PKEY_get_octet_string_param(pk, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, buf, sizeof buf, &n) != 1) return false;
        return std::string(reinterpret_cast<char*>(buf), n) == "\x04" + B64UrlDecode(key.value("x", "")) + B64UrlDecode(key.value("y", ""));
    }
    if (alg == -8) {
        unsigned char buf[64]; size_t n = sizeof buf;
        return EVP_PKEY_get_base_id(pk) == EVP_PKEY_ED25519 && EVP_PKEY_get_raw_public_key(pk, buf, &n) == 1 && std::string(reinterpret_cast<char*>(buf), n) == B64UrlDecode(key.value("x", ""));
    }
    if (alg == -257) {
        BIGNUM *n = nullptr, *e = nullptr;
        if (EVP_PKEY_get_base_id(pk) != EVP_PKEY_RSA || !EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_N, &n) || !EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_E, &e)) { BN_free(n); BN_free(e); return false; }
        auto bin = [](BIGNUM* b) { std::string s(BN_num_bytes(b), '\0'); BN_bn2bin(b, reinterpret_cast<unsigned char*>(s.data())); return s; };
        auto strip = [](std::string s) { while (s.size() > 1 && s[0] == '\0') s.erase(0, 1); return s; };
        const bool ok = bin(n) == strip(B64UrlDecode(key.value("n", ""))) && bin(e) == strip(B64UrlDecode(key.value("e", "")));
        BN_free(n); BN_free(e);
        return ok;
    }
    return false;
}
std::string Sha256Raw(const std::string& s) { unsigned char h[32]; SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), h); return std::string(reinterpret_cast<char*>(h), 32); }
std::string ShaByAlg(int tpm_alg, const std::string& s) {
    unsigned char h[64]; unsigned int n = 0;
    const EVP_MD* md = tpm_alg == 0x0004 ? EVP_sha1() : tpm_alg == 0x000b ? EVP_sha256() : tpm_alg == 0x000c ? EVP_sha384() : tpm_alg == 0x000d ? EVP_sha512() : nullptr;
    if (!md) return {};
    EVP_Digest(s.data(), s.size(), h, &n, md, nullptr);
    return std::string(reinterpret_cast<char*>(h), n);
}
std::string AaguidCheck(X509* leaf, const std::string& aaguid) {
    bool crit = false;
    auto ext = ExtData(leaf, "1.3.6.1.4.1.45724.1.1.4", &crit);
    if (!ext) return {};
    if (ext->size() != 18 || (*ext)[0] != 0x04 || (*ext)[1] != 16 || ext->substr(2) != aaguid) return "aaguid extension does not match authData";
    if (crit) return "aaguid extension must not be critical";
    return {};
}
// WebAuthn §8: verify attStmt. Returns {ok, type: none|self|basic, trusted?, error?}.
nlohmann::json VerifyAttestation(const nlohmann::json& att, const std::string& auth_data, const AuthData& ad,
                                 const nlohmann::json& key, const std::string& client_hash) {
    const auto fmt = att.value("fmt", std::string());
    const auto stmt = att.value("attStmt", nlohmann::json::object());
    auto bad = [&](const std::string& e) { return nlohmann::json{{"ok", false}, {"fmt", fmt}, {"error", "attestation (" + fmt + "): " + e}}; };
    const std::string signed_data = auth_data + client_hash;
    std::vector<X509Ptr> chain;
    if (stmt.contains("x5c")) {
        if (!stmt["x5c"].is_array() || stmt["x5c"].empty() || stmt["x5c"].size() > 8) return bad("x5c malformed");
        for (auto& c : stmt["x5c"]) { auto x = ParseDer(Bin(c)); if (!x) return bad("x5c certificate unparsable"); chain.push_back(std::move(x)); }
    }
    if (fmt == "none") {
        if (!stmt.empty()) return bad("attStmt must be empty");
        return {{"ok", true}, {"fmt", fmt}, {"type", "none"}};
    }
    if (fmt == "packed") {
        const int alg = stmt.value("alg", 0);
        const auto sig = Bin(stmt.value("sig", nlohmann::json()));
        if (sig.empty()) return bad("sig missing");
        if (chain.empty()) {  // self attestation
            if (alg != key.value("alg", 0)) return bad("self attestation alg differs from credential alg");
            if (!VerifySig(key, signed_data, sig)) return bad("self attestation signature invalid");
            return {{"ok", true}, {"fmt", fmt}, {"type", "self"}};
        }
        if (alg != -7 && alg != -257 && alg != -8) return bad("unsupported alg " + std::to_string(alg));
        X509* leaf = chain[0].get();
        if (!VerifyWithPkey(X509_get0_pubkey(leaf), alg != -8, signed_data, sig)) return bad("signature does not verify with x5c[0]");
        if (X509_get_version(leaf) != 2) return bad("attestation certificate must be X.509 v3");
        if (SubjectField(leaf, NID_organizationalUnitName) != "Authenticator Attestation") return bad("subject OU must be 'Authenticator Attestation'");
        if (SubjectField(leaf, NID_countryName).empty() || SubjectField(leaf, NID_organizationName).empty() || SubjectField(leaf, NID_commonName).empty())
            return bad("subject C/O/CN required");
        if (X509_get_extension_flags(leaf) & EXFLAG_CA) return bad("attestation certificate must not be a CA");
        if (auto e = AaguidCheck(leaf, ad.aaguid); !e.empty()) return bad(e);
        auto why = ChainToAnchors(chain);
        return {{"ok", true}, {"fmt", fmt}, {"type", "basic"}, {"trusted", why.empty()}, {"trust_note", why}};
    }
    if (fmt == "fido-u2f") {
        if (chain.size() != 1) return bad("x5c must hold exactly one certificate");
        if (key.value("alg", 0) != -7) return bad("credential key must be ES256");
        EVP_PKEY* pk = X509_get0_pubkey(chain[0].get());
        if (!pk || EVP_PKEY_get_base_id(pk) != EVP_PKEY_EC) return bad("certificate key must be EC P-256");
        const auto sig = Bin(stmt.value("sig", nlohmann::json()));
        const std::string vd = std::string(1, '\0') + ad.rp_hash + client_hash + ad.cred_id + "\x04" + B64UrlDecode(key.value("x", "")) + B64UrlDecode(key.value("y", ""));
        if (!VerifyWithPkey(pk, true, vd, sig)) return bad("signature invalid");
        auto why = ChainToAnchors(chain);
        return {{"ok", true}, {"fmt", fmt}, {"type", "basic"}, {"trusted", why.empty()}, {"trust_note", why}};
    }
    if (fmt == "android-key") {  // §8.4
        if (chain.empty()) return bad("x5c required");
        const int alg = stmt.value("alg", 0);
        const auto sig = Bin(stmt.value("sig", nlohmann::json()));
        X509* leaf = chain[0].get();
        if (alg != -7 && alg != -257 && alg != -8) return bad("unsupported alg " + std::to_string(alg));
        if (!VerifyWithPkey(X509_get0_pubkey(leaf), alg != -8, signed_data, sig)) return bad("signature does not verify with x5c[0]");
        if (!SameKey(X509_get0_pubkey(leaf), key)) return bad("credential public key differs from x5c[0] public key");
        auto kd = ExtData(leaf, "1.3.6.1.4.1.11129.2.1.17");
        if (!kd) return bad("KeyDescription extension missing");
        auto top = Children(*kd);
        if (top.size() != 1 || top[0].tag != 16) return bad("KeyDescription malformed");
        auto f = Children(top[0].val);
        if (f.size() < 8) return bad("KeyDescription malformed");
        if (f[4].tag != 4 || f[4].val != client_hash) return bad("attestationChallenge does not match clientDataHash");
        auto authz = [&](const Tlv& list, unsigned tag) -> std::optional<std::string> {
            for (auto& e : Children(list.val)) if (e.cls == 2 && e.tag == tag) { auto in = Children(e.val); return in.empty() ? std::string() : in[0].val; }
            return std::nullopt;
        };
        const Tlv &sw = f[6], &tee = f[7];
        if (authz(sw, 600) || authz(tee, 600)) return bad("allApplications must not be present");
        const bool strict = std::getenv("OPENCLAW_WEBAUTHN_ANDROID_TEE_ONLY") != nullptr;
        auto pick = [&](unsigned tag) { auto v = authz(tee, tag); if (!v && !strict) v = authz(sw, tag); return v; };
        auto origin = pick(702);
        if (!origin || DerInt(*origin) != 0) return bad("key origin must be KM_ORIGIN_GENERATED");
        bool sign = false;
        for (auto& e : Children(tee.val)) if (e.cls == 2 && e.tag == 1) for (auto& set : Children(e.val)) for (auto& v : Children(set.val)) sign |= DerInt(v.val) == 2;
        if (!strict && !sign) for (auto& e : Children(sw.val)) if (e.cls == 2 && e.tag == 1) for (auto& set : Children(e.val)) for (auto& v : Children(set.val)) sign |= DerInt(v.val) == 2;
        if (!sign) return bad("key purpose must include KM_PURPOSE_SIGN");
        auto why = ChainToAnchors(chain);
        return {{"ok", true}, {"fmt", fmt}, {"type", "basic"}, {"trusted", why.empty()}, {"trust_note", why}};
    }
    if (fmt == "apple") {  // §8.8 Apple Anonymous
        if (chain.empty()) return bad("x5c required");
        X509* leaf = chain[0].get();
        auto ext = ExtData(leaf, "1.2.840.113635.100.8.2");
        if (!ext) return bad("nonce extension 1.2.840.113635.100.8.2 missing");
        auto seq = Children(*ext);
        std::string nonce;
        if (seq.size() == 1 && seq[0].tag == 16) for (auto& e : Children(seq[0].val)) if (e.cls == 2 && e.tag == 1) { auto in = Children(e.val); if (!in.empty() && in[0].tag == 4) nonce = in[0].val; }
        if (nonce != Sha256Raw(signed_data)) return bad("nonce does not match SHA-256(authData || clientDataHash)");
        if (!SameKey(X509_get0_pubkey(leaf), key)) return bad("credential public key differs from certificate public key");
        auto why = ChainToAnchors(chain);
        return {{"ok", true}, {"fmt", fmt}, {"type", "anonca"}, {"trusted", why.empty()}, {"trust_note", why}};
    }
    if (fmt == "tpm") {  // §8.3 (TPM 2.0)
        if (stmt.value("ver", std::string()) != "2.0") return bad("ver must be 2.0");
        if (chain.empty()) return bad("x5c (AIK certificate) required; ECDAA not supported");
        const int alg = stmt.value("alg", 0);
        const auto sig = Bin(stmt.value("sig", nlohmann::json())), ci = Bin(stmt.value("certInfo", nlohmann::json())), pa = Bin(stmt.value("pubArea", nlohmann::json()));
        auto u16 = [](const std::string& s, std::size_t o) { return (static_cast<unsigned>(static_cast<unsigned char>(s[o])) << 8) | static_cast<unsigned char>(s[o + 1]); };
        // pubArea (TPMT_PUBLIC)
        if (pa.size() < 10) return bad("pubArea malformed");
        const unsigned type = u16(pa, 0), name_alg = u16(pa, 2);
        const unsigned pol = u16(pa, 8);
        std::size_t o = 10 + pol;  // type, nameAlg, objectAttributes(4), authPolicy (TPM2B)
        if (type == 0x0023) {  // ECC
            if (pa.size() < o + 8) return bad("pubArea malformed");
            const unsigned curve = u16(pa, o + 4); o += 8;
            if (pa.size() < o + 2) return bad("pubArea malformed");
            const unsigned xl = u16(pa, o); if (pa.size() < o + 2 + xl + 2) return bad("pubArea malformed");
            std::string x = pa.substr(o + 2, xl); o += 2 + xl;
            const unsigned yl = u16(pa, o); if (pa.size() < o + 2 + yl) return bad("pubArea malformed");
            std::string y = pa.substr(o + 2, yl);
            if (curve != 0x0003 || key.value("alg", 0) != -7 || x != B64UrlDecode(key.value("x", "")) || y != B64UrlDecode(key.value("y", ""))) return bad("pubArea key does not match credential public key");
        } else if (type == 0x0001) {  // RSA
            if (pa.size() < o + 10) return bad("pubArea malformed");
            std::string exp = pa.substr(o + 6, 4); o += 10;
            const unsigned nl = u16(pa, o); if (pa.size() < o + 2 + nl) return bad("pubArea malformed");
            std::string n = pa.substr(o + 2, nl);
            unsigned long e = 0; for (unsigned char c : exp) e = (e << 8) | c; if (e == 0) e = 65537;
            std::string eb; for (unsigned long v = e; v; v >>= 8) eb.insert(eb.begin(), static_cast<char>(v & 0xff));
            auto strip = [](std::string s) { while (s.size() > 1 && s[0] == '\0') s.erase(0, 1); return s; };
            if (key.value("alg", 0) != -257 || strip(n) != strip(B64UrlDecode(key.value("n", ""))) || eb != strip(B64UrlDecode(key.value("e", "")))) return bad("pubArea key does not match credential public key");
        } else return bad("unsupported pubArea type");
        // certInfo (TPMS_ATTEST)
        if (ci.size() < 10 || ci.substr(0, 4) != std::string("\xff\x54\x43\x47", 4)) return bad("certInfo magic invalid");
        if (u16(ci, 4) != 0x8017) return bad("certInfo type must be TPM_ST_ATTEST_CERTIFY");
        std::size_t c = 6;
        auto tpm2b = [&](std::string* out) { if (ci.size() < c + 2) return false; const unsigned l = u16(ci, c); if (ci.size() < c + 2 + l) return false; *out = ci.substr(c + 2, l); c += 2 + l; return true; };
        std::string signer, extra, name, qname;
        if (!tpm2b(&signer) || !tpm2b(&extra)) return bad("certInfo malformed");
        const int hash_alg = alg == -7 || alg == -257 || alg == -37 ? 0x000b : alg == -35 || alg == -258 ? 0x000c : alg == -36 || alg == -259 ? 0x000d : alg == -65535 ? 0x0004 : 0;
        if (!hash_alg || extra != ShaByAlg(hash_alg, signed_data)) return bad("extraData does not match hash(authData || clientDataHash)");
        c += 17 + 8;  // clockInfo + firmwareVersion
        if (!tpm2b(&name) || !tpm2b(&qname)) return bad("certInfo malformed");
        if (name.size() < 2 || u16(name, 0) != name_alg || name.substr(2) != ShaByAlg(static_cast<int>(name_alg), pa)) return bad("attested name does not match pubArea");
        X509* aik = chain[0].get();
        if (alg != -7 && alg != -257) return bad("unsupported alg " + std::to_string(alg));
        if (!VerifyWithPkey(X509_get0_pubkey(aik), true, ci, sig)) return bad("sig does not verify over certInfo with AIK certificate");
        if (X509_get_version(aik) != 2) return bad("AIK certificate must be X.509 v3");
        if (X509_NAME_entry_count(X509_get_subject_name(aik)) != 0) return bad("AIK certificate subject must be empty");
        if (!ExtData(aik, "2.5.29.17")) return bad("AIK certificate SubjectAlternativeName (TPM manufacturer) missing");
        bool eku = false;
        if (auto* ek = static_cast<EXTENDED_KEY_USAGE*>(X509_get_ext_d2i(aik, NID_ext_key_usage, nullptr, nullptr))) {
            ASN1_OBJECT* want = OBJ_txt2obj("2.23.133.8.3", 1);
            for (int i = 0; i < sk_ASN1_OBJECT_num(ek); ++i) eku |= OBJ_cmp(sk_ASN1_OBJECT_value(ek, i), want) == 0;
            ASN1_OBJECT_free(want); EXTENDED_KEY_USAGE_free(ek);
        }
        if (!eku) return bad("AIK certificate EKU must contain tcg-kp-AIKCertificate (2.23.133.8.3)");
        if (X509_get_extension_flags(aik) & EXFLAG_CA) return bad("AIK certificate must not be a CA");
        if (auto e = AaguidCheck(aik, ad.aaguid); !e.empty()) return bad(e);
        auto why = ChainToAnchors(chain);
        return {{"ok", true}, {"fmt", fmt}, {"type", "attca"}, {"trusted", why.empty()}, {"trust_note", why}};
    }
    if (fmt == "android-safetynet") {  // §8.5 (deprecated by Google, still accepted)
        const auto jws = Bin(stmt.value("response", nlohmann::json()));
        auto d1 = jws.find('.'), d2 = jws.rfind('.');
        if (stmt.value("ver", std::string()).empty() || d1 == std::string::npos || d2 == d1) return bad("response must be a JWS");
        nlohmann::json hdr, pl;
        try { hdr = nlohmann::json::parse(B64UrlDecode(jws.substr(0, d1))); pl = nlohmann::json::parse(B64UrlDecode(jws.substr(d1 + 1, d2 - d1 - 1))); } catch (...) { return bad("JWS unparsable"); }
        if (!hdr.contains("x5c") || !hdr["x5c"].is_array() || hdr["x5c"].empty()) return bad("JWS x5c missing");
        std::vector<X509Ptr> jc;
        for (auto& c : hdr["x5c"]) {
            std::string s = c.get<std::string>(), der;
            for (auto& ch : s) { if (ch == '+') ch = '-'; else if (ch == '/') ch = '_'; }
            while (!s.empty() && s.back() == '=') s.pop_back();
            auto x = ParseDer(B64UrlDecode(s)); if (!x) return bad("JWS x5c unparsable"); jc.push_back(std::move(x));
        }
        const auto jalg = hdr.value("alg", std::string());
        if (jalg != "RS256" && jalg != "ES256") return bad("JWS alg must be RS256/ES256");
        std::string jsig = B64UrlDecode(jws.substr(d2 + 1));
        if (jalg == "ES256") {  // JOSE raw r||s → DER
            if (jsig.size() != 64) return bad("JWS signature length");
            ECDSA_SIG* es = ECDSA_SIG_new();
            ECDSA_SIG_set0(es, BN_bin2bn(reinterpret_cast<const unsigned char*>(jsig.data()), 32, nullptr), BN_bin2bn(reinterpret_cast<const unsigned char*>(jsig.data()) + 32, 32, nullptr));
            unsigned char* der = nullptr; int dl = i2d_ECDSA_SIG(es, &der); jsig.assign(reinterpret_cast<char*>(der), dl); OPENSSL_free(der); ECDSA_SIG_free(es);
        }
        if (!VerifyWithPkey(X509_get0_pubkey(jc[0].get()), true, jws.substr(0, d2), jsig)) return bad("JWS signature invalid");
        char host[256] = {0};
        X509_NAME_get_text_by_NID(X509_get_subject_name(jc[0].get()), NID_commonName, host, sizeof host);
        if (std::string(host) != "attest.android.com") return bad("leaf certificate must be issued to attest.android.com");
        std::string want_nonce; { auto h = Sha256Raw(signed_data); static const char* tb = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; int val = 0, bits = -6; for (unsigned char ch : h) { val = (val << 8) + ch; bits += 8; while (bits >= 0) { want_nonce.push_back(tb[(val >> bits) & 0x3f]); bits -= 6; } } if (bits > -6) want_nonce.push_back(tb[((val << 8) >> (bits + 8)) & 0x3f]); while (want_nonce.size() % 4) want_nonce.push_back('='); }
        if (pl.value("nonce", std::string()) != want_nonce) return bad("nonce does not match");
        if (pl.value("ctsProfileMatch", false) != true) return bad("ctsProfileMatch is false");
        const long long ts = pl.value("timestampMs", 0LL), now = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        if (ts <= 0 || ts > now + 60000 || now - ts > 600000) return bad("timestampMs out of range");
        auto why = ChainToAnchors(jc);
        return {{"ok", true}, {"fmt", fmt}, {"type", "basic"}, {"trusted", why.empty()}, {"trust_note", why}};
    }
    return {{"ok", false}, {"fmt", fmt}, {"unsupported", true}, {"error", "attestation format '" + fmt + "' not supported (none|packed|fido-u2f|android-key|android-safetynet|apple|tpm)"}};
}
}  // namespace

std::string B64UrlEncode(const std::string& raw) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o; int val = 0, bits = -6;
    for (unsigned char c : raw) { val = (val << 8) + c; bits += 8; while (bits >= 0) { o.push_back(t[(val >> bits) & 0x3f]); bits -= 6; } }
    if (bits > -6) o.push_back(t[((val << 8) >> (bits + 8)) & 0x3f]);
    return o;
}
std::string B64UrlDecode(const std::string& s) {
    std::string o; int val = 0, bits = -8;
    for (char c : s) {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A'; else if (c >= 'a' && c <= 'z') d = c - 'a' + 26; else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '-' || c == '+') d = 62; else if (c == '_' || c == '/') d = 63; else continue;
        val = (val << 6) + d; bits += 6;
        if (bits >= 0) { o.push_back(static_cast<char>((val >> bits) & 0xff)); bits -= 8; }
    }
    return o;
}
std::optional<nlohmann::json> DecodeCbor(const std::string& in, std::size_t* used) {
    std::size_t i = 0;
    auto r = Item(in, i, 0);
    if (used) *used = i;
    return r;
}

Rp RpFor(const std::string& host) {
    Rp rp;
    const char* id = std::getenv("OPENCLAW_WEBAUTHN_RP_ID");
    const char* og = std::getenv("OPENCLAW_WEBAUTHN_ORIGIN");
    rp.id = id && *id ? id : host.substr(0, host.rfind(':') == std::string::npos || host.front() == '[' ? host.size() : host.rfind(':'));
    rp.origin = og && *og ? og : "http://" + host;
    return rp;
}

Store::Store(std::filesystem::path path) : path_(std::move(path)) { Load(); }
void Store::Load() {
    data_ = {{"credentials", nlohmann::json::array()}, {"challenges", nlohmann::json::array()}};
    std::ifstream f(path_);
    if (f) try { data_ = nlohmann::json::parse(f); } catch (...) {}
    if (!data_.contains("credentials")) data_["credentials"] = nlohmann::json::array();
    if (!data_.contains("challenges")) data_["challenges"] = nlohmann::json::array();
}
void Store::Save() const {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    auto tmp = path_; tmp += ".tmp";
    { std::ofstream f(tmp); f << data_.dump(2); }
    ::chmod(tmp.c_str(), 0600);
    std::filesystem::rename(tmp, path_, ec);
}
bool Store::Enabled() const { std::lock_guard l(mu_); return !data_["credentials"].empty(); }
bool Store::Active(const std::string& id) const {
    std::lock_guard l(mu_);
    for (auto& c : data_["credentials"]) if (c.value("id", "") == id) return true;
    return false;
}
nlohmann::json Store::List() const {
    std::lock_guard l(mu_);
    auto out = nlohmann::json::array();
    for (auto& c : data_["credentials"])
        out.push_back({{"id", c["id"]}, {"user", c["user"]}, {"role", c["role"]}, {"alg", c["key"]["alg"]}, {"sign_count", c["sign_count"]},
                       {"created_ms", c["created_ms"]}, {"last_used_ms", c.value("last_used_ms", 0)}, {"attestation", c.value("attestation", nlohmann::json())}});
    return out;
}
nlohmann::json Store::Remove(const std::string& id) {
    std::lock_guard l(mu_);
    auto& cs = data_["credentials"];
    const auto n = cs.size();
    cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const nlohmann::json& c) { return c.value("id", "") == id; }), cs.end());
    Save();
    return {{"ok", cs.size() != n}, {"removed", cs.size() != n}};
}
std::optional<nlohmann::json> Store::TakeChallenge(const std::string& ch, const std::string& type) {
    auto& cs = data_["challenges"];
    const auto now = NowMs();
    std::optional<nlohmann::json> hit;
    nlohmann::json keep = nlohmann::json::array();
    for (auto& c : cs) {
        if (c.value("exp", (std::int64_t)0) < now) continue;
        if (!hit && c.value("challenge", "") == ch && c.value("type", "") == type) { hit = c; continue; }  // single use
        keep.push_back(c);
    }
    cs = keep;
    return hit;
}

nlohmann::json Store::BeginRegistration(const Rp& rp, const std::string& user, const std::string& role) {
    std::lock_guard l(mu_);
    const auto ch = B64UrlEncode(Rand(32));
    const auto uid = B64UrlEncode(Sha256("openclaw-user:" + user).substr(0, 16));
    data_["challenges"].push_back({{"challenge", ch}, {"type", "webauthn.create"}, {"user", user}, {"role", role}, {"exp", NowMs() + 300000}});
    auto exclude = nlohmann::json::array();
    for (auto& c : data_["credentials"]) if (c.value("user", "") == user) exclude.push_back({{"type", "public-key"}, {"id", c["id"]}});
    Save();
    return {{"rp", {{"id", rp.id}, {"name", rp.name}}},
            {"user", {{"id", uid}, {"name", user}, {"displayName", user}}},
            {"challenge", ch},
            {"pubKeyCredParams", nlohmann::json::array({{{"type", "public-key"}, {"alg", -7}}, {{"type", "public-key"}, {"alg", -8}}, {{"type", "public-key"}, {"alg", -257}}})},
            {"timeout", 300000}, {"attestation", Policy() == "none" ? "none" : "direct"}, {"excludeCredentials", exclude},
            {"authenticatorSelection", {{"residentKey", "preferred"}, {"userVerification", "preferred"}}}};
}

nlohmann::json Store::FinishRegistration(const Rp& rp, const nlohmann::json& cred) {
    auto fail = [](const std::string& e) { return nlohmann::json{{"ok", false}, {"error", e}}; };
    if (!cred.is_object() || !cred.contains("response")) return fail("credential.response missing");
    const auto& r = cred["response"];
    const auto cdj = B64UrlDecode(r.value("clientDataJSON", std::string()));
    nlohmann::json cd;
    try { cd = nlohmann::json::parse(cdj); } catch (...) { return fail("clientDataJSON not JSON"); }
    if (cd.value("type", "") != "webauthn.create") return fail("clientData.type must be webauthn.create");
    if (cd.value("origin", "") != rp.origin) return fail("origin mismatch: " + cd.value("origin", std::string()));
    std::lock_guard l(mu_);
    auto chal = TakeChallenge(cd.value("challenge", std::string()), "webauthn.create");
    Save();
    if (!chal) return fail("unknown, used or expired challenge");
    auto att = DecodeCbor(B64UrlDecode(r.value("attestationObject", std::string())));
    if (!att || !att->is_object() || !att->contains("authData")) return fail("attestationObject malformed");
    auto ad = ParseAuthData(Bin((*att)["authData"]), true);
    if (!ad) return fail("authData malformed / no attested credential");
    if (ad->rp_hash != Sha256(rp.id)) return fail("rpIdHash mismatch");
    if (!(ad->flags & 0x01)) return fail("user presence flag not set");
    auto key = CoseToKey(ad->cose);
    if (!key) return fail("unsupported credential key (ES256, EdDSA, RS256 >= 2048)");
    const auto id = B64UrlEncode(ad->cred_id);
    if (cred.contains("rawId") && B64UrlDecode(cred.value("rawId", std::string())) != ad->cred_id) return fail("rawId does not match authData");
    for (auto& c : data_["credentials"]) if (c.value("id", "") == id) return fail("credential already registered");
    // Attestation: a statement that is present but does not verify is always rejected; policy decides what is enough.
    auto av = VerifyAttestation(*att, Bin((*att)["authData"]), *ad, *key, Sha256(cdj));
    const auto policy = Policy();
    if (!av.value("ok", false) && !(av.value("unsupported", false) && policy == "none")) return fail(av.value("error", std::string("attestation invalid")));
    if (policy == "required" && !(av.value("ok", false) && (av["type"] == "self" || av["type"] == "basic" || av["type"] == "attca" || av["type"] == "anonca"))) return fail("attestation required (policy=required), got " + av.value("type", std::string("none")));
    if (policy == "trusted" && !av.value("trusted", false)) return fail("trusted attestation required: " + av.value("trust_note", av.value("error", std::string("type ") + av.value("type", std::string("none")))));
    nlohmann::json attestation{{"fmt", av.value("fmt", std::string())}, {"type", av.value("ok", false) ? av.value("type", std::string()) : std::string("unverified")}, {"trusted", av.value("trusted", false)}};
    data_["credentials"].push_back({{"id", id}, {"user", (*chal)["user"]}, {"role", (*chal)["role"]}, {"key", *key}, {"sign_count", ad->count},
                                    {"uv", (ad->flags & 0x04) != 0}, {"fmt", att->value("fmt", std::string())}, {"attestation", attestation},
                                    {"aaguid", B64UrlEncode(ad->aaguid)}, {"created_ms", NowMs()}});
    Save();
    return {{"ok", true}, {"credential_id", id}, {"user", (*chal)["user"]}, {"role", (*chal)["role"]}, {"alg", (*key)["alg"]}, {"attestation", attestation}};
}

nlohmann::json Store::BeginAuthentication(const Rp& rp) {
    std::lock_guard l(mu_);
    const auto ch = B64UrlEncode(Rand(32));
    data_["challenges"].push_back({{"challenge", ch}, {"type", "webauthn.get"}, {"exp", NowMs() + 300000}});
    auto allow = nlohmann::json::array();
    for (auto& c : data_["credentials"]) allow.push_back({{"type", "public-key"}, {"id", c["id"]}});
    Save();
    return {{"challenge", ch}, {"rpId", rp.id}, {"timeout", 300000}, {"allowCredentials", allow}, {"userVerification", "preferred"}};
}

nlohmann::json Store::FinishAuthentication(const Rp& rp, const nlohmann::json& cred) {
    auto fail = [](const std::string& e) { return nlohmann::json{{"ok", false}, {"error", e}}; };
    if (!cred.is_object() || !cred.contains("response")) return fail("credential.response missing");
    const auto& r = cred["response"];
    const auto cdj = B64UrlDecode(r.value("clientDataJSON", std::string()));
    nlohmann::json cd;
    try { cd = nlohmann::json::parse(cdj); } catch (...) { return fail("clientDataJSON not JSON"); }
    if (cd.value("type", "") != "webauthn.get") return fail("clientData.type must be webauthn.get");
    if (cd.value("origin", "") != rp.origin) return fail("origin mismatch");
    std::lock_guard l(mu_);
    auto chal = TakeChallenge(cd.value("challenge", std::string()), "webauthn.get");
    Save();
    if (!chal) return fail("unknown, used or expired challenge");
    const auto id = B64UrlEncode(B64UrlDecode(cred.value("rawId", cred.value("id", std::string()))));
    nlohmann::json* c = nullptr;
    for (auto& x : data_["credentials"]) if (x.value("id", "") == id) c = &x;
    if (!c) return fail("unknown credential");
    const auto adb = B64UrlDecode(r.value("authenticatorData", std::string()));
    auto ad = ParseAuthData(adb, false);
    if (!ad) return fail("authenticatorData malformed");
    if (ad->rp_hash != Sha256(rp.id)) return fail("rpIdHash mismatch");
    if (!(ad->flags & 0x01)) return fail("user presence flag not set");
    const auto sig = B64UrlDecode(r.value("signature", std::string()));
    if (!VerifySig((*c)["key"], adb + Sha256(cdj), sig)) return fail("bad signature");
    const std::uint32_t stored = (*c).value("sign_count", 0u);
    if ((ad->count != 0 || stored != 0) && ad->count <= stored) return fail("signCount did not increase (possible cloned authenticator)");
    (*c)["sign_count"] = ad->count;
    (*c)["last_used_ms"] = NowMs();
    Save();
    return {{"ok", true}, {"credential_id", id}, {"user", (*c)["user"]}, {"role", (*c)["role"]}, {"sign_count", ad->count}, {"uv", (ad->flags & 0x04) != 0}};
}

}  // namespace openclaw::auth::webauthn
