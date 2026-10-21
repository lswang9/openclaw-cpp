#include "openclaw/session/store.hpp"
#include <cctype>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace openclaw::session {
namespace fs = std::filesystem;
std::string NewSessionIdPublic();

Store::Store(fs::path root) : root_(std::move(root)) {
    fs::create_directories(root_);
}

std::shared_ptr<std::recursive_mutex> Store::MutexFor(const std::string& id) const {
    std::lock_guard<std::recursive_mutex> g(map_mu_);
    auto it = locks_.find(id);
    if (it != locks_.end()) return it->second;
    auto m = std::make_shared<std::recursive_mutex>();
    locks_[id] = m;
    return m;
}

Store::Guard Store::Lock(const std::string& id) const { return Guard(MutexFor(id)); }

namespace {
std::string EncodeKey(const std::string& id) {
    static const char* hex = "0123456789abcdef";
    std::string enc;
    for (unsigned char c : id) {
        if (std::isalnum(c) || c == '-' || c == '_') enc.push_back(static_cast<char>(c));
        else { enc.push_back('~'); enc.push_back(hex[c >> 4]); enc.push_back(hex[c & 15]); }
    }
    if (enc.empty()) enc = "default";
    if (enc.size() > 200) enc = enc.substr(0, 180) + "~h" + std::to_string(std::hash<std::string>{}(id));
    return enc;
}
}  // namespace

// Read path: <enc>.jsonl (upstream) > <enc>.json (1.29/1.30) > pre-1.29 stripped .json (unindexed keys only).
fs::path Store::PathFor(const std::string& id) const {
    const auto enc = EncodeKey(id);
    fs::path jl = root_ / (enc + ".jsonl");
    if (fs::exists(jl)) return jl;
    fs::path p = root_ / (enc + ".json");
    if (fs::exists(p)) return p;
    auto legacy = LegacyPathFor(id);
    if (legacy != p && fs::exists(legacy)) {
        std::lock_guard<std::recursive_mutex> ig(index_mu_);
        if (!ReadIndex().contains(id)) return legacy;
    }
    return jl;
}

fs::path Store::WritePathFor(const std::string& id) const { return root_ / (EncodeKey(id) + ".jsonl"); }

fs::path Store::LegacyPathFor(const std::string& id) const {
    std::string safe;
    for (char c : id) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            safe.push_back(c);
        }
    }
    if (safe.empty()) safe = "default";
    return root_ / (safe + ".json");
}

std::vector<std::string> Store::List() const {
    std::vector<std::string> ids;
    if (!fs::exists(root_)) return ids;
    for (const auto& e : fs::directory_iterator(root_)) {
        const auto ext = e.path().extension();
        if ((ext != ".json" && ext != ".jsonl") || e.path().filename() == "sessions.json") continue;
        std::string stem = e.path().stem().string(), dec;
        for (std::size_t i = 0; i < stem.size(); ++i) {
            if (stem[i] == '~' && i + 2 < stem.size() && std::isxdigit(static_cast<unsigned char>(stem[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(stem[i + 2]))) {
                dec.push_back(static_cast<char>(std::stoi(stem.substr(i + 1, 2), nullptr, 16))); i += 2;
            } else dec.push_back(stem[i]);
        }
        if (std::find(ids.begin(), ids.end(), dec) == ids.end()) ids.push_back(dec);
    }
    return ids;
}

bool Store::Exists(const std::string& id) const {
    auto g = Lock(id);
    return fs::exists(PathFor(id));
}

std::vector<agent::Message> ParseTranscript(const fs::path& path) {
    std::vector<agent::Message> msgs;
    std::ifstream in(path);
    if (path.extension() == ".jsonl") {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                auto j = nlohmann::json::parse(line);
                if (j.value("type", "") == "message" && j.contains("message")) msgs.push_back(agent::Message::FromJson(j["message"]));
            } catch (...) {}  // tolerate a torn last line
        }
        return msgs;
    }
    nlohmann::json j;
    try { in >> j; } catch (...) { return msgs; }
    if (j.contains("messages") && j["messages"].is_array())
        for (const auto& m : j["messages"]) msgs.push_back(agent::Message::FromJson(m));
    return msgs;
}

std::vector<agent::Message> Store::Load(const std::string& id) const {
    auto g = Lock(id);
    const auto path = PathFor(id);
    if (!fs::exists(path)) return {};
    return ParseTranscript(path);
}

