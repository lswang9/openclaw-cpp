#include "openclaw/auth/device_link.hpp"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <fstream>

namespace openclaw::auth {
namespace {
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string RandHex(int bytes) {
    std::string raw(bytes, '\0');
    RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()), bytes);
    static const char* h = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) { out += h[c >> 4]; out += h[c & 15]; }
    return out;
}
std::string RandCode() {  // 8 chars, no ambiguous glyphs
    static const char* a = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    unsigned char b[8];
    RAND_bytes(b, 8);
    std::string s;
    for (int i = 0; i < 8; ++i) { if (i == 4) s += '-'; s += a[b[i] % 32]; }
    return s;
}
std::int64_t TtlMs() {
    const char* v = std::getenv("OPENCLAW_PAIR_TTL_SEC");
    long s = (v && *v) ? std::atol(v) : 600;
    return static_cast<std::int64_t>(s) * 1000;
}
int EnvInt(const char* k, int d) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::atoi(v) : d;
}
std::string S(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string();
}
nlohmann::json Public(const nlohmann::json& l) {
    nlohmann::json o = l;
    o.erase("poll_secret_sha256");
    return o;
}
}  // namespace

std::string DeviceLinkStore::Sha256Hex(const std::string& s) {
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), d);
    static const char* h = "0123456789abcdef";
    std::string out;
    for (unsigned char c : d) { out += h[c >> 4]; out += h[c & 15]; }
    return out;
}

DeviceLinkStore::DeviceLinkStore(std::filesystem::path path) : path_(std::move(path)) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    Load();
}

void DeviceLinkStore::Load() {
    data_ = {{"version", 1}, {"links", nlohmann::json::array()}, {"tokens", nlohmann::json::array()}};
    std::ifstream in(path_);
    if (!in) return;
    try {
        nlohmann::json j; in >> j;
        if (j.is_object()) {
            if (j.contains("links") && j["links"].is_array()) data_["links"] = j["links"];
            if (j.contains("tokens") && j["tokens"].is_array()) data_["tokens"] = j["tokens"];
            if (j.contains("sessions") && j["sessions"].is_array()) data_["sessions"] = j["sessions"];
            if (j.contains("claim_rate") && j["claim_rate"].is_object()) data_["claim_rate"] = j["claim_rate"];
        }
    } catch (...) {}
}

