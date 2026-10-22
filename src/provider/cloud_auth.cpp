#include "openclaw/provider/cloud_auth.hpp"
#include <cctype>
#include <cstdio>
#include <ctime>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace openclaw::provider::cloud_auth {
namespace {

using json = nlohmann::json;

std::string HexLower(const unsigned char* d, std::size_t n) {
    static const char* k = "0123456789abcdef";
    std::string o; o.resize(n * 2);
    for (std::size_t i = 0; i < n; ++i) { o[i * 2] = k[d[i] >> 4]; o[i * 2 + 1] = k[d[i] & 15]; }
    return o;
}

std::string Sha256Hex(const std::string& s) {
    unsigned char dig[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), dig);
    return HexLower(dig, SHA256_DIGEST_LENGTH);
}

std::string HmacSha256Raw(const std::string& key, const std::string& data) {
    unsigned char mac[EVP_MAX_MD_SIZE]; unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), mac, &len);
    return std::string(reinterpret_cast<char*>(mac), len);
}

std::string Env(const char* k) {
    if (const char* e = std::getenv(k); e && *e) return e;
    return {};
}

std::string UrlEncode(const std::string& s) {
    std::string o; char buf[8];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { std::snprintf(buf, sizeof(buf), "%%%02X", c); o += buf; }
    }
    return o;
}

std::string B64Url(const unsigned char* d, std::size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (std::size_t i = 0; i < n; i += 3) {
        unsigned v = d[i] << 16; if (i + 1 < n) v |= d[i + 1] << 8; if (i + 2 < n) v |= d[i + 2];
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63];
        o += (i + 1 < n) ? t[(v >> 6) & 63] : '='; o += (i + 2 < n) ? t[v & 63] : '=';
    }
    for (auto& c : o) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    while (!o.empty() && o.back() == '=') o.pop_back();
    return o;
}

std::string B64UrlStr(const std::string& s) {
    return B64Url(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

size_t CurlSink(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n); return s * n;
}

json HttpPostForm(const std::string& url, const std::string& form) {
    std::string body; long status = 0;
    CURL* c = curl_easy_init();
    if (!c) return json{{"error", "curl init"}};
    curl_slist* h = curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, form.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, CurlSink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(h); curl_easy_cleanup(c);
    try {
        auto j = json::parse(body.empty() ? "{}" : body);
        if (!j.is_object()) j = json::object();
        j["_http"] = status;
        return j;
    } catch (...) { return json{{"error", body.substr(0, 200)}, {"_http", status}}; }
}

std::string AdcPath() {
    if (auto p = Env("GOOGLE_APPLICATION_CREDENTIALS"); !p.empty()) return p;
    if (auto home = Env("HOME"); !home.empty()) {
        auto p = home + "/.config/gcloud/application_default_credentials.json";
        std::ifstream f(p); if (f.good()) return p;
    }
    return {};
}

json ReadAdc(const std::string& path, std::string* why) {
    std::ifstream f(path);
    if (!f) { if (why) *why = "cannot open ADC file " + path; return {}; }
    try { return json::parse(f); } catch (const std::exception& e) { if (why) *why = std::string("ADC parse: ") + e.what(); return {}; }
}

std::string TokenUrl(const json& adc) {
    if (auto e = Env("OPENCLAW_GOOGLE_TOKEN_URL"); !e.empty()) return e;
    if (adc.contains("token_uri") && adc["token_uri"].is_string()) return adc["token_uri"].get<std::string>();
    return "https://oauth2.googleapis.com/token";
}

std::mutex g_mu;
GoogleToken g_cached;

std::optional<GoogleToken> RefreshAuthorizedUser(const json& adc, const std::string& path, std::string* why) {
    const std::string cid = adc.value("client_id", "");
    const std::string csec = adc.value("client_secret", "");
    const std::string rt = adc.value("refresh_token", "");
    if (cid.empty() || csec.empty() || rt.empty()) { if (why) *why = "ADC authorized_user missing client_id/secret/refresh_token"; return std::nullopt; }
    const std::string form = "grant_type=refresh_token&client_id=" + UrlEncode(cid) + "&client_secret=" + UrlEncode(csec) +
                             "&refresh_token=" + UrlEncode(rt);
    auto j = HttpPostForm(TokenUrl(adc), form);
    if (j.value("_http", 0) < 200 || j.value("_http", 0) >= 300 || !j.contains("access_token")) {
        if (why) *why = "ADC refresh failed: " + j.value("error", j.dump().substr(0, 200));
        return std::nullopt;
    }
    GoogleToken t;
    t.access_token = j["access_token"].get<std::string>();
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const long long life = j.value("expires_in", 3600);
    t.expires_at_ms = now + life * 1000;
    (void)path;
    return t;
}

std::string SignRs256(const std::string& pem, const std::string& signing_input, std::string* why) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) { if (why) *why = "BIO failed"; return {}; }
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) { if (why) *why = "PEM private key load failed"; return {}; }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    std::size_t sl = 0;
    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) != 1 ||
        EVP_DigestSign(ctx, nullptr, &sl, reinterpret_cast<const unsigned char*>(signing_input.data()), signing_input.size()) != 1) {
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(key); if (why) *why = "DigestSignInit failed"; return {};
    }
    std::vector<unsigned char> sig(sl);
    if (EVP_DigestSign(ctx, sig.data(), &sl, reinterpret_cast<const unsigned char*>(signing_input.data()), signing_input.size()) != 1) {
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(key); if (why) *why = "DigestSign failed"; return {};
    }
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(key);
    return B64Url(sig.data(), sl);
}