void Store::Save(const std::string& id, const std::vector<agent::Message>& messages) const {
    auto g = Lock(id);
    const auto src = PathFor(id);
    const auto dst = WritePathFor(id);
    if (!IndexEntry(id).is_object()) Touch(id, 0);
    auto entry = IndexEntry(id);
    std::string sid = entry.is_object() ? entry.value("sessionId", std::string()) : std::string();
    if (sid.empty()) sid = NewSessionIdPublic();
    auto ts = [] {
        auto t = std::chrono::system_clock::now();
        std::time_t tt = std::chrono::system_clock::to_time_t(t);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() % 1000;
        std::tm u{}; gmtime_r(&tt, &u);
        char buf[40]; std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &u);
        char out[48]; std::snprintf(out, sizeof out, "%s.%03dZ", buf, static_cast<int>(ms));
        return std::string(out);
    }();
    auto tmp = fs::path(dst.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << nlohmann::json{{"type", "session"}, {"version", 3}, {"id", sid}, {"key", id}, {"timestamp", ts}}.dump() << "\n";
        std::string parent;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            std::ostringstream mid; mid << std::hex << std::setw(8) << std::setfill('0') << (i + 1);
            nlohmann::json line{{"type", "message"}, {"id", mid.str()}, {"timestamp", ts}, {"message", messages[i].ToJson()}};
            line["parentId"] = parent.empty() ? nlohmann::json(nullptr) : nlohmann::json(parent);
            out << line.dump() << "\n";
            parent = mid.str();
        }
    }
    fs::rename(tmp, dst);
    // migrate this key's own old-format file (never a stripped legacy name that may belong to another key)
    if (src != dst && src == root_ / (EncodeKey(id) + ".json")) {
        std::error_code ec;
        fs::rename(src, fs::path(src.string() + ".migrated"), ec);
    }
    Touch(id, messages.size());
}

void Store::Append(const std::string& id, const agent::Message& message) const {
    auto g = Lock(id);
    auto msgs = Load(id);
    msgs.push_back(message);
    Save(id, msgs);
}

bool Store::Remove(const std::string& id) const {
    auto g = Lock(id);
    const auto path = PathFor(id);
    {
        std::lock_guard<std::recursive_mutex> ig(index_mu_);
        auto ix = ReadIndex();
        if (ix.contains(id)) { ix.erase(id); WriteIndex(ix); }
    }
    return fs::remove(path);
}

namespace {
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string NewSessionId() {
    std::random_device rd;
    std::mt19937_64 g(rd() ^ static_cast<std::uint64_t>(NowMs()));
    std::ostringstream o;
    o << std::hex << std::setfill('0') << std::setw(16) << g() << std::setw(16) << g();
    auto s = o.str();
    return s.substr(0, 8) + "-" + s.substr(8, 4) + "-4" + s.substr(13, 3) + "-a" + s.substr(17, 3) + "-" + s.substr(20, 12);
}
}  // namespace

std::string NewSessionIdPublic() { return NewSessionId(); }

nlohmann::json Store::ReadIndex() const {
    auto p = root_ / "sessions.json";
    if (!fs::exists(p)) return nlohmann::json::object();
    try {
        std::ifstream in(p);
        nlohmann::json j; in >> j;
        return j.is_object() ? j : nlohmann::json::object();
    } catch (...) { return nlohmann::json::object(); }
}

void Store::WriteIndex(const nlohmann::json& j) const {
    auto tmp = root_ / "sessions.json.tmp";
    { std::ofstream out(tmp); out << j.dump(2); }
    fs::rename(tmp, root_ / "sessions.json");
}

void Store::Touch(const std::string& key, std::size_t messages) const {
    std::lock_guard<std::recursive_mutex> ig(index_mu_);
    auto ix = ReadIndex();
    auto& e = ix[key];
    auto now = NowMs();
    if (!e.is_object()) e = nlohmann::json::object();
    if (!e.contains("sessionId")) e["sessionId"] = NewSessionId();
    if (!e.contains("createdAt")) e["createdAt"] = now;
    e["updatedAt"] = now;
    e["messages"] = messages;
    WriteIndex(ix);
}

nlohmann::json Store::IndexEntry(const std::string& key) const {
    std::lock_guard<std::recursive_mutex> ig(index_mu_);
    auto ix = ReadIndex();
    return ix.contains(key) ? ix[key] : nlohmann::json();
}

nlohmann::json Store::IndexJson() const {
    std::lock_guard<std::recursive_mutex> ig(index_mu_);
    return ReadIndex();
}