void DeviceLinkStore::Save() const {
    auto tmp = path_; tmp += ".tmp";
    { std::ofstream out(tmp); out << data_.dump(2); }
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    std::filesystem::permissions(path_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
}

void DeviceLinkStore::ExpireLocked() {
    auto now = NowMs();
    for (auto& l : data_["links"]) {
        if (S(l, "state") == "pending" && l.value("expires_ms", (std::int64_t)0) < now) l["state"] = "expired";
    }
}

nlohmann::json* DeviceLinkStore::FindLink(const std::string& k) {
    for (auto& l : data_["links"]) if (S(l, "id") == k || S(l, "code") == k) return &l;
    return nullptr;
}

nlohmann::json DeviceLinkStore::Start(const nlohmann::json& p) {
    std::lock_guard lock(mu_);
    ExpireLocked();
    auto secret = RandHex(16);
    nlohmann::json l{{"id", "link-" + RandHex(6)}, {"code", RandCode()},
                     {"device_name", S(p, "device_name").empty() ? std::string("device") : S(p, "device_name")},
                     {"channel", S(p, "channel").empty() ? std::string("gateway") : S(p, "channel")},
                     {"state", "pending"}, {"created_ms", NowMs()}, {"expires_ms", NowMs() + TtlMs()},
                     {"poll_secret_sha256", Sha256Hex(secret)}};
    data_["links"].push_back(l);
    Save();
    auto out = Public(l);
    return {{"ok", true}, {"impl", "real"}, {"link", out}, {"poll_secret", secret},
            {"note", "show code to operator; approve in /pairing; then pairing.link.claim with poll_secret"}};
}

nlohmann::json DeviceLinkStore::Status(const std::string& id) const {
    std::lock_guard lock(mu_);
    auto now = NowMs();
    for (const auto& l : data_["links"]) {
        if (S(l, "id") == id || S(l, "code") == id) {
            auto o = Public(l);
            if (S(o, "state") == "pending" && o.value("expires_ms", (std::int64_t)0) < now) o["state"] = "expired";
            return {{"ok", true}, {"impl", "real"}, {"link", o}};
        }
    }
    return {{"ok", false}, {"error", "not found"}};
}

nlohmann::json DeviceLinkStore::List() const {
    std::lock_guard lock(mu_);
    nlohmann::json links = nlohmann::json::array(), toks = nlohmann::json::array();
    auto now = NowMs();
    for (const auto& l : data_["links"]) {
        auto o = Public(l);
        if (S(o, "state") == "pending" && o.value("expires_ms", (std::int64_t)0) < now) o["state"] = "expired";
        links.push_back(o);
    }
    for (const auto& t : data_["tokens"]) { auto o = t; o.erase("token_sha256"); toks.push_back(o); }
    return {{"ok", true}, {"impl", "real"}, {"links", links}, {"tokens", toks}, {"path", path_.string()}};
}

nlohmann::json DeviceLinkStore::Approve(const std::string& k, const std::string& role_in) {
    const std::string role = role_in.empty() ? "device" : role_in;
    if (!ValidRole(role)) return {{"ok", false}, {"error", "invalid role (device|operator)"}};
    std::lock_guard lock(mu_);
    ExpireLocked();
    auto* l = FindLink(k);
    if (!l) return {{"ok", false}, {"error", "not found"}};
    if (S(*l, "state") != "pending") return {{"ok", false}, {"error", "not pending"}, {"state", (*l)["state"]}};
    (*l)["state"] = "approved"; (*l)["decided_ms"] = NowMs(); (*l)["role"] = role;
    Save();
    return {{"ok", true}, {"impl", "real"}, {"link", Public(*l)}};
}

nlohmann::json DeviceLinkStore::Deny(const std::string& k) {
    std::lock_guard lock(mu_);
    ExpireLocked();
    auto* l = FindLink(k);
    if (!l) return {{"ok", false}, {"error", "not found"}};
    if (S(*l, "state") != "pending") return {{"ok", false}, {"error", "not pending"}, {"state", (*l)["state"]}};
    (*l)["state"] = "denied"; (*l)["decided_ms"] = NowMs();
    Save();
    return {{"ok", true}, {"impl", "real"}, {"link", Public(*l)}};
}

namespace { thread_local std::string g_client_ip; }
void DeviceLinkStore::SetThreadClientIp(const std::string& ip) { g_client_ip = ip; }
std::string DeviceLinkStore::ThreadClientIp() { return g_client_ip; }

// Sliding 60s window per client IP, persisted in the store (survives restart).
bool DeviceLinkStore::ClaimRateLimitedLocked(std::int64_t now) {
    const int max_per_min = EnvInt("OPENCLAW_PAIR_CLAIM_MAX_PER_MIN", 30);
    const std::string ip = g_client_ip.empty() ? std::string("local") : g_client_ip;
    if (!data_.contains("claim_rate") || !data_["claim_rate"].is_object()) data_["claim_rate"] = nlohmann::json::object();
    auto& rate = data_["claim_rate"];
    for (auto it = rate.begin(); it != rate.end();) {  // prune all IPs
        nlohmann::json kept = nlohmann::json::array();
        if (it.value().is_array())
            for (auto& t0 : it.value()) if (t0.is_number() && now - t0.get<std::int64_t>() <= 60000) kept.push_back(t0);
        if (kept.empty() && it.key() != ip) it = rate.erase(it); else { it.value() = kept; ++it; }
    }
    auto& mine = rate[ip];
    if (!mine.is_array()) mine = nlohmann::json::array();
    if (static_cast<int>(mine.size()) >= max_per_min) return true;
    mine.push_back(now);
    Save();
    return false;
}

std::pair<std::string, std::string> DeviceLinkStore::CreateSession(const std::string& cred_sha, bool is_gw) {
    std::lock_guard lock(mu_);
    auto sid = "ocs_" + RandHex(24);
    auto csrf = RandHex(16);
    if (!data_.contains("sessions") || !data_["sessions"].is_array()) data_["sessions"] = nlohmann::json::array();
    const auto now = NowMs();
    auto& ss = data_["sessions"];
    ss.erase(std::remove_if(ss.begin(), ss.end(), [&](const nlohmann::json& s) {
        return s.value("expires_ms", (std::int64_t)0) < now; }), ss.end());
    ss.push_back({{"sid_sha256", Sha256Hex(sid)}, {"csrf", csrf}, {"cred_sha256", cred_sha},
                  {"gateway", is_gw}, {"created_ms", now},
                  {"expires_ms", now + (std::int64_t)EnvInt("OPENCLAW_SESSION_TTL_SEC", 2592000) * 1000}});
    Save();
    return {sid, csrf};
}

std::pair<std::string, std::string> DeviceLinkStore::CreateIdentitySession(const std::string& subject, const std::string& role) {
    std::lock_guard lock(mu_);
    auto sid = "ocs_" + RandHex(24);
    auto csrf = RandHex(16);
    if (!data_.contains("sessions") || !data_["sessions"].is_array()) data_["sessions"] = nlohmann::json::array();
    const auto now = NowMs();
    data_["sessions"].push_back({{"sid_sha256", Sha256Hex(sid)}, {"csrf", csrf}, {"oidc_sub", subject}, {"role", role},
                                 {"created_ms", now},
                                 {"expires_ms", now + (std::int64_t)EnvInt("OPENCLAW_OIDC_SESSION_TTL_SEC", 43200) * 1000}});
    Save();
    return {sid, csrf};
}

DeviceLinkStore::SessionInfo DeviceLinkStore::SessionLookup(const std::string& sid, const std::string& gw_sha) {
    if (sid.rfind("ocs_", 0) != 0) return {};
    std::lock_guard lock(mu_);
    if (!data_.contains("sessions")) return {};
    const auto h = Sha256Hex(sid);
    for (auto& s : data_["sessions"]) {
        if (S(s, "sid_sha256") != h) continue;
        if (s.value("expires_ms", (std::int64_t)0) < NowMs()) return {};
        if (s.contains("oidc_sub")) {
            const char* iss = std::getenv("OPENCLAW_OIDC_ISSUER");
            if (!iss || !*iss) return {};
            const auto r = S(s, "role");
            return {(r == "admin" || r == "operator") ? r : std::string(), S(s, "csrf")};
        }
        const auto cred = S(s, "cred_sha256");
        if (s.value("gateway", false)) {
            if (!gw_sha.empty() && cred == gw_sha) return {"admin", S(s, "csrf")};
            return {};
        }
        for (auto& t : data_["tokens"])
            if (S(t, "token_sha256") == cred && !t.value("revoked", false))
                return {t.contains("role") && t["role"].is_string() ? t["role"].get<std::string>() : "device",
                        S(s, "csrf")};
        return {};
    }
    return {};
}

void DeviceLinkStore::DeleteSession(const std::string& sid) {
    std::lock_guard lock(mu_);
    if (!data_.contains("sessions")) return;
    const auto h = Sha256Hex(sid);
    auto& ss = data_["sessions"];
    ss.erase(std::remove_if(ss.begin(), ss.end(), [&](const nlohmann::json& s) { return S(s, "sid_sha256") == h; }),
             ss.end());
    Save();
}

nlohmann::json DeviceLinkStore::Claim(const std::string& id, const std::string& secret) {
    std::lock_guard lock(mu_);
    ExpireLocked();
    const auto now = NowMs();
    if (ClaimRateLimitedLocked(now))
        return {{"ok", false}, {"error", "rate limited"}, {"rate_limited", true}, {"retry_after_sec", 60}};
    auto* l = FindLink(id);
    if (!l) return {{"ok", false}, {"error", "invalid link or poll_secret"}};
    if (S(*l, "state") == "locked")
        return {{"ok", false}, {"error", "link locked after too many bad secrets"}, {"state", "locked"}};
    if (secret.empty() || S(*l, "poll_secret_sha256") != Sha256Hex(secret)) {
        const int fails = l->value("failed_claims", 0) + 1;
        (*l)["failed_claims"] = fails;
        if (fails >= EnvInt("OPENCLAW_PAIR_MAX_BAD_SECRET", 5)) (*l)["state"] = "locked";
        Save();
        return {{"ok", false}, {"error", "invalid link or poll_secret"}, {"failed_claims", fails}};
    }
    auto st = S(*l, "state");
    if (st == "pending") return {{"ok", false}, {"pending", true}, {"state", st}};
    if (st != "approved") return {{"ok", false}, {"error", "link " + st}, {"state", st}};
    auto token = "ocd_" + RandHex(24);
    nlohmann::json t{{"id", "tok-" + RandHex(6)}, {"link_id", S(*l, "id")}, {"device_name", (*l)["device_name"]},
                     {"token_sha256", Sha256Hex(token)}, {"created_ms", NowMs()}, {"revoked", false},
                     {"role", l->contains("role") ? (*l)["role"] : nlohmann::json("device")}};
    data_["tokens"].push_back(t);
    (*l)["state"] = "claimed"; (*l)["token_id"] = t["id"];
    Save();
    return {{"ok", true}, {"impl", "real"}, {"token", token}, {"token_id", t["id"]}, {"role", t["role"]},
            {"note", "token shown once; only SHA-256 persisted"}};
}

nlohmann::json DeviceLinkStore::Verify(const std::string& token) const {
    std::lock_guard lock(mu_);
    if (token.empty()) return {{"ok", false}, {"valid", false}};
    auto h = Sha256Hex(token);
    for (const auto& t : data_["tokens"]) {
        if (S(t, "token_sha256") == h)
            return {{"ok", true}, {"valid", !t.value("revoked", false)}, {"token_id", t["id"]},
                    {"device_name", t["device_name"]},
                    {"role", (t.contains("role") && t["role"].is_string()) ? t["role"].get<std::string>() : "device"}};
    }
    return {{"ok", true}, {"valid", false}};
}

bool DeviceLinkStore::IsValidToken(const std::string& token) const {
    if (token.rfind("ocd_", 0) != 0) return false;
    auto v = Verify(token);
    return v.value("valid", false);
}

std::string DeviceLinkStore::TokenRole(const std::string& token) const {
    if (token.rfind("ocd_", 0) != 0) return {};
    auto v = Verify(token);
    if (!v.value("valid", false)) return {};
    return v.value("role", std::string("device"));
}

bool DeviceLinkStore::AcceptsHeaders(const std::string& authorization, const std::string& x_token) const {
    std::string bearer;
    if (authorization.size() > 7) {
        std::string pre = authorization.substr(0, 7);
        for (auto& ch : pre) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (pre == "bearer ") {
            bearer = authorization.substr(7);
            while (!bearer.empty() && (bearer.front() == ' ' || bearer.front() == '\t')) bearer.erase(bearer.begin());
            while (!bearer.empty() && (bearer.back() == ' ' || bearer.back() == '\r')) bearer.pop_back();
        }
    }
    return IsValidToken(bearer) || IsValidToken(x_token);
}

nlohmann::json DeviceLinkStore::Revoke(const std::string& k) {
    std::lock_guard lock(mu_);
    auto h = Sha256Hex(k);
    for (auto& t : data_["tokens"]) {
        if (S(t, "id") == k || S(t, "link_id") == k || S(t, "token_sha256") == h) {
            t["revoked"] = true; t["revoked_ms"] = NowMs();
            Save();
            return {{"ok", true}, {"impl", "real"}, {"revoked", t["id"]}};
        }
    }
    return {{"ok", false}, {"error", "token not found"}};
}

}  // namespace openclaw::auth