std::optional<GoogleToken> RefreshServiceAccount(const json& adc, std::string* why) {
    const std::string email = adc.value("client_email", "");
    const std::string pem = adc.value("private_key", "");
    if (email.empty() || pem.empty()) { if (why) *why = "ADC service_account missing client_email/private_key"; return std::nullopt; }
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    json header = {{"alg", "RS256"}, {"typ", "JWT"}};
    json claim = {{"iss", email},
                  {"scope", "https://www.googleapis.com/auth/cloud-platform"},
                  {"aud", TokenUrl(adc)},
                  {"iat", now},
                  {"exp", now + 3600}};
    const std::string input = B64UrlStr(header.dump()) + "." + B64UrlStr(claim.dump());
    const std::string sig = SignRs256(pem, input, why);
    if (sig.empty()) return std::nullopt;
    const std::string assertion = input + "." + sig;
    const std::string form = "grant_type=" + UrlEncode("urn:ietf:params:oauth:grant-type:jwt-bearer") +
                             "&assertion=" + UrlEncode(assertion);
    auto j = HttpPostForm(TokenUrl(adc), form);
    if (j.value("_http", 0) < 200 || j.value("_http", 0) >= 300 || !j.contains("access_token")) {
        if (why) *why = "ADC JWT grant failed: " + j.value("error", j.dump().substr(0, 200));
        return std::nullopt;
    }
    GoogleToken t;
    t.access_token = j["access_token"].get<std::string>();
    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    t.expires_at_ms = now_ms + j.value("expires_in", 3600) * 1000LL;
    return t;
}

}  // namespace

AwsCreds ResolveAwsFromEnv() {
    AwsCreds c;
    c.access_key_id = Env("AWS_ACCESS_KEY_ID");
    c.secret_access_key = Env("AWS_SECRET_ACCESS_KEY");
    c.session_token = Env("AWS_SESSION_TOKEN");
    c.region = Env("AWS_REGION");
    if (c.region.empty()) c.region = Env("AWS_DEFAULT_REGION");
    if (c.region.empty()) c.region = "us-east-1";
    c.bearer_token = Env("AWS_BEARER_TOKEN_BEDROCK");
    return c;
}

