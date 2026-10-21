#include "openclaw/auth/oidc.hpp"

#include <curl/curl.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>

namespace openclaw::auth::oidc {
namespace {
std::string Env(const char* k, const std::string& d = "") { const char* v = std::getenv(k); return v && *v ? v : d; }
std::int64_t Now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string B64Url(const unsigned char* d, std::size_t n) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o;
    for (std::size_t i = 0; i < n; i += 3) {
        std::uint32_t v = std::uint32_t(d[i]) << 16 | (i + 1 < n ? std::uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += a[(v >> 18) & 63]; o += a[(v >> 12) & 63];
        if (i + 1 < n) o += a[(v >> 6) & 63];
        if (i + 2 < n) o += a[v & 63];
    }
    return o;
}
std::optional<std::string> B64UrlDec(const std::string& s) {
    std::string o; std::uint32_t buf = 0; int bits = 0;
    for (char c : s) {
        int v = (c >= 'A' && c <= 'Z') ? c - 'A' : (c >= 'a' && c <= 'z') ? c - 'a' + 26 : (c >= '0' && c <= '9') ? c - '0' + 52 :
                (c == '-' || c == '+') ? 62 : (c == '_' || c == '/') ? 63 : (c == '=' ? -2 : -1);
        if (v == -2) break;
        if (v < 0) return std::nullopt;
        buf = (buf << 6) | static_cast<std::uint32_t>(v); bits += 6;
        if (bits >= 8) { o += static_cast<char>((buf >> (bits - 8)) & 0xff); bits -= 8; }
    }
    return o;
}
std::string Rand(std::size_t n) { std::string b(n, '\0'); RAND_bytes(reinterpret_cast<unsigned char*>(b.data()), static_cast<int>(n)); return B64Url(reinterpret_cast<const unsigned char*>(b.data()), n); }
std::string Enc(const std::string& s) {
    std::string o; char h[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { std::snprintf(h, sizeof h, "%%%02X", c); o += h; }
    }
    return o;
}
size_t W(char* p, size_t s, size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }
std::optional<nlohmann::json> Http(const std::string& url, const std::string* form, std::string* err) {
    CURL* c = curl_easy_init();
    std::string body;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, W);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_slist* h = curl_slist_append(nullptr, "Accept: application/json");
    if (form) {
        h = curl_slist_append(h, "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, form->c_str());
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    auto rc = curl_easy_perform(c);
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h); curl_easy_cleanup(c);
    if (rc != CURLE_OK || code / 100 != 2) { if (err) *err = "http " + std::to_string(code) + " from " + url + ": " + body.substr(0, 200); return std::nullopt; }
    try { return nlohmann::json::parse(body); } catch (...) { if (err) *err = "bad json from " + url; return std::nullopt; }
}
struct Pending { std::string verifier, nonce, redirect; std::int64_t at; };
std::mutex g_mu;
std::map<std::string, Pending> g_pending;
std::map<std::string, std::pair<nlohmann::json, std::int64_t>> g_disco;
std::optional<nlohmann::json> Discover(const Config& c, std::string* err) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (auto it = g_disco.find(c.issuer); it != g_disco.end() && Now() - it->second.second < 300) return it->second.first;
    std::string u = c.issuer; while (!u.empty() && u.back() == '/') u.pop_back();
    auto d = Http(u + "/.well-known/openid-configuration", nullptr, err);
    if (!d) return std::nullopt;
    if (d->value("issuer", std::string()) != c.issuer && d->value("issuer", std::string()) + "/" != c.issuer) {
        if (err) *err = "discovery issuer mismatch"; return std::nullopt;
    }
    g_disco[c.issuer] = {*d, Now()};
    return d;
}
bool InList(const std::string& list, const std::string& v) {
    std::stringstream ss(list); std::string x;
    while (std::getline(ss, x, ',')) {
        while (!x.empty() && x.front() == ' ') x.erase(x.begin());
        while (!x.empty() && x.back() == ' ') x.pop_back();
        if (!x.empty() && x == v) return true;
    }
    return false;
}
std::string Lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
}  // namespace