nlohmann::json Store::Reset(const std::string& key, const std::string& reason) const {
    auto g = Lock(key);
    std::lock_guard<std::recursive_mutex> ig(index_mu_);
    auto ix = ReadIndex();
    auto old = ix.contains(key) ? ix[key] : nlohmann::json::object();
    auto path = PathFor(key);
    if (fs::exists(path)) {
        fs::create_directories(root_ / "archive");
        std::string sid = old.value("sessionId", std::string("legacy"));
        fs::rename(path, root_ / "archive" / (path.stem().string() + "." + sid + path.extension().string()));
    }
    auto now = NowMs();
    nlohmann::json e{{"sessionId", NewSessionId()}, {"createdAt", now}, {"updatedAt", now}, {"messages", 0},
                     {"resets", old.value("resets", 0) + 1}, {"lastResetReason", reason}};
    if (old.contains("sessionId")) e["previousSessionId"] = old["sessionId"];
    ix[key] = e;
    WriteIndex(ix);
    return e;
}

bool Store::ResetIfStale(const std::string& key, const ResetPolicy& p, std::int64_t now_ms) const {
    if (now_ms == 0) now_ms = NowMs();
    auto e = IndexEntry(key);
    if (!e.is_object() || !e.contains("updatedAt")) return false;
    if (!IsStale(p, e.value("updatedAt", std::int64_t{0}), now_ms)) return false;
    bool idle = p.idle_minutes > 0 && now_ms - e.value("updatedAt", std::int64_t{0}) >= std::int64_t(p.idle_minutes) * 60000;
    Reset(key, idle ? "idle" : "daily");
    return true;
}

ResetPolicy PolicyFromConfig(const nlohmann::json& raw) {
    ResetPolicy p;
    if (!raw.is_object() || !raw.contains("session") || !raw["session"].is_object()) return p;
    auto r = raw["session"].value("reset", nlohmann::json::object());
    if (!r.is_object()) return p;
    p.mode = r.value("mode", p.mode);
    p.at_hour = r.value("atHour", p.at_hour);
    p.idle_minutes = r.value("idleMinutes", p.idle_minutes);
    return p;
}

namespace {
void ApplyPolicy(ResetPolicy& p, const nlohmann::json& r) {
    if (!r.is_object()) return;
    p.mode = r.value("mode", p.mode);
    p.at_hour = r.value("atHour", p.at_hour);
    p.idle_minutes = r.value("idleMinutes", p.idle_minutes);
}
}  // namespace

ResetPolicy PolicyFor(const nlohmann::json& raw, const std::string& channel, const std::string& chat_type) {
    ResetPolicy p = PolicyFromConfig(raw);
    if (!raw.is_object() || !raw.contains("session") || !raw["session"].is_object()) return p;
    const auto& s = raw["session"];
    std::string t = chat_type == "dm" ? "direct" : chat_type;
    if (!t.empty() && s.contains("resetByType") && s["resetByType"].is_object() && s["resetByType"].contains(t))
        ApplyPolicy(p, s["resetByType"][t]);
    if (!channel.empty() && s.contains("resetByChannel") && s["resetByChannel"].is_object() &&
        s["resetByChannel"].contains(channel))
        ApplyPolicy(p, s["resetByChannel"][channel]);
    return p;
}

std::string InferChatType(const std::string& key) {
    if (key.find(":thread:") != std::string::npos || key.find(":topic:") != std::string::npos) return "thread";
    if (key.find(":group:") != std::string::npos || key.find(":channel:") != std::string::npos) return "group";
    return "direct";
}

bool IsStale(const ResetPolicy& p, std::int64_t updated_ms, std::int64_t now_ms) {
    if (p.mode == "off" || updated_ms <= 0 || now_ms <= updated_ms) return false;
    if (p.idle_minutes > 0 && now_ms - updated_ms >= std::int64_t(p.idle_minutes) * 60000) return true;
    if (p.mode != "daily") return false;
    // Most recent boundary (local time at_hour:00) <= now; stale if updated before it.
    std::time_t nt = static_cast<std::time_t>(now_ms / 1000);
    std::tm lt{};
    localtime_r(&nt, &lt);
    std::tm b = lt; b.tm_hour = p.at_hour; b.tm_min = 0; b.tm_sec = 0; b.tm_isdst = -1;
    std::time_t bt = std::mktime(&b);
    if (bt > nt) { b.tm_mday -= 1; b.tm_isdst = -1; bt = std::mktime(&b); }
    return updated_ms < static_cast<std::int64_t>(bt) * 1000;
}

bool ParseResetCommand(const std::string& text, std::string& rest) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    for (const char* cmd : {"/new", "/reset"}) {
        std::string c = cmd;
        if (text.compare(i, c.size(), c) == 0 &&
            (i + c.size() == text.size() || std::isspace(static_cast<unsigned char>(text[i + c.size()])))) {
            rest = text.substr(i + c.size());
            auto s = rest.find_first_not_of(" \t\r\n");
            rest = s == std::string::npos ? "" : rest.substr(s);
            return true;
        }
    }
    return false;
}

}  // namespace openclaw::session
