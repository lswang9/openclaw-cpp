#include "openclaw/session/sqlite_backend.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <stdexcept>

#include "agent_schema_sql.inc"

namespace openclaw::session {
namespace {
using json = nlohmann::json;
std::mutex g_kind_mu;
std::string g_kind = "json";
long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db) + " :: " + sql);
    }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt& Text(int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); return *this; }
    Stmt& Null(int i) { sqlite3_bind_null(s, i); return *this; }
    Stmt& Int(int i, long long v) { sqlite3_bind_int64(s, i, v); return *this; }
    bool Step() {
        int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("sqlite step: ") + sqlite3_errmsg(sqlite3_db_handle(s)));
    }
    std::string Col(int i) { auto p = sqlite3_column_text(s, i); return p ? reinterpret_cast<const char*>(p) : ""; }
    long long ColInt(int i) { return sqlite3_column_int64(s, i); }
};
std::string ChatTypeOf(const std::string& key) {
    if (key.find(":group:") != std::string::npos) return "group";
    if (key.find(":channel:") != std::string::npos) return "channel";
    return "direct";
}
}  // namespace

void SetSessionBackend(const std::string& kind) { std::lock_guard<std::mutex> l(g_kind_mu); g_kind = kind == "sqlite" ? "sqlite" : "json"; }
std::string SessionBackend() {
    if (const char* e = std::getenv("OPENCLAW_SESSION_STORE"); e && *e) return std::string(e) == "sqlite" ? "sqlite" : "json";
    std::lock_guard<std::mutex> l(g_kind_mu);
    return g_kind;
}

SqliteBackend::SqliteBackend(const std::filesystem::path& root, const std::string& agent_id) : path_(root / "openclaw-agent.sqlite"), agent_id_(agent_id) {
    std::error_code ec; std::filesystem::create_directories(root, ec);
    if (sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot open " + path_.string());
    sqlite3_busy_timeout(db_, 5000);
    Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    long long ver = 0;
    { Stmt s(db_, "PRAGMA user_version"); if (s.Step()) ver = s.ColInt(0); }
    if (ver > kAgentSchemaVersion) throw std::runtime_error(path_.string() + " has newer schema v" + std::to_string(ver));
    if (ver < kAgentSchemaVersion) {
        std::string sql; for (auto* c : kAgentSchemaSql) sql += c;
        Exec("PRAGMA foreign_keys=OFF; BEGIN;");
        try { Exec(sql); Exec("PRAGMA user_version=" + std::to_string(kAgentSchemaVersion) + "; COMMIT;"); }
        catch (...) { Exec("ROLLBACK;"); throw; }
    }
    Exec("PRAGMA foreign_keys=ON;");
    const auto now = NowMs();
    Stmt s(db_, "INSERT INTO schema_meta (meta_key, role, schema_version, agent_id, app_version, created_at, updated_at) VALUES ('primary','agent',?,?,?,?,?) "
                "ON CONFLICT(meta_key) DO UPDATE SET schema_version=excluded.schema_version, agent_id=excluded.agent_id, app_version=excluded.app_version, updated_at=excluded.updated_at "
                "WHERE schema_meta.schema_version != excluded.schema_version OR schema_meta.app_version IS NOT excluded.app_version OR schema_meta.agent_id IS NOT excluded.agent_id");
    s.Int(1, kAgentSchemaVersion).Text(2, agent_id_).Text(3, "openclaw-cpp").Int(4, now).Int(5, now).Step();
}
SqliteBackend::~SqliteBackend() { if (db_) sqlite3_close_v2(db_); }

void SqliteBackend::Exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "sqlite error"; sqlite3_free(err);
        throw std::runtime_error("sqlite: " + e);
    }
}

void SqliteBackend::EnsureNodeAndWindow(const std::string& key, const json& e) {
    const auto sid = e.value("sessionId", std::string());
    const long long now = NowMs(), created = e.value("createdAt", now), updated = e.value("updatedAt", now);
    Stmt n(db_, "INSERT INTO session_nodes (session_key, current_session_id, entry_json, entry_valid, updated_at, created_at, last_activity_at) VALUES (?,?,?,1,?,?,?) "
                "ON CONFLICT(session_key) DO UPDATE SET current_session_id=excluded.current_session_id, entry_json=excluded.entry_json, updated_at=excluded.updated_at, "
                "last_activity_at=excluded.last_activity_at");
    n.Text(1, key).Text(2, sid).Text(3, e.dump()).Int(4, updated).Int(5, created).Int(6, updated).Step();
    Stmt w(db_, "INSERT INTO session_windows (session_id, session_key, previous_session_id, reason, created_at, updated_at, transcript_updated_at, chat_type) "
                "VALUES (?,?,?,?,?,?,?,?) ON CONFLICT(session_id) DO UPDATE SET updated_at=excluded.updated_at, transcript_updated_at=excluded.transcript_updated_at");
    w.Text(1, sid).Text(2, key);
    if (e.contains("previousSessionId") && e["previousSessionId"].is_string()) w.Text(3, e["previousSessionId"].get<std::string>()); else w.Null(3);
    w.Text(4, e.contains("previousSessionId") ? "reset" : "initial").Int(5, created).Int(6, updated).Int(7, updated).Text(8, ChatTypeOf(key)).Step();
}