Config FromEnv() {
    Config c;
    c.issuer = Env("OPENCLAW_OIDC_ISSUER");
    c.client_id = Env("OPENCLAW_OIDC_CLIENT_ID");
    c.client_secret = Env("OPENCLAW_OIDC_CLIENT_SECRET");
    c.redirect_uri = Env("OPENCLAW_OIDC_REDIRECT_URI");
    c.role = Env("OPENCLAW_OIDC_ROLE", "operator");
    c.scopes = Env("OPENCLAW_OIDC_SCOPES", "openid email profile");
    c.allowed_emails = Lower(Env("OPENCLAW_OIDC_ALLOWED_EMAILS"));
    c.allowed_subs = Env("OPENCLAW_OIDC_ALLOWED_SUBS");
    c.allowed_domains = Lower(Env("OPENCLAW_OIDC_ALLOWED_DOMAINS"));
    return c;
}

std::string PkceChallenge(const std::string& v) {
    unsigned char h[32]; unsigned int l = 0;
    EVP_Digest(v.data(), v.size(), h, &l, EVP_sha256(), nullptr);
    return B64Url(h, l);
}

Start BeginLogin(const Config& c, const std::string& redirect_default) {
    Start s;
    std::string err;
    auto d = Discover(c, &err);
    if (!d) { s.error = err; return s; }
    const auto ae = d->value("authorization_endpoint", std::string());
    if (ae.empty()) { s.error = "no authorization_endpoint"; return s; }
    Pending p{Rand(32), Rand(16), c.redirect_uri.empty() ? redirect_default : c.redirect_uri, Now()};
    s.state = Rand(16);
    s.url = ae + (ae.find('?') == std::string::npos ? "?" : "&") + "response_type=code&client_id=" + Enc(c.client_id) +
            "&redirect_uri=" + Enc(p.redirect) + "&scope=" + Enc(c.scopes) + "&state=" + s.state + "&nonce=" + p.nonce +
            "&code_challenge=" + PkceChallenge(p.verifier) + "&code_challenge_method=S256";
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_pending.begin(); it != g_pending.end();) it = Now() - it->second.at > 600 ? g_pending.erase(it) : std::next(it);
    g_pending[s.state] = p;
    return s;
}

std::optional<nlohmann::json> VerifyRs256(const std::string& jwt, const nlohmann::json& jwks, std::string* err) {
    auto fail = [&](const std::string& e) -> std::optional<nlohmann::json> { if (err) *err = e; return std::nullopt; };
    const auto d1 = jwt.find('.'), d2 = jwt.find('.', d1 == std::string::npos ? 0 : d1 + 1);
    if (d1 == std::string::npos || d2 == std::string::npos) return fail("malformed jwt");
    auto hs = B64UrlDec(jwt.substr(0, d1)), ps = B64UrlDec(jwt.substr(d1 + 1, d2 - d1 - 1)), sg = B64UrlDec(jwt.substr(d2 + 1));
    if (!hs || !ps || !sg) return fail("bad base64");
    nlohmann::json hdr, pl;
    try { hdr = nlohmann::json::parse(*hs); pl = nlohmann::json::parse(*ps); } catch (...) { return fail("bad jwt json"); }
    if (hdr.value("alg", std::string()) != "RS256") return fail("unsupported alg (RS256 only)");
    const auto kid = hdr.value("kid", std::string());
    const nlohmann::json* key = nullptr;
    if (jwks.contains("keys"))
        for (auto& k : jwks["keys"])
            if (k.value("kty", std::string()) == "RSA" && (kid.empty() || k.value("kid", std::string()) == kid)) { key = &k; break; }
    if (!key) return fail("no matching jwk");
    auto n = B64UrlDec(key->value("n", std::string())), e = B64UrlDec(key->value("e", std::string()));
    if (!n || !e || n->empty()) return fail("bad jwk");
    BIGNUM* bn = BN_bin2bn(reinterpret_cast<const unsigned char*>(n->data()), static_cast<int>(n->size()), nullptr);
    BIGNUM* be = BN_bin2bn(reinterpret_cast<const unsigned char*>(e->data()), static_cast<int>(e->size()), nullptr);
    OSSL_PARAM_BLD* b = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_BN(b, OSSL_PKEY_PARAM_RSA_N, bn);
    OSSL_PARAM_BLD_push_BN(b, OSSL_PKEY_PARAM_RSA_E, be);
    OSSL_PARAM* prm = OSSL_PARAM_BLD_to_param(b);
    EVP_PKEY_CTX* pc = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    EVP_PKEY* pk = nullptr;
    bool ok = EVP_PKEY_fromdata_init(pc) == 1 && EVP_PKEY_fromdata(pc, &pk, EVP_PKEY_PUBLIC_KEY, prm) == 1;
    if (ok && EVP_PKEY_get_bits(pk) < 2048) ok = false;
    if (ok) {
        EVP_MD_CTX* m = EVP_MD_CTX_new();
        const std::string si = jwt.substr(0, d2);
        ok = EVP_DigestVerifyInit(m, nullptr, EVP_sha256(), nullptr, pk) == 1 &&
             EVP_DigestVerify(m, reinterpret_cast<const unsigned char*>(sg->data()), sg->size(),
                              reinterpret_cast<const unsigned char*>(si.data()), si.size()) == 1;
        EVP_MD_CTX_free(m);
    }
    EVP_PKEY_free(pk); EVP_PKEY_CTX_free(pc); OSSL_PARAM_free(prm); OSSL_PARAM_BLD_free(b); BN_free(bn); BN_free(be);
    if (!ok) return fail("signature invalid");
    return pl;
}

