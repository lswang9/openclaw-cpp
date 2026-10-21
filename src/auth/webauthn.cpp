#include "openclaw/auth/webauthn.hpp"

#include <openssl/core_names.h>
#include <openssl/evp.h>
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
        ASN1_OBJECT* oid = OBJ_txt2obj("1.3.6.1.4.1.45724.1.1.4", 1);
        const int idx = X509_get_ext_by_OBJ(leaf, oid, -1);
        ASN1_OBJECT_free(oid);
        if (idx >= 0) {  // id-fido-gen-ce-aaguid: OCTET STRING { OCTET STRING aaguid }
            ASN1_OCTET_STRING* v = X509_EXTENSION_get_data(X509_get_ext(leaf, idx));
            const std::string raw(reinterpret_cast<const char*>(ASN1_STRING_get0_data(v)), ASN1_STRING_length(v));
            if (raw.size() != 18 || raw[0] != 0x04 || raw[1] != 16 || raw.substr(2) != ad.aaguid) return bad("aaguid extension does not match authData");
            if (X509_EXTENSION_get_critical(X509_get_ext(leaf, idx))) return bad("aaguid extension must not be critical");
        }
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
    return {{"ok", false}, {"fmt", fmt}, {"unsupported", true}, {"error", "attestation format '" + fmt + "' not supported (none|packed|fido-u2f)"}};
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
    if (policy == "required" && !(av.value("ok", false) && (av["type"] == "self" || av["type"] == "basic"))) return fail("attestation required (policy=required), got " + av.value("type", std::string("none")));
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
