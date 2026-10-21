#include "openclaw/channels/signing.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstdlib>
#include <vector>

namespace openclaw::channels::signing {
namespace {
std::string Hex(const unsigned char* d, std::size_t n) {
    static const char* k = "0123456789abcdef";
    std::string o;
    for (std::size_t i = 0; i < n; ++i) { o += k[d[i] >> 4]; o += k[d[i] & 15]; }
    return o;
}
bool Unhex(const std::string& h, std::vector<unsigned char>& out) {
    if (h.size() % 2) return false;
    out.clear();
    for (std::size_t i = 0; i < h.size(); i += 2) {
        char* end = nullptr;
        std::string b = h.substr(i, 2);
        long v = std::strtol(b.c_str(), &end, 16);
        if (*end) return false;
        out.push_back(static_cast<unsigned char>(v));
    }
    return true;
}
std::string UrlDecode(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) { o += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
        else o += s[i];
    }
    return o;
}
}  // namespace

std::string SlackSignature(const std::string& secret, const std::string& ts, const std::string& body) {
    const std::string base = "v0:" + ts + ":" + body;
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), reinterpret_cast<const unsigned char*>(base.data()),
         base.size(), mac, &len);
    return "v0=" + Hex(mac, len);
}

bool VerifySlack(const std::string& secret, const std::string& ts, const std::string& body, const std::string& sig,
                 long long now, std::string* why) {
    if (ts.empty() || sig.empty()) { if (why) *why = "missing X-Slack-Signature/X-Slack-Request-Timestamp"; return false; }
    long long t = std::atoll(ts.c_str());
    if (t <= 0 || std::llabs(now - t) > 300) { if (why) *why = "stale slack request timestamp"; return false; }
    auto expect = SlackSignature(secret, ts, body);
    if (expect.size() != sig.size() || CRYPTO_memcmp(expect.data(), sig.data(), sig.size()) != 0) {
        if (why) *why = "slack signature mismatch";
        return false;
    }
    return true;
}

bool VerifyDiscord(const std::string& pk_hex, const std::string& ts, const std::string& body, const std::string& sig_hex,
                   std::string* why) {
    std::vector<unsigned char> pk, sig;
    if (!Unhex(pk_hex, pk) || pk.size() != 32) { if (why) *why = "bad DISCORD_PUBLIC_KEY"; return false; }
    if (ts.empty() || !Unhex(sig_hex, sig) || sig.size() != 64) { if (why) *why = "missing/bad X-Signature-Ed25519"; return false; }
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pk.data(), pk.size());
    if (!key) { if (why) *why = "ed25519 key load failed"; return false; }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const std::string msg = ts + body;
    bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
              EVP_DigestVerify(ctx, sig.data(), sig.size(), reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (!ok && why) *why = "discord signature mismatch";
    return ok;
}

nlohmann::json ParseForm(const std::string& body) {
    nlohmann::json o = nlohmann::json::object();
    std::size_t i = 0;
    while (i <= body.size()) {
        auto amp = body.find('&', i);
        std::string kv = body.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
        if (!kv.empty()) {
            auto eq = kv.find('=');
            o[UrlDecode(kv.substr(0, eq))] = eq == std::string::npos ? "" : UrlDecode(kv.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    return o;
}

}  // namespace openclaw::channels::signing