Result FinishLogin(const Config& c, const std::string& code, const std::string& state) {
    Result r;
    Pending p;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_pending.find(state);
        if (state.empty() || it == g_pending.end()) { r.error = "unknown or expired state"; return r; }
        p = it->second;
        g_pending.erase(it);  // single use
    }
    if (Now() - p.at > 600) { r.error = "state expired"; return r; }
    std::string err;
    auto d = Discover(c, &err);
    if (!d) { r.error = err; return r; }
    std::string form = "grant_type=authorization_code&code=" + Enc(code) + "&redirect_uri=" + Enc(p.redirect) +
                       "&client_id=" + Enc(c.client_id) + "&code_verifier=" + p.verifier;
    if (!c.client_secret.empty()) form += "&client_secret=" + Enc(c.client_secret);
    auto tok = Http(d->value("token_endpoint", std::string()), &form, &err);
    if (!tok) { r.error = "token exchange failed: " + err; return r; }
    const auto idt = tok->value("id_token", std::string());
    if (idt.empty()) { r.error = "no id_token"; return r; }
    auto jwks = Http(d->value("jwks_uri", std::string()), nullptr, &err);
    if (!jwks) { r.error = "jwks: " + err; return r; }
    auto cl = VerifyRs256(idt, *jwks, &err);
    if (!cl) { r.error = "id_token: " + err; return r; }
    const auto now = Now();
    bool aud_ok = false;
    if ((*cl)["aud"].is_string()) aud_ok = (*cl)["aud"] == c.client_id;
    else if ((*cl)["aud"].is_array()) {
        for (auto& a : (*cl)["aud"]) if (a == c.client_id) aud_ok = true;
        if (aud_ok && (*cl)["aud"].size() > 1 && cl->value("azp", std::string()) != c.client_id) aud_ok = false;
    }
    if (cl->value("iss", std::string()) != d->value("issuer", std::string())) { r.error = "iss mismatch"; return r; }
    if (!aud_ok) { r.error = "aud mismatch"; return r; }
    if (!cl->contains("exp") || !(*cl)["exp"].is_number() || (*cl)["exp"].get<std::int64_t>() + 60 < now) { r.error = "id_token expired"; return r; }
    if (cl->contains("iat") && (*cl)["iat"].is_number() && (*cl)["iat"].get<std::int64_t>() > now + 300) { r.error = "iat in future"; return r; }
    if (cl->value("nonce", std::string()) != p.nonce) { r.error = "nonce mismatch"; return r; }
    r.sub = cl->value("sub", std::string());
    r.email = Lower(cl->value("email", std::string()));
    const bool ev = cl->contains("email_verified") && ((*cl)["email_verified"] == true || (*cl)["email_verified"] == "true");
    if (r.sub.empty()) { r.error = "no sub"; return r; }
    const auto at = r.email.find('@');
    const bool allowed = InList(c.allowed_subs, r.sub) || (ev && !r.email.empty() && InList(c.allowed_emails, r.email)) ||
                         (ev && at != std::string::npos && InList(c.allowed_domains, r.email.substr(at + 1)));
    if (!allowed) { r.error = "not allowed: " + (r.email.empty() ? r.sub : r.email); return r; }
    r.role = (c.role == "admin") ? "admin" : "operator";
    r.ok = true;
    return r;
}

}  // namespace openclaw::auth::oidc