std::vector<std::string> SignAwsV4(const AwsCreds& c, const std::string& method, const std::string& host,
                                   const std::string& canonical_uri, const std::string& canonical_query,
                                   const std::string& payload, const std::string& service) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm gmt{};
#if defined(_WIN32)
    gmtime_s(&gmt, &tt);
#else
    gmtime_r(&tt, &gmt);
#endif
    char amz[32], datestamp[16];
    std::strftime(amz, sizeof(amz), "%Y%m%dT%H%M%SZ", &gmt);
    std::strftime(datestamp, sizeof(datestamp), "%Y%m%d", &gmt);
    const std::string payload_hash = Sha256Hex(payload);
    const std::string signed_headers = c.session_token.empty()
                                           ? "content-type;host;x-amz-content-sha256;x-amz-date"
                                           : "content-type;host;x-amz-content-sha256;x-amz-date;x-amz-security-token";
    std::ostringstream canon;
    canon << method << "\n" << (canonical_uri.empty() ? "/" : canonical_uri) << "\n" << canonical_query << "\n"
          << "content-type:application/json\n"
          << "host:" << host << "\n"
          << "x-amz-content-sha256:" << payload_hash << "\n"
          << "x-amz-date:" << amz << "\n";
    if (!c.session_token.empty()) canon << "x-amz-security-token:" << c.session_token << "\n";
    canon << "\n" << signed_headers << "\n" << payload_hash;
    const std::string credential_scope = std::string(datestamp) + "/" + c.region + "/" + service + "/aws4_request";
    const std::string string_to_sign = "AWS4-HMAC-SHA256\n" + std::string(amz) + "\n" + credential_scope + "\n" + Sha256Hex(canon.str());
    const std::string k_date = HmacSha256Raw("AWS4" + c.secret_access_key, datestamp);
    const std::string k_region = HmacSha256Raw(k_date, c.region);
    const std::string k_service = HmacSha256Raw(k_region, service);
    const std::string k_signing = HmacSha256Raw(k_service, "aws4_request");
    const std::string sig_raw = HmacSha256Raw(k_signing, string_to_sign);
    const std::string signature = HexLower(reinterpret_cast<const unsigned char*>(sig_raw.data()), sig_raw.size());
    std::vector<std::string> h;
    h.push_back("X-Amz-Date: " + std::string(amz));
    h.push_back("X-Amz-Content-Sha256: " + payload_hash);
    if (!c.session_token.empty()) h.push_back("X-Amz-Security-Token: " + c.session_token);
    h.push_back("Authorization: AWS4-HMAC-SHA256 Credential=" + c.access_key_id + "/" + credential_scope +
                ", SignedHeaders=" + signed_headers + ", Signature=" + signature);
    return h;
}

std::optional<GoogleToken> ResolveGoogleAdcAccessToken(std::string* why) {
    std::lock_guard<std::mutex> lock(g_mu);
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    if (!g_cached.access_token.empty() && g_cached.expires_at_ms > now + 60000) return g_cached;
    const auto path = AdcPath();
    if (path.empty()) { if (why) *why = "no Google ADC file (set GOOGLE_APPLICATION_CREDENTIALS)"; return std::nullopt; }
    auto adc = ReadAdc(path, why);
    if (!adc.is_object()) return std::nullopt;
    const std::string type = adc.value("type", "");
    std::optional<GoogleToken> t;
    if (type == "authorized_user") t = RefreshAuthorizedUser(adc, path, why);
    else if (type == "service_account") t = RefreshServiceAccount(adc, why);
    else { if (why) *why = "unsupported ADC type \"" + type + "\""; return std::nullopt; }
    if (t) g_cached = *t;
    return t;
}

void ResetGoogleAdcCacheForTest() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_cached = {};
}

}  // namespace openclaw::provider::cloud_auth
