#include "openclaw/persist/state_db.hpp"

#include "openclaw/session/sqlite_backend.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace openclaw::persist {
namespace fs = std::filesystem;
namespace {
std::mutex g_mu;
std::map<std::string, sqlite3*> g_dbs;
sqlite3* OpenLocked(const fs::path& dbp) {
    auto it = g_dbs.find(dbp.string());
    if (it != g_dbs.end()) return it->second;
    std::error_code ec; fs::create_directories(dbp.parent_path(), ec);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbp.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;"
                     "CREATE TABLE IF NOT EXISTS state_documents(store_key TEXT PRIMARY KEY, value_json TEXT NOT NULL,"
                     " updated_at INTEGER NOT NULL, migrated_from TEXT);", nullptr, nullptr, nullptr);
    fs::permissions(dbp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    g_dbs[dbp.string()] = db;
    return db;
}
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
bool PutLocked(sqlite3* db, const std::string& key, const std::string& text, const char* from) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO state_documents(store_key,value_json,updated_at,migrated_from) VALUES(?,?,?,?)"
                               " ON CONFLICT(store_key) DO UPDATE SET value_json=excluded.value_json, updated_at=excluded.updated_at",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, NowMs());
    if (from) sqlite3_bind_text(st, 4, from, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 4);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}
std::optional<std::string> ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary); if (!in) return std::nullopt;
    std::stringstream ss; ss << in.rdbuf(); return ss.str();
}
}  // namespace

bool StateDbEnabled() {
    if (const char* e = std::getenv("OPENCLAW_STATE_STORE"); e && *e) return std::string(e) == "sqlite";
    return session::SessionBackend() == "sqlite";
}

fs::path StateDbPath(const fs::path& file) { return file.parent_path() / "openclaw-state.sqlite"; }

std::optional<std::string> StateRead(const fs::path& file) {
    if (!StateDbEnabled()) return ReadFile(file);
    std::lock_guard<std::mutex> l(g_mu);
    sqlite3* db = OpenLocked(StateDbPath(file));
    if (!db) return ReadFile(file);
    const std::string key = file.filename().string();
    sqlite3_stmt* st = nullptr; std::optional<std::string> out;
    if (sqlite3_prepare_v2(db, "SELECT value_json FROM state_documents WHERE store_key=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    if (out) return out;
    auto legacy = ReadFile(file);  // one-time import; the file is left in place as a backup
    if (legacy) PutLocked(db, key, *legacy, file.string().c_str());
    return legacy;
}

bool StateWrite(const fs::path& file, const std::string& text) {
    if (StateDbEnabled()) {
        std::lock_guard<std::mutex> l(g_mu);
        if (sqlite3* db = OpenLocked(StateDbPath(file))) return PutLocked(db, file.filename().string(), text, nullptr);
    }
    std::error_code ec; fs::create_directories(file.parent_path(), ec);
    const auto tmp = file.string() + ".tmp";
    { std::ofstream f(tmp, std::ios::trunc | std::ios::binary); if (!f) return false; f << text; }
    fs::rename(tmp, file, ec);
    return !ec;
}
}  // namespace openclaw::persist
