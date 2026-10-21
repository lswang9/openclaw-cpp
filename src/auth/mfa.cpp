#include "openclaw/auth/mfa.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace openclaw::auth::mfa {
namespace fs = std::filesystem;
namespace {
std::mutex g_mu;
const char* kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
std::uint64_t Now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}
std::string Sha256Hex(const std::string& s) {
    unsigned char h[32]; unsigned int l = 0;
    EVP_Digest(s.data(), s.size(), h, &l, EVP_sha256(), nullptr);
    static const char* x = "0123456789abcdef";
    std::string o;
    for (unsigned i = 0; i < l; ++i) { o += x[h[i] >> 4]; o += x[h[i] & 15]; }
    return o;
}
nlohmann::json LoadJ(const std::string& p) {
    std::ifstream f(p);
    if (!f) return nlohmann::json::object();
    try { return nlohmann::json::parse(f); } catch (...) { return nlohmann::json::object(); }
}
void SaveJ(const std::string& p, const nlohmann::json& j) {
    fs::create_directories(fs::path(p).parent_path());
    const auto tmp = p + ".tmp";
    { std::ofstream f(tmp, std::ios::trunc); f << j.dump(2); }
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    fs::rename(tmp, p);
}
std::string EnvSecret() { const char* s = std::getenv("OPENCLAW_MFA_TOTP_SECRET"); return s ? s : ""; }
std::string Normalize(std::string c) {
    std::string o;
    for (char ch : c) if (ch != ' ' && ch != '-') o += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return o;
}
}  // namespace

std::string Base32Encode(const Bytes& b) {
    std::string o; std::uint32_t buf = 0; int bits = 0;
    for (auto c : b) { buf = (buf << 8) | c; bits += 8; while (bits >= 5) { o += kAlpha[(buf >> (bits - 5)) & 31]; bits -= 5; } }
    if (bits) o += kAlpha[(buf << (5 - bits)) & 31];
    return o;
}

std::optional<Bytes> Base32Decode(const std::string& s) {
    Bytes o; std::uint32_t buf = 0; int bits = 0;
    for (char ch : s) {
        if (ch == '=' || ch == ' ' || ch == '-') continue;
        char u = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        int v = (u >= 'A' && u <= 'Z') ? u - 'A' : (u >= '2' && u <= '7') ? 26 + u - '2' : -1;
        if (v < 0) return std::nullopt;
        buf = (buf << 5) | static_cast<std::uint32_t>(v); bits += 5;
        if (bits >= 8) { o.push_back(static_cast<std::uint8_t>((buf >> (bits - 8)) & 0xff)); bits -= 8; }
    }
    if (o.empty()) return std::nullopt;
    return o;
}

std::string Hotp(const Bytes& key, std::uint64_t counter, int digits) {
    unsigned char msg[8];
    for (int i = 7; i >= 0; --i) { msg[i] = static_cast<unsigned char>(counter & 0xff); counter >>= 8; }
    unsigned char h[20]; unsigned int l = 0;
    HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()), msg, 8, h, &l);
    const int off = h[19] & 0x0f;
    std::uint32_t bin = (std::uint32_t(h[off] & 0x7f) << 24) | (std::uint32_t(h[off + 1]) << 16) | (std::uint32_t(h[off + 2]) << 8) | h[off + 3];
    std::uint32_t mod = 1; for (int i = 0; i < digits; ++i) mod *= 10;
    std::string s = std::to_string(bin % mod);
    while (static_cast<int>(s.size()) < digits) s = "0" + s;
    return s;
}

std::string Totp(const Bytes& key, std::uint64_t unix_s, int step, int digits) { return Hotp(key, unix_s / step, digits); }

std::string DefaultPath() {
    if (const char* p = std::getenv("OPENCLAW_MFA_FILE"); p && *p) return p;
    fs::path root;
    if (const char* d = std::getenv("OPENCLAW_STATE_DIR"); d && *d) root = d;
    else if (const char* d2 = std::getenv("OPENCLAW_DATA_DIR"); d2 && *d2) root = d2;
    else root = fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".openclaw";
    return (root / "auth" / "mfa.json").string();
}

Store::Store(std::string path) : path_(std::move(path)) {}
bool Store::FromEnv() const { return !EnvSecret().empty(); }
bool Store::Enabled() const {
    if (FromEnv()) return true;
    std::lock_guard<std::mutex> lk(g_mu);
    return !LoadJ(path_).value("secret", std::string()).empty();
}

EnrollResult Store::Enroll(const std::string& account) {
    std::lock_guard<std::mutex> lk(g_mu);
    Bytes key(20); RAND_bytes(key.data(), 20);
    EnrollResult r;
    r.secret_b32 = Base32Encode(key);
    r.otpauth_uri = "otpauth://totp/OpenClaw:" + account + "?secret=" + r.secret_b32 + "&issuer=OpenClaw&algorithm=SHA1&digits=6&period=30";
    nlohmann::json rc = nlohmann::json::array();
    for (int i = 0; i < 8; ++i) {
        Bytes b(5); RAND_bytes(b.data(), 5);
        auto c = Base32Encode(b);  // 8 chars
        c = c.substr(0, 4) + "-" + c.substr(4, 4);
        r.recovery_codes.push_back(c);
        rc.push_back(Sha256Hex(Normalize(c)));
    }
    SaveJ(path_, {{"secret", r.secret_b32}, {"lastCounter", 0}, {"recovery", rc}, {"enrolledAt", Now()}});
    return r;
}

bool Store::Disable() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::error_code ec;
    return fs::remove(path_, ec);
}

std::string Store::CurrentCode(std::uint64_t now_s) const {
    std::string sec = EnvSecret();
    if (sec.empty()) { std::lock_guard<std::mutex> lk(g_mu); sec = LoadJ(path_).value("secret", std::string()); }
    auto k = Base32Decode(sec);
    return k ? Totp(*k, now_s ? now_s : Now()) : std::string();
}

int Store::RecoveryRemaining() const {
    std::lock_guard<std::mutex> lk(g_mu);
    auto j = LoadJ(path_);
    return j.contains("recovery") ? static_cast<int>(j["recovery"].size()) : 0;
}

bool Store::Verify(const std::string& code_in, std::uint64_t now_s, std::string* how) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto j = LoadJ(path_);
    std::string sec = EnvSecret();
    if (sec.empty()) sec = j.value("secret", std::string());
    auto key = Base32Decode(sec);
    if (!key) return false;
    const std::string code = Normalize(code_in);
    const std::uint64_t t = now_s ? now_s : Now(), ctr = t / 30;
    const std::uint64_t last = j.value("lastCounter", std::uint64_t{0});
    if (code.size() == 6) {
        for (std::uint64_t c : {ctr - 1, ctr, ctr + 1}) {
            if (c <= last) continue;
            const auto want = Hotp(*key, c);
            if (CRYPTO_memcmp(want.data(), code.data(), 6) == 0) {
                j["lastCounter"] = c;
                SaveJ(path_, j);
                if (how) *how = "totp";
                return true;
            }
        }
        return false;
    }
    if (j.contains("recovery")) {
        const auto h = Sha256Hex(code.size() == 8 ? code.substr(0, 4) + code.substr(4) : code);
        auto& arr = j["recovery"];
        for (auto it = arr.begin(); it != arr.end(); ++it) {
            if (it->get<std::string>() == h) {
                arr.erase(it);
                SaveJ(path_, j);
                if (how) *how = "recovery";
                return true;
            }
        }
    }
    return false;
}

}  // namespace openclaw::auth::mfa
