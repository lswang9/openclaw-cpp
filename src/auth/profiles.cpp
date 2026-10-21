#include "openclaw/auth/profiles.hpp"

#include "openclaw/auth/keychain.hpp"
#include "openclaw/util.hpp"

#include <sys/stat.h>

#include <chrono>
#include <fstream>
#include <mutex>

namespace openclaw::auth {
namespace fs = std::filesystem;
namespace {
std::mutex g_mu;
long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
constexpr const char* kSecretFields[] = {"key", "access", "refresh", "token"};
// Keychain id is scoped by file path so per-agent stores do not collide.
std::string KcId(const fs::path& p, const std::string& id) { return p.string() + "#" + id; }
std::string Mask(const std::string& s) { return s.size() <= 8 ? std::string(s.size(), '*') : s.substr(0, 4) + "…" + s.substr(s.size() - 4); }
}  // namespace

fs::path DefaultProfilesPath(const std::string& agent_id) {
    std::string st = EnvOr("OPENCLAW_STATE_DIR", "");
    fs::path base = st.empty() ? fs::path(EnvOr("HOME", "/tmp")) / ".openclaw" : fs::path(st);
    return base / "agents" / agent_id / "agent" / "auth-profiles.json";
}

ProfileStore::ProfileStore(fs::path path) : path_(std::move(path)) {}

nlohmann::json ProfileStore::Load() const {
    std::ifstream in(path_);
    auto j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
    if (!j.is_object()) j = nlohmann::json::object();
    if (!j.contains("profiles") || !j["profiles"].is_object()) j["profiles"] = nlohmann::json::object();
    j["version"] = 1;
    return j;
}

void ProfileStore::Save(const nlohmann::json& doc) const {
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);
    const auto tmp = path_.string() + ".tmp";
    {
        mode_t old = ::umask(077);
        std::ofstream o(tmp, std::ios::trunc);
        o << doc.dump(2) << "\n";
        ::umask(old);
    }
    ::chmod(tmp.c_str(), 0600);
    fs::rename(tmp, path_, ec);
}

bool ProfileStore::Upsert(const std::string& id, const nlohmann::json& cred) {
    if (id.empty() || !cred.is_object()) return false;
    std::lock_guard l(g_mu);
    auto d = Load();
    auto c = cred;
    c["updatedAt"] = NowMs();
    c.erase("keychain");
    if (keychain::Available()) {
        nlohmann::json moved = nlohmann::json::array();
        for (const char* f : kSecretFields)
            if (c.contains(f) && c[f].is_string() && keychain::Store(KcId(path_, id), f, c[f].get<std::string>())) {
                c.erase(f);
                moved.push_back(f);
            }
        if (!moved.empty()) c["keychain"] = {{"backend", "libsecret"}, {"fields", moved}};
    }
    d["profiles"][id] = c;
    Save(d);
    return true;
}

bool ProfileStore::Remove(const std::string& id) {
    std::lock_guard l(g_mu);
    auto d = Load();
    if (!d["profiles"].contains(id)) return false;
    for (const char* f : kSecretFields) keychain::Clear(KcId(path_, id), f);
    d["profiles"].erase(id);
    Save(d);
    return true;
}

std::optional<nlohmann::json> ProfileStore::Get(const std::string& id) const {
    std::lock_guard l(g_mu);
    auto d = Load();
    if (!d["profiles"].contains(id)) return std::nullopt;
    auto c = d["profiles"][id];
    if (c.is_object() && c.contains("keychain"))
        for (auto& f : c["keychain"].value("fields", nlohmann::json::array()))
            if (auto v = keychain::Lookup(KcId(path_, id), f.get<std::string>())) c[f.get<std::string>()] = *v;
    return c;
}

std::optional<std::pair<std::string, nlohmann::json>> ProfileStore::ForProvider(const std::string& provider) const {
    std::string pick;
    {
        std::lock_guard l(g_mu);
        auto d = Load();
        const auto& p = d["profiles"];
        if (p.contains(provider + ":default")) pick = provider + ":default";
        else
            for (auto& [k, v] : p.items())
                if (v.is_object() && (v.value("provider", "") == provider || k.rfind(provider + ":", 0) == 0)) { pick = k; break; }
    }
    if (pick.empty()) return std::nullopt;
    auto c = Get(pick);
    if (!c) return std::nullopt;
    return std::make_pair(pick, *c);
}

nlohmann::json ProfileStore::ListMasked() const {
    auto d = Load();
    nlohmann::json out = nlohmann::json::object();
    for (auto& [k, v] : d["profiles"].items()) {
        auto c = v;
        if (c.is_object() && c.contains("keychain")) if (auto h = Get(k)) c = *h;
        for (const char* s : {"key", "access", "refresh", "token"})
            if (c.contains(s) && c[s].is_string()) c[s] = Mask(c[s].get<std::string>());
        out[k] = c;
    }
    return {{"path", path_.string()}, {"profiles", out}, {"secretBackend", keychain::Backend()},
            {"keychainNote", keychain::UnavailableReason()}};
}

bool OAuthNeedsRefresh(const nlohmann::json& cred, long long skew) {
    if (!cred.is_object() || cred.value("type", "") != "oauth") return false;
    long long exp = cred.value("expires", 0LL);
    return exp > 0 && exp - skew <= NowMs();
}

}  // namespace openclaw::auth