std::vector<std::string> SqliteBackend::Keys() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::vector<std::string> out; Stmt s(db_, "SELECT session_key FROM session_nodes ORDER BY created_at, session_key");
    while (s.Step()) out.push_back(s.Col(0));
    return out;
}
bool SqliteBackend::Exists(const std::string& key) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Stmt s(db_, "SELECT 1 FROM session_nodes n JOIN transcript_events t ON t.session_id = n.current_session_id WHERE n.session_key=? LIMIT 1");
    s.Text(1, key); return s.Step();
}
json SqliteBackend::Entry(const std::string& key) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Stmt s(db_, "SELECT entry_json FROM session_nodes WHERE session_key=?"); s.Text(1, key);
    if (!s.Step()) return nullptr;
    auto j = json::parse(s.Col(0), nullptr, false); return j.is_object() ? j : json(nullptr);
}
json SqliteBackend::AllEntries() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    json out = json::object(); Stmt s(db_, "SELECT session_key, entry_json FROM session_nodes");
    while (s.Step()) { auto j = json::parse(s.Col(1), nullptr, false); if (j.is_object()) out[s.Col(0)] = j; }
    return out;
}
std::vector<json> SqliteBackend::Lines(const std::string& key) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::vector<json> out;
    Stmt s(db_, "SELECT t.event_json FROM session_nodes n JOIN transcript_events t ON t.session_id = n.current_session_id WHERE n.session_key=? ORDER BY t.seq");
    s.Text(1, key);
    while (s.Step()) { auto j = json::parse(s.Col(0), nullptr, false); if (!j.is_discarded()) out.push_back(j); }
    return out;
}
void SqliteBackend::Replace(const std::string& key, const json& entry, const std::vector<json>& lines) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Exec("BEGIN IMMEDIATE;");
    try {
        EnsureNodeAndWindow(key, entry);
        const auto sid = entry.value("sessionId", std::string());
        { Stmt d(db_, "DELETE FROM transcript_events WHERE session_id=?"); d.Text(1, sid).Step(); }
        const long long now = NowMs();
        Stmt ins(db_, "INSERT INTO transcript_events (session_id, seq, event_json, created_at) VALUES (?,?,?,?)");
        for (size_t i = 0; i < lines.size(); ++i) {
            sqlite3_reset(ins.s); ins.Text(1, sid).Int(2, static_cast<long long>(i)).Text(3, lines[i].dump()).Int(4, now).Step();
        }
        Exec("COMMIT;");
    } catch (...) { Exec("ROLLBACK;"); throw; }
}
void SqliteBackend::AppendLine(const std::string& key, const json& entry, const json& line) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Exec("BEGIN IMMEDIATE;");
    try {
        EnsureNodeAndWindow(key, entry);
        Stmt ins(db_, "INSERT INTO transcript_events (session_id, seq, event_json, created_at) "
                      "SELECT ?1, coalesce(max(seq)+1, 0), ?2, ?3 FROM transcript_events WHERE session_id=?1");
        ins.Text(1, entry.value("sessionId", std::string())).Text(2, line.dump()).Int(3, NowMs()).Step();
        Exec("COMMIT;");
    } catch (...) { Exec("ROLLBACK;"); throw; }
}
void SqliteBackend::PutEntry(const std::string& key, const json& entry) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    EnsureNodeAndWindow(key, entry);
}
void SqliteBackend::NewWindow(const std::string& key, const json& entry, const std::string& prev, const std::string& reason) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Exec("BEGIN IMMEDIATE;");
    try {
        EnsureNodeAndWindow(key, entry);
        Stmt u(db_, "UPDATE session_windows SET previous_session_id=?, reason=? WHERE session_id=?");
        u.Text(1, prev).Text(2, reason).Text(3, entry.value("sessionId", std::string())).Step();
        if (!prev.empty()) { Stmt e(db_, "UPDATE session_windows SET ended_at=? WHERE session_id=? AND ended_at IS NULL"); e.Int(1, NowMs()).Text(2, prev).Step(); }
        Exec("COMMIT;");
    } catch (...) { Exec("ROLLBACK;"); throw; }
}
bool SqliteBackend::Remove(const std::string& key) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Stmt s(db_, "DELETE FROM session_nodes WHERE session_key=?"); s.Text(1, key).Step();
    return sqlite3_changes(db_) > 0;
}
bool SqliteBackend::Empty() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Stmt s(db_, "SELECT 1 FROM session_nodes LIMIT 1"); return !s.Step();
}
json SqliteBackend::Stats() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    json out{{"path", path_.string()}, {"schemaVersion", kAgentSchemaVersion}};
    for (auto t : {"session_nodes", "session_windows", "transcript_events"}) {
        Stmt s(db_, (std::string("SELECT count(*) FROM ") + t).c_str()); s.Step(); out[t] = s.ColInt(0);
    }
    return out;
}
}  // namespace openclaw::session
