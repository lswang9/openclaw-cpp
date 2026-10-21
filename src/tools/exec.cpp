#include "openclaw/tools/exec.hpp"
#include <iomanip>
#include <atomic>
#include <thread>
#include <memory>
#include <openssl/sha.h>
#include <sqlite3.h>

#include "openclaw/tools/runenv.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>

namespace openclaw::tools::exec {
namespace fs = std::filesystem;
namespace {

int SecRank(const std::string& s) { return s == "deny" ? 0 : s == "allowlist" ? 1 : 2; }
int AskRank(const std::string& s) { return s == "always" ? 2 : s == "on-miss" ? 1 : 0; }
const char* SecName(int r) { return r == 0 ? "deny" : r == 1 ? "allowlist" : "full"; }
const char* AskName(int r) { return r == 2 ? "always" : r == 1 ? "on-miss" : "off"; }

std::string Str(const nlohmann::json& j, const char* k, const std::string& d = "") {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : d;
}
std::string Home() {
    const char* h = std::getenv("HOME");
    return h ? h : "/tmp";
}
std::string ExpandTilde(const std::string& p) {
    if (!p.empty() && p[0] == '~') return Home() + p.substr(1);
    return p;
}
std::string Base(const std::string& p) {
    auto s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}
bool InPath(const std::string& name, std::string* out = nullptr) {
    const char* path = std::getenv("PATH");
    std::stringstream ss(path ? path : "/usr/bin:/bin");
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        fs::path c = fs::path(dir) / name;
        if (::access(c.c_str(), X_OK) == 0 && !fs::is_directory(c)) {
            if (out) {
                std::error_code ec;
                auto canon = fs::canonical(c, ec);
                *out = ec ? c.string() : canon.string();
            }
            return true;
        }
    }
    return false;
}
std::string NewId() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream o;
    o << std::hex << rng();
    return o.str().substr(0, 12);
}
long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string ShellQuote(const std::string& s) {
    std::string o = "'";
    for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return o + "'";
}
std::string RegexEscape(const std::string& s) {
    static const std::string special = R"(\^$.|?*+()[]{}/)";
    std::string o;
    for (char c : s) { if (special.find(c) != std::string::npos) o += '\\'; o += c; }
    return o;
}
thread_local RunContext t_ctx;
}  // namespace

// ---------- approvals document ----------
fs::path DefaultApprovalsPath() {
    if (const char* p = std::getenv("OPENCLAW_EXEC_APPROVALS_PATH"); p && *p) return p;
    if (const char* d = std::getenv("OPENCLAW_STATE_DIR"); d && *d) return fs::path(d) / "exec-approvals.json";
    if (const char* d = std::getenv("OPENCLAW_DATA_DIR"); d && *d) return fs::path(d) / "exec-approvals.json";
    return fs::path(Home()) / ".openclaw" / "exec-approvals.json";
}

fs::path DefaultSqlitePath() {
    if (const char* p = std::getenv("OPENCLAW_STATE_DB"); p && *p) return p;
    fs::path root;
    if (const char* d = std::getenv("OPENCLAW_STATE_DIR"); d && *d) root = d;
    else if (const char* d = std::getenv("OPENCLAW_DATA_DIR"); d && *d) root = d;
    else root = fs::path(Home()) / ".openclaw";
    return root / "state" / "openclaw.sqlite";
}

namespace {
constexpr const char* kApprovalsKey = "exec_approvals_config";
std::mutex g_sqlite_mu;

bool SqliteGet(const fs::path& db, const std::string& key, std::string* out) {
    std::error_code ec;
    fs::create_directories(db.parent_path(), ec);
    sqlite3* conn = nullptr;
    if (sqlite3_open(db.string().c_str(), &conn) != SQLITE_OK) return false;
    sqlite3_exec(conn, "CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at INTEGER NOT NULL);",
                 nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(conn, "SELECT value FROM kv WHERE key=?1;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(st, 0);
            if (t) { *out = reinterpret_cast<const char*>(t); ok = true; }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(conn);
    return ok;
}

bool SqliteSet(const fs::path& db, const std::string& key, const std::string& value) {
    std::error_code ec;
    fs::create_directories(db.parent_path(), ec);
    sqlite3* conn = nullptr;
    if (sqlite3_open(db.string().c_str(), &conn) != SQLITE_OK) return false;
    sqlite3_exec(conn, "CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at INTEGER NOT NULL);",
                 nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(conn,
            "INSERT INTO kv(key,value,updated_at) VALUES(?1,?2,?3) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at;",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, NowMs());
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    sqlite3_close(conn);
    return ok;
}

nlohmann::json LoadJsonFile(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return {{"version", 1}};
    try {
        auto j = nlohmann::json::parse(in);
        if (j.is_object()) return j;
    } catch (...) {}
    return {{"version", 1}};
}

bool SaveJsonFile(const fs::path& p, const nlohmann::json& doc) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    auto tmp = p.string() + ".tmp";
    {
        std::ofstream o(tmp);
        if (!o) return false;
        o << doc.dump(2);
    }
    fs::rename(tmp, p, ec);
    if (!ec) fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, ec);
    return !ec;
}
}  // namespace

nlohmann::json LoadApprovals(const fs::path& p_in) {
    // Explicit JSON path (tests / OPENCLAW_EXEC_APPROVALS_PATH) bypasses SQLite.
    if (!p_in.empty() || (std::getenv("OPENCLAW_EXEC_APPROVALS_PATH") && *std::getenv("OPENCLAW_EXEC_APPROVALS_PATH"))) {
        const fs::path p = p_in.empty() ? DefaultApprovalsPath() : p_in;
        return LoadJsonFile(p);
    }
    std::lock_guard lock(g_sqlite_mu);
    const auto db = DefaultSqlitePath();
    std::string raw;
    if (SqliteGet(db, kApprovalsKey, &raw)) {
        try {
            auto j = nlohmann::json::parse(raw);
            if (j.is_object()) return j;
        } catch (...) {}
    }
    // Migrate legacy JSON once into SQLite.
    const auto legacy = DefaultApprovalsPath();
    auto doc = LoadJsonFile(legacy);
    if (fs::exists(legacy)) {
        SqliteSet(db, kApprovalsKey, doc.dump());
        std::error_code ec;
        fs::rename(legacy, legacy.string() + ".migrated", ec);
    }
    return doc;
}

bool SaveApprovals(const fs::path& p_in, const nlohmann::json& doc) {
    if (!p_in.empty() || (std::getenv("OPENCLAW_EXEC_APPROVALS_PATH") && *std::getenv("OPENCLAW_EXEC_APPROVALS_PATH"))) {
        const fs::path p = p_in.empty() ? DefaultApprovalsPath() : p_in;
        return SaveJsonFile(p, doc);
    }
    std::lock_guard lock(g_sqlite_mu);
    return SqliteSet(DefaultSqlitePath(), kApprovalsKey, doc.dump());
}

std::string FileSha256(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    SHA256_CTX ctx; SHA256_Init(&ctx);
    char buf[8192];
    while (in) {
        in.read(buf, sizeof buf);
        auto n = in.gcount();
        if (n > 0) SHA256_Update(&ctx, buf, static_cast<size_t>(n));
    }
    unsigned char dig[SHA256_DIGEST_LENGTH];
    SHA256_Final(dig, &ctx);
    std::ostringstream os;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        os << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(dig[i]);
    return os.str();
}

nlohmann::json SnapshotOperands(const ParsedCommand& pc, const fs::path& cwd) {
    nlohmann::json arr = nlohmann::json::array();
    std::error_code ec;
    auto add = [&](const fs::path& path, const std::string& role) {
        if (path.empty()) return;
        auto canon = fs::weakly_canonical(path, ec);
        if (ec || !fs::is_regular_file(canon, ec)) return;
        auto h = FileSha256(canon);
        if (h.empty()) return;
        arr.push_back({{"path", canon.string()}, {"sha256", h}, {"role", role}});
    };
    for (auto& s : pc.segments) {
        if (!s.resolved.empty()) add(s.resolved, "executable");
        for (std::size_t i = 1; i < s.argv.size(); ++i) {
            const auto& a = s.argv[i];
            if (a.empty() || a[0] == '-') continue;
            fs::path cand = (a[0] == '/') ? fs::path(a) : (cwd.empty() ? fs::path(a) : cwd / a);
            if (fs::is_regular_file(cand, ec)) add(cand, "operand");
        }
    }
    return arr;
}

std::string RevalidateOperands(const nlohmann::json& snap) {
    if (!snap.is_array()) return "";
    for (auto& e : snap) {
        if (!e.is_object()) continue;
        const std::string path = e.value("path", "");
        const std::string want = e.value("sha256", "");
        if (path.empty() || want.empty()) continue;
        auto got = FileSha256(path);
        if (got.empty()) return "operand missing after approval: " + path;
        if (got != want) return "operand changed after approval: " + path;
    }
    return "";
}

// ---------- background sessions (yieldMs / background) ----------
struct BgSession {
    std::string id, agent_id, command, cwd;
    int pid = -1;
    std::atomic<bool> done{false};
    std::atomic<bool> killed{false};
    ProcessResult result;
    std::string live_out;
    std::mutex mu;
    BackgroundHandle handle;
    int write_fd = -1;
    bool pty = false;
    int pty_rows = 24, pty_cols = 80;
    std::thread waiter;
};
std::mutex g_bg_mu;
std::map<std::string, std::shared_ptr<BgSession>> g_bg;

std::string AdoptBgSession(const std::string& agent_id, const std::string& cmd, const std::string& cwd,
                           BackgroundHandle handle, int timeout_sec, std::string already_out = {}) {
    if (handle.pid < 0) return {};
    auto s = std::make_shared<BgSession>();
    s->id = "bg" + NewId().substr(0, 10);
    s->agent_id = agent_id;
    s->command = cmd;
    s->cwd = cwd;
    s->handle = handle;
    s->pid = handle.pid;
    s->write_fd = handle.write_fd;
    s->pty = handle.pty;
    s->live_out = std::move(already_out);
    const int to_ms = timeout_sec <= 0 ? 86400000 : timeout_sec * 1000;
    s->waiter = std::thread([s, to_ms] {
        // Continue collecting until exit or overall timeout.
        ProcessResult r;
        r.out = s->live_out;
        const std::size_t base_len = r.out.size();
        auto rest = WaitBackground(s->handle, to_ms, 200 * 1024, [s](const char* b, std::size_t n) {
            std::lock_guard lock(s->mu);
            if (s->live_out.size() < 200 * 1024) s->live_out.append(b, std::min<std::size_t>(n, 200 * 1024 - s->live_out.size()));
        });
        (void)base_len;
        {
            std::lock_guard lock(s->mu);
            r.out += rest.out;
            r.exit_code = rest.exit_code;
            r.timed_out = rest.timed_out;
            r.truncated = rest.truncated || (r.out.size() >= 200 * 1024);
            r.error = rest.error;
            s->result = std::move(r);
            s->live_out = s->result.out;
            if (s->write_fd >= 0) { close(s->write_fd); s->write_fd = -1; }
        }
        s->done.store(true);
    });
    s->waiter.detach();
    std::lock_guard lock(g_bg_mu);
    g_bg[s->id] = s;
    return s->id;
}
std::string StartBgSession(const std::string& agent_id, const std::string& cmd, const std::string& cwd,
                           const std::string& run_cmd, int timeout_sec) {
    return AdoptBgSession(agent_id, cmd, cwd, StartBackground(run_cmd, cwd), timeout_sec);
}

// ---------- policy ----------
Policy ResolvePolicy(const nlohmann::json& raw, const nlohmann::json& doc, const std::string& agent_id,
                     const std::string& session_key, const std::string& requested_host) {
    Policy p;
    nlohmann::json ex = nlohmann::json::object();
    if (raw.contains("tools") && raw["tools"].is_object() && raw["tools"].contains("exec") && raw["tools"]["exec"].is_object())
        ex = raw["tools"]["exec"];
    nlohmann::json sandbox = nlohmann::json::object();
    if (raw.contains("agents") && raw["agents"].is_object()) {
        const auto& ag = raw["agents"];
        if (ag.contains("defaults") && ag["defaults"].is_object() && ag["defaults"].contains("sandbox") && ag["defaults"]["sandbox"].is_object())
            sandbox = ag["defaults"]["sandbox"];
        auto apply = [&](const nlohmann::json& a) {
            if (a.contains("tools") && a["tools"].is_object() && a["tools"].contains("exec") && a["tools"]["exec"].is_object())
                for (auto& [k, v] : a["tools"]["exec"].items()) ex[k] = v;
            if (a.contains("sandbox") && a["sandbox"].is_object())
                for (auto& [k, v] : a["sandbox"].items()) sandbox[k] = v;
        };
        if (ag.contains("list") && ag["list"].is_array())
            for (auto& a : ag["list"]) if (a.is_object() && Str(a, "id") == agent_id) apply(a);
        if (ag.contains("entries") && ag["entries"].is_object() && ag["entries"].contains(agent_id)) apply(ag["entries"][agent_id]);
    }
    // config-requested security/ask (tools.exec.mode preferred)
    std::string sec = Str(ex, "security", "full"), ask = Str(ex, "ask", "off");
    if (auto mode = Str(ex, "mode"); !mode.empty()) {
        if (mode == "deny") { sec = "deny"; ask = "off"; }
        else if (mode == "allowlist") { sec = "allowlist"; ask = "off"; }
        else if (mode == "ask" || mode == "auto") { sec = "allowlist"; ask = "on-miss"; }
        else if (mode == "full") { sec = "full"; ask = "off"; }
        p.auto_review = mode == "auto";
    }
    if (ex.contains("autoReview") && ex["autoReview"].is_object()) {
        const auto& ar = ex["autoReview"];
        if (ar.contains("enabled") && ar["enabled"].is_boolean()) p.auto_review = ar["enabled"].get<bool>() && ask != "off";
        p.auto_review_on_error = Str(ar, "onError", "escalate") == "deny" ? "deny" : "escalate";
        p.auto_review_model = Str(ar, "model");
    }
    p.sources["config"] = {{"security", sec}, {"ask", ask}};
    p.strict_inline_eval = ex.value("strictInlineEval", false);
    p.timeout_sec = ex.value("timeoutSec", ex.value("timeout", 1800));
    p.approval_timeout_ms = ex.value("approvalTimeoutMs", 120000);
    p.safe_bins = {"jq", "cut", "uniq", "head", "tail", "tr", "wc"};
    if (ex.contains("safeBins") && ex["safeBins"].is_array()) {
        p.safe_bins.clear();
        for (auto& b : ex["safeBins"]) if (b.is_string()) p.safe_bins.push_back(b);
    }
    // approvals document: defaults then agents.<id>; can only tighten
    nlohmann::json host_doc = doc.value("defaults", nlohmann::json::object());
    if (doc.contains("agents") && doc["agents"].is_object() && doc["agents"].contains(agent_id) && doc["agents"][agent_id].is_object())
        for (auto& [k, v] : doc["agents"][agent_id].items()) host_doc[k] = v;
    int sr = SecRank(sec), ar = AskRank(ask);
    if (auto s = Str(host_doc, "security"); !s.empty()) sr = std::min(sr, SecRank(s));
    if (auto a = Str(host_doc, "ask"); !a.empty()) ar = std::max(ar, AskRank(a));
    p.security = SecName(sr);
    p.ask = AskName(ar);
    std::string fb = Str(host_doc, "askFallback", Str(ex, "askFallback", "deny"));
    p.ask_fallback = fb == "full" ? "full" : fb == "allowlist" ? "allowlist" : "deny";
    p.auto_allow_skills = host_doc.value("autoAllowSkills", false);
    if (host_doc.contains("allowlist") && host_doc["allowlist"].is_array())
        for (auto& e : host_doc["allowlist"]) {
            if (!e.is_object() || Str(e, "pattern").empty()) continue;
            p.allowlist.push_back({Str(e, "id"), Str(e, "pattern"), Str(e, "argPattern"), Str(e, "source"), Str(e, "cwd")});
        }
    p.sources["approvals"] = host_doc;
    // host selection
    const std::string mode = Str(sandbox, "mode", "off");
    const bool is_main = session_key.empty() || session_key == "main" || session_key == "agent:" + agent_id + ":main";
    const bool sandboxed = mode == "all" || (mode == "non-main" && !is_main);
    std::string host = Str(ex, "host", "auto");
    if (!requested_host.empty() && !sandboxed) host = requested_host;  // per-call host only when no sandbox runtime
    if (host == "auto") host = sandboxed ? "sandbox" : "gateway";
    if (sandboxed && host != "sandbox" && Str(ex, "host") != "gateway") host = "sandbox";
    p.host = host == "sandbox" ? "sandbox" : "gateway";
    if (sandbox.contains("docker") && sandbox["docker"].is_object()) {
        p.sandbox_image = Str(sandbox["docker"], "image", p.sandbox_image);
        p.sandbox_network = Str(sandbox["docker"], "network", "none") != "none";
    }
    std::string backend = Str(sandbox, "backend", "auto");
    if (backend == "auto") backend = InPath("docker") ? "docker" : (InPath("bwrap") ? "bwrap" : "");
    else if (!InPath(backend)) backend = "";
    p.sandbox_backend = backend;
    return p;
}

// ---------- parsing ----------
ParsedCommand Parse(const std::string& command, const std::string& cwd) {
    ParsedCommand pc;
    if (command.find("$(") != std::string::npos || command.find('`') != std::string::npos ||
        command.find("<<") != std::string::npos || command.find("<(") != std::string::npos || command.find(">(") != std::string::npos)
        pc.complex = true;
    std::vector<std::vector<std::string>> segs(1);
    std::string cur;
    bool have = false, skip_next = false;
    auto push = [&] {
        if (have) {
            if (skip_next) skip_next = false;
            else segs.back().push_back(cur);
        }
        cur.clear();
        have = false;
    };
    for (std::size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (c == '\'') {
            auto e = command.find('\'', i + 1);
            if (e == std::string::npos) { pc.complex = true; e = command.size(); }
            cur += command.substr(i + 1, e - i - 1); have = true; i = e;
        } else if (c == '"') {
            std::size_t j = i + 1;
            for (; j < command.size() && command[j] != '"'; ++j) {
                if (command[j] == '\\' && j + 1 < command.size()) ++j;
                cur += command[j];
            }
            if (j >= command.size()) pc.complex = true;
            have = true; i = j;
        } else if (c == '\\' && i + 1 < command.size()) {
            cur += command[++i]; have = true;
        } else if (c == ' ' || c == '\t') {
            push();
        } else if (c == ';' || c == '|' || c == '&' || c == '\n') {
            push();
            if (!segs.back().empty()) segs.emplace_back();
            if (i + 1 < command.size() && (command[i + 1] == '|' || command[i + 1] == '&')) ++i;
        } else if (c == '>' || c == '<') {
            push();
            if (i + 1 < command.size() && command[i + 1] == '>') ++i;
            if (i + 1 < command.size() && command[i + 1] == '&') { ++i; while (i + 1 < command.size() && std::isdigit(static_cast<unsigned char>(command[i + 1]))) ++i; }
            else skip_next = true;  // redirection target is not an argument
        } else {
            cur += c; have = true;
        }
    }
    push();
    for (auto& s : segs) {
        // drop leading VAR=value assignments and a bare fd number before redirection
        std::size_t k = 0;
        while (k < s.size() && s[k].find('=') != std::string::npos && s[k][0] != '-' &&
               std::isalpha(static_cast<unsigned char>(s[k][0])) && s[k].find('/') > s[k].find('='))
            ++k;
        std::vector<std::string> argv(s.begin() + static_cast<long>(k), s.end());
        if (!argv.empty() && std::all_of(argv.back().begin(), argv.back().end(), ::isdigit)) argv.pop_back();
        if (argv.empty()) continue;
        Segment seg;
        seg.argv = argv;
        const auto& a0 = argv[0];
        if (a0.find('/') != std::string::npos) {
            fs::path p = a0[0] == '/' ? fs::path(a0) : fs::path(cwd) / a0;
            std::error_code ec;
            auto canon = fs::canonical(p, ec);
            seg.resolved = ec ? "" : canon.string();
        } else {
            seg.via_path = InPath(a0, &seg.resolved);
        }
        pc.segments.push_back(std::move(seg));
    }
    return pc;
}

bool GlobMatch(const std::string& pattern_in, const std::string& text) {
    const std::string pattern = ExpandTilde(pattern_in);
    std::string re;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                ++i;
                if (i + 1 < pattern.size() && pattern[i + 1] == '/') { ++i; re += "(?:.*/)?"; }
                else re += ".*";
            } else re += "[^/]*";
        } else if (c == '?') re += "[^/]";
        else re += RegexEscape(std::string(1, c));
    }
    try { return std::regex_match(text, std::regex(re)); } catch (...) { return false; }
}

bool IsInlineEval(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;
    std::string b = Base(argv[0]);
    while (!b.empty() && (std::isdigit(static_cast<unsigned char>(b.back())) || b.back() == '.')) b.pop_back();
    static const std::map<std::string, std::vector<std::string>> flags = {
        {"python", {"-c"}}, {"node", {"-e", "--eval", "-p", "--print"}}, {"ruby", {"-e"}}, {"perl", {"-e", "-E"}},
        {"php", {"-r"}}, {"lua", {"-e"}}, {"osascript", {"-e"}}, {"bash", {"-c"}}, {"sh", {"-c"}}, {"zsh", {"-c"}},
        {"deno", {"eval"}}, {"bun", {"-e", "--eval"}}};
    auto it = flags.find(b);
    if (it != flags.end())
        for (std::size_t i = 1; i < argv.size(); ++i)
            for (auto& f : it->second) if (argv[i] == f || argv[i].rfind(f + "=", 0) == 0) return true;
    if (b == "awk" || b == "gawk") return argv.size() > 1;
    if (b == "find") for (auto& a : argv) if (a == "-exec" || a == "-execdir") return true;
    if (b == "xargs") return argv.size() > 1;
    return false;
}

namespace {
bool SegmentAllowed(const Policy& p, const Segment& s, const std::string& cwd) {
    // safe bins: stdin-only filters with no file operands
    if (s.via_path && std::find(p.safe_bins.begin(), p.safe_bins.end(), Base(s.argv[0])) != p.safe_bins.end()) {
        bool file_arg = false;
        for (std::size_t i = 1; i < s.argv.size(); ++i) {
            const auto& a = s.argv[i];
            if (a.empty() || a[0] == '-') continue;
            std::error_code ec;
            if (a.find('/') != std::string::npos || fs::exists(fs::path(cwd) / a, ec)) { file_arg = true; break; }
        }
        if (!file_arg) return true;
    }
    std::string args;
    for (std::size_t i = 1; i < s.argv.size(); ++i) args += (i > 1 ? " " : "") + s.argv[i];
    for (auto& e : p.allowlist) {
        if (!e.cwd.empty() && e.cwd != cwd) continue;
        const bool path_pat = e.pattern.find('/') != std::string::npos || e.pattern[0] == '~';
        bool m = path_pat ? (!s.resolved.empty() && GlobMatch(e.pattern, s.resolved))
                          : (s.via_path && GlobMatch(e.pattern, Base(s.argv[0])));
        if (!m) continue;
        if (e.arg_pattern.empty()) return true;
        try { if (std::regex_search(args, std::regex(e.arg_pattern, std::regex::ECMAScript))) return true; } catch (...) {}
    }
    return false;
}
}  // namespace

bool AllowlistHit(const Policy& p, const ParsedCommand& pc, const std::string& cwd) {
    if (pc.complex || pc.segments.empty()) return false;
    for (auto& s : pc.segments) if (!SegmentAllowed(p, s, cwd)) return false;
    return true;
}

Decision Evaluate(const Policy& p, const ParsedCommand& pc, const std::string& cwd) {
    Decision d;
    if (p.security == "deny") return {"deny", "exec denied by policy (security=deny)", false, false};
    d.allowlist_hit = AllowlistHit(p, pc, cwd);
    for (auto& s : pc.segments) d.inline_eval = d.inline_eval || IsInlineEval(s.argv);
    const bool strict_inline = p.strict_inline_eval && d.inline_eval;
    if (p.security == "full" && p.ask == "off") {
        d.verdict = strict_inline ? "ask" : "allow";
        d.reason = strict_inline ? "strict inline-eval requires approval" : "security=full";
        return d;
    }
    if (p.ask == "always") { d.verdict = "ask"; d.reason = "ask=always"; return d; }
    if (d.allowlist_hit && !strict_inline) { d.verdict = "allow"; d.reason = "allowlist match"; return d; }
    if (p.ask == "on-miss") { d.verdict = "ask"; d.reason = strict_inline ? "strict inline-eval requires approval" : "allowlist miss"; return d; }
    if (p.security == "full") { d.verdict = strict_inline ? "deny" : "allow"; d.reason = "security=full"; return d; }
    d.verdict = "deny";
    d.reason = strict_inline ? "strict inline-eval requires approval (ask=off)" : "allowlist miss (security=allowlist, ask=off)";
    return d;
}

// ---------- broker ----------
ApprovalBroker& ApprovalBroker::Global() {
    static ApprovalBroker b;
    return b;
}

std::string ApprovalBroker::Create(nlohmann::json request) {
    std::string id;
    {
        std::lock_guard l(mu_);
        id = "ex" + std::to_string(++seq_) + "-" + NewId().substr(0, 6);
        request["id"] = id;
        request["status"] = "pending";
        request["createdAtMs"] = NowMs();
        items_[id] = request;
    }
    if (on_requested) on_requested(request);
    return id;
}

std::string ApprovalBroker::Wait(const std::string& id, std::chrono::milliseconds timeout) {
    std::unique_lock l(mu_);
    auto done = [&] { auto it = items_.find(id); return it == items_.end() || it->second.value("status", "") != "pending"; };
    if (!cv_.wait_for(l, timeout, done)) {
        items_[id]["status"] = "expired";
        return "timeout";
    }
    auto it = items_.find(id);
    return it == items_.end() ? "deny" : it->second.value("decision", std::string("deny"));
}

bool ApprovalBroker::Resolve(const std::string& id, const std::string& decision) {
    nlohmann::json snap;
    {
        std::lock_guard l(mu_);
        auto it = items_.find(id);
        if (it == items_.end() || it->second.value("status", "") != "pending") return false;
        const std::string d = (decision == "allow-once" || decision == "allow-always" || decision == "deny") ? decision
                              : (decision == "allow" || decision == "approve" || decision == "once") ? "allow-once"
                              : (decision == "always") ? "allow-always" : "deny";
        it->second["status"] = "resolved";
        it->second["decision"] = d;
        it->second["resolvedAtMs"] = NowMs();
        snap = it->second;
    }
    cv_.notify_all();
    if (on_resolved) on_resolved(snap);
    return true;
}

nlohmann::json ApprovalBroker::Pending() const {
    std::lock_guard l(mu_);
    nlohmann::json out = nlohmann::json::array();
    for (auto& [id, r] : items_) if (r.value("status", "") == "pending") out.push_back(r);
    return out;
}

nlohmann::json ApprovalBroker::Get(const std::string& id) const {
    std::lock_guard l(mu_);
    auto it = items_.find(id);
    return it == items_.end() ? nlohmann::json() : it->second;
}

ScopedRunContext::ScopedRunContext(RunContext c) : prev_(t_ctx) { t_ctx = std::move(c); }
ScopedRunContext::~ScopedRunContext() { t_ctx = prev_; }
const RunContext& CurrentContext() { return t_ctx; }

// ---------- run ----------
nlohmann::json Run(const nlohmann::json& args, const nlohmann::json& cfg_raw, const fs::path& workspace) {
    const std::string cmd = args.value("command", "");
    if (cmd.empty()) return {{"status", "failed"}, {"error", "missing command"}};
    const auto& ctx = CurrentContext();
    auto doc = LoadApprovals({});
    auto pol = ResolvePolicy(cfg_raw, doc, ctx.agent_id, ctx.session_key, args.value("host", ""));
    // per-call ask/security can only harden
    if (auto a = args.value("ask", std::string()); !a.empty() && AskRank(a) > AskRank(pol.ask)) pol.ask = AskName(AskRank(a));
    if (auto s = args.value("security", std::string()); !s.empty() && SecRank(s) < SecRank(pol.security)) pol.security = SecName(SecRank(s));

    std::error_code ec;
    fs::create_directories(workspace, ec);
    fs::path cwd = workspace;
    if (auto wd = args.value("workdir", args.value("cwd", std::string())); !wd.empty())
        cwd = wd[0] == '/' ? fs::path(wd) : workspace / wd;
    if (!fs::is_directory(cwd, ec)) return {{"status", "failed"}, {"error", "workdir does not exist: " + cwd.string()}};

    // env overrides: reject loader/PATH hijacks on host exec
    EnvVars env;
    if (args.contains("env") && args["env"].is_object())
        for (auto& [k, v] : args["env"].items()) {
            if (pol.host == "gateway" && (k == "PATH" || k.rfind("LD_", 0) == 0 || k.rfind("DYLD_", 0) == 0 || k == "NODE_OPTIONS"))
                return {{"status", "failed"}, {"error", "env override not allowed for host exec: " + k}};
            env.emplace_back(k, v.is_string() ? v.get<std::string>() : v.dump());
        }
    int timeout_sec = args.value("timeout", pol.timeout_sec);
    timeout_sec = std::clamp(timeout_sec, 1, 7200);
    nlohmann::json base{{"host", pol.host}, {"cwd", cwd.string()}, {"command", cmd},
                        {"policy", {{"security", pol.security}, {"ask", pol.ask}, {"askFallback", pol.ask_fallback}}}};

    if (LookupEnv("OPENCLAW_EXEC_MODE") == "mock") {
        base.update({{"status", "completed"}, {"exitCode", 0}, {"mocked", true}, {"output", "mock: executed (no real shell)"}});
        return base;
    }

    std::string run_cmd = cmd;
    if (pol.host == "sandbox") {
        if (pol.sandbox_backend.empty())
            return base.update({{"status", "failed"}, {"error", "sandbox runtime unavailable (need docker or bwrap)"}}), base;
        if (pol.sandbox_backend == "docker") {
            run_cmd = "docker run --rm -i " + std::string(pol.sandbox_network ? "" : "--network none ") + "-v " +
                      ShellQuote(workspace.string()) + ":/workspace -w /workspace " + ShellQuote(pol.sandbox_image) + " sh -lc " +
                      ShellQuote(cmd);
        } else {  // bwrap: read-only host, writable workspace, no network, private /tmp
            run_cmd = "bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp --bind " + ShellQuote(workspace.string()) + " " +
                      ShellQuote(workspace.string()) + " --chdir " + ShellQuote(cwd.string()) +
                      (pol.sandbox_network ? "" : " --unshare-net") + " --die-with-parent sh -c " + ShellQuote(cmd);
        }
        base["sandbox"] = pol.sandbox_backend;
    } else {
        auto pc = Parse(cmd, cwd.string());
        auto d = Evaluate(pol, pc, cwd.string());
        base["decision"] = {{"verdict", d.verdict}, {"reason", d.reason}, {"allowlistHit", d.allowlist_hit}};
        if (d.verdict == "deny") return base.update({{"status", "denied"}, {"error", "exec denied: " + d.reason}}), base;
        if (d.verdict == "ask") {
            auto& broker = ApprovalBroker::Global();
            std::string decision;
            auto operands = SnapshotOperands(pc, cwd);
            bool reviewed = false;
            if (pol.auto_review && pol.ask != "always") {
                nlohmann::json req{{"kind", "exec"}, {"command", cmd}, {"cwd", cwd.string()}, {"agentId", ctx.agent_id},
                                   {"sessionKey", ctx.session_key}, {"reason", d.reason}, {"inlineEval", d.inline_eval},
                                   {"model", pol.auto_review_model}, {"onError", pol.auto_review_on_error}};
                nlohmann::json rv = broker.auto_review ? broker.auto_review(req)
                                                       : nlohmann::json{{"decision", pol.auto_review_on_error == "deny" ? "deny" : "escalate"},
                                                                        {"reason", "no reviewer provider wired"}};
                base["autoReview"] = rv;
                const std::string rd = rv.value("decision", "escalate");
                if (rd == "deny") return base.update({{"status", "denied"}, {"error", "exec denied by auto reviewer: " + rv.value("reason", "")}}), base;
                if (rd == "allow") { decision = "allow-once"; reviewed = true; }
            }
            if (reviewed) {
            } else if (broker.has_ui && broker.has_ui()) {
                auto id = broker.Create({{"kind", "exec"}, {"command", cmd}, {"cwd", cwd.string()}, {"agentId", ctx.agent_id},
                                         {"sessionKey", ctx.session_key}, {"host", "gateway"},
                                         {"channel", ctx.channel}, {"requester", ctx.sender}, {"reason", d.reason},
                                         {"allowAlways", !d.inline_eval}, {"operands", operands}});
                base["approvalId"] = id;
                decision = broker.Wait(id, std::chrono::milliseconds(pol.approval_timeout_ms));
            } else {
                decision = "timeout";
                base["approvalUi"] = false;
            }
            if (decision == "timeout") {
                const bool ok = pol.ask_fallback == "full" ? !d.inline_eval
                              : pol.ask_fallback == "allowlist" ? (d.allowlist_hit && !d.inline_eval) : false;
                base["askFallback"] = pol.ask_fallback;
                decision = ok ? "allow-once" : "deny";
                if (!ok) return base.update({{"status", "denied"}, {"error", "exec approval unavailable; askFallback=" + pol.ask_fallback + " denied the command"}}), base;
            }
            base["approvalDecision"] = decision;
            if (decision == "deny") return base.update({{"status", "denied"}, {"error", "exec denied by operator"}}), base;
            if (auto err = RevalidateOperands(operands); !err.empty())
                return base.update({{"status", "denied"}, {"error", err}}), base;
            if (decision == "allow-always" && !d.inline_eval && !pc.complex) {
                // Mutable script operands: durable grant becomes one-shot (upstream byte-bound rule).
                bool mutable_operand = false;
                for (auto& e : operands) if (e.value("role", "") == "operand") mutable_operand = true;
                if (mutable_operand) {
                    base["allowAlwaysDowngraded"] = true;
                } else {
                    auto& list = doc["agents"][ctx.agent_id]["allowlist"];
                    if (!list.is_array()) list = nlohmann::json::array();
                    for (auto& seg : pc.segments) {
                        std::string a;
                        for (std::size_t i = 1; i < seg.argv.size(); ++i) a += (i > 1 ? " " : "") + seg.argv[i];
                        list.push_back({{"id", NewId()}, {"pattern", seg.resolved.empty() ? seg.argv[0] : seg.resolved},
                                        {"argPattern", "^" + RegexEscape(a) + "$"}, {"source", "allow-always"},
                                        {"cwd", cwd.string()}, {"lastUsedAt", NowMs()}, {"lastResolvedPath", seg.resolved}});
                    }
                    SaveApprovals({}, doc);
                    base["persisted"] = true;
                }
            }
        }
    }
    const bool want_pty = args.value("pty", false);
    const bool force_bg = args.value("background", false);
    const int yield_ms = args.value("yieldMs", want_pty && !force_bg ? 10000 : 0);
    const bool want_bg = force_bg || yield_ms > 0;
    ScopedRunEnv scoped(env);
    if (want_bg) {
        auto h = StartBackground(run_cmd, cwd.string(), want_pty && pol.host != "sandbox");
        if (h.pid < 0) return base.update({{"status", "failed"}, {"error", "failed to start process"}}), base;
        std::string early;
        if (!force_bg && yield_ms > 0) {
            auto pr = CollectBackground(h, yield_ms, 200 * 1024);
            if (h.pid < 0) {  // finished within yield window
                base["exitCode"] = pr.exit_code;
                base["output"] = pr.out;
                base["truncated"] = pr.truncated;
                base["status"] = pr.exit_code == 0 ? "completed" : "failed";
                if (!pr.error.empty()) base["error"] = pr.error;
                return base;
            }
            early = pr.out;
        }
        auto sid = AdoptBgSession(ctx.agent_id, cmd, cwd.string(), h, timeout_sec, early);
        if (sid.empty()) return base.update({{"status", "failed"}, {"error", "failed to track background process"}}), base;
        int pid = -1;
        { std::lock_guard lock(g_bg_mu); pid = g_bg[sid]->pid; }
        base.update({{"status", "running"}, {"sessionId", sid}, {"pid", pid},
                     {"background", true}, {"output", early},
                     {"note", "use process tool (poll/log/kill) with sessionId"}});
        return base;
    }
    auto pr = RunProcess(run_cmd, cwd.string(), timeout_sec * 1000, 200 * 1024);
    base["exitCode"] = pr.exit_code;
    base["output"] = pr.out;
    base["truncated"] = pr.truncated;
    base["status"] = pr.timed_out ? "timeout" : (pr.exit_code == 0 ? "completed" : "failed");
    if (!pr.error.empty()) base["error"] = pr.error;
    return base;
}

nlohmann::json ProcessTool(const nlohmann::json& args) {
    const std::string action = args.value("action", "list");
    const auto& ctx = CurrentContext();
    if (action == "list") {
        nlohmann::json arr = nlohmann::json::array();
        std::lock_guard lock(g_bg_mu);
        for (auto& [id, s] : g_bg) {
            if (s->agent_id != ctx.agent_id && !ctx.agent_id.empty()) continue;
            std::lock_guard lk(s->mu);
            arr.push_back({{"sessionId", id}, {"command", s->command}, {"cwd", s->cwd}, {"pid", s->pid},
                           {"done", s->done.load()}, {"pty", s->pty},
                           {"rows", s->pty_rows}, {"cols", s->pty_cols}, {"status", s->done.load() ? (s->result.timed_out ? "timeout" : "exited") : "running"}});
        }
        return {{"sessions", arr}};
    }
    const std::string sid = args.value("sessionId", args.value("id", ""));
    if (sid.empty()) return {{"ok", false}, {"error", "missing sessionId"}};
    std::shared_ptr<BgSession> s;
    {
        std::lock_guard lock(g_bg_mu);
        auto it = g_bg.find(sid);
        if (it == g_bg.end()) return {{"ok", false}, {"error", "unknown sessionId"}};
        if (!ctx.agent_id.empty() && it->second->agent_id != ctx.agent_id)
            return {{"ok", false}, {"error", "session belongs to another agent"}};
        s = it->second;
    }
    if (action == "kill") {
        KillBackground(s->handle);
        s->killed.store(true);
        return {{"ok", true}, {"sessionId", sid}, {"killed", true}};
    }
    if (action == "resize") {
        int rows = args.value("rows", args.value("height", 24));
        int cols = args.value("cols", args.value("width", 80));
        std::lock_guard lk(s->mu);
        if (!s->pty || s->write_fd < 0)
            return {{"ok", false}, {"error", "session is not a pty (start exec with pty:true)"}};
        // Resize via the live handle's write_fd (same as master).
        BackgroundHandle tmp;
        tmp.pty = true; tmp.write_fd = s->write_fd; tmp.pid = s->pid;
        bool ok = ResizePty(tmp, rows, cols);
        if (ok) { s->pty_rows = rows; s->pty_cols = cols; }
        return {{"ok", ok}, {"sessionId", sid}, {"rows", rows}, {"cols", cols}};
    }
    if (action == "write" || action == "send-keys" || action == "submit" || action == "paste") {
        std::string bytes;
        if (action == "send-keys") {
            std::vector<std::string> keys;
            if (args.contains("keys") && args["keys"].is_array())
                for (auto& k : args["keys"]) if (k.is_string()) keys.push_back(k);
            if (args.contains("literal") && args["literal"].is_string()) bytes += args["literal"].get<std::string>();
            bytes += KeysToBytes(keys);
        } else {
            bytes = args.value("data", args.value("text", std::string()));
            if (action == "paste") bytes = "\x1b[200~" + bytes + "\x1b[201~";
            if (action == "submit") bytes += "\r";
        }
        std::lock_guard lk(s->mu);
        if (s->done.load() || s->write_fd < 0)
            return {{"ok", false}, {"error", s->pty ? "session has exited" : "session has no stdin (start exec with pty:true)"}};
        std::size_t off = 0;
        while (off < bytes.size()) {
            ssize_t n = ::write(s->write_fd, bytes.data() + off, bytes.size() - off);
            if (n < 0) { if (errno == EAGAIN || errno == EINTR) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
                         return {{"ok", false}, {"error", std::string("write failed: ") + std::strerror(errno)}}; }
            off += static_cast<std::size_t>(n);
        }
        return {{"ok", true}, {"sessionId", sid}, {"bytes", bytes.size()}};
    }
    if (action == "poll" || action == "log" || action == "status") {
        int wait_ms = args.value("timeout", 0);
        if (wait_ms > 0 && !s->done.load()) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
            while (!s->done.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::lock_guard lk(s->mu);
        nlohmann::json out{{"ok", true}, {"sessionId", sid}, {"done", s->done.load()}, {"pid", s->pid},
                           {"output", s->done.load() ? s->result.out : s->live_out},
                           {"status", s->done.load() ? (s->result.timed_out ? "timeout" : (s->result.exit_code == 0 ? "completed" : "failed")) : "running"}};
        if (s->done.load()) {
            out["exitCode"] = s->result.exit_code;
            out["truncated"] = s->result.truncated;
            if (!s->result.error.empty()) out["error"] = s->result.error;
        }
        return out;
    }
    return {{"ok", false}, {"error", "unknown action (list|poll|log|write|send-keys|submit|paste|resize|kill)"}};
}

nlohmann::json ReviewWithProvider(provider::Provider& prov, const nlohmann::json& request, const std::string& on_error) {
    const std::string fallback = on_error == "deny" ? "deny" : "escalate";
    std::vector<agent::Message> msgs;
    agent::Message sys;
    sys.role = "system";
    sys.content =
        "You are the OpenClaw exec auto-reviewer. Decide whether a shell command requested by an AI agent may run "
        "without a human. Allow only clearly read-only or low-risk, workspace-scoped commands. Deny destructive, "
        "exfiltrating, privilege-escalating or credential-reading commands. If unsure, escalate to a human. "
        "Reply with ONLY a JSON object: {\"decision\":\"allow|deny|escalate\",\"risk\":\"low|medium|high\",\"reason\":\"...\"}";
    agent::Message user;
    user.role = "user";
    user.content = "Command: " + request.value("command", "") + "\ncwd: " + request.value("cwd", "") +
                   "\nagent: " + request.value("agentId", "") + "\npolicy reason: " + request.value("reason", "") +
                   (request.value("inlineEval", false) ? "\nnote: inline interpreter eval detected" : "");
    msgs.push_back(sys);
    msgs.push_back(user);
    agent::CompletionResult r;
    try { r = prov.Complete(msgs, nlohmann::json::array()); }
    catch (const std::exception& e) { return {{"decision", fallback}, {"reason", std::string("reviewer error: ") + e.what()}, {"reviewer", "provider"}}; }
    if (!r.ok) return {{"decision", fallback}, {"reason", "reviewer call failed: " + r.error}, {"reviewer", "provider"}};
    const std::string& t = r.message.content;
    auto a = t.find('{'), b = t.rfind('}');
    nlohmann::json v;
    if (a != std::string::npos && b != std::string::npos && b > a) v = nlohmann::json::parse(t.substr(a, b - a + 1), nullptr, false);
    if (!v.is_object() || !v.contains("decision") || !v["decision"].is_string())
        return {{"decision", fallback}, {"reason", "reviewer reply was not a JSON verdict"}, {"raw", t.substr(0, 200)}, {"reviewer", "provider"}};
    std::string dec = v["decision"];
    std::string risk = v.value("risk", "");
    if (dec != "allow" && dec != "deny") dec = "escalate";
    if (dec == "allow" && risk == "high") dec = "escalate";
    if (dec == "allow" && request.value("inlineEval", false) && risk != "low") dec = "escalate";
    return {{"decision", dec}, {"risk", risk}, {"reason", v.value("reason", "")}, {"reviewer", "provider"}};
}

nlohmann::json ReviewWithProviderRoute(provider::Router& router, const std::string& route, const nlohmann::json& request,
                                       const std::string& on_error) {
    const std::string fallback = on_error == "deny" ? "deny" : "escalate";
    std::vector<agent::Message> msgs;
    agent::Message sys;
    sys.role = "system";
    sys.content =
        "You are the OpenClaw exec auto-reviewer. Decide whether a shell command requested by an AI agent may run "
        "without a human. Allow only clearly read-only or low-risk, workspace-scoped commands. Deny destructive, "
        "exfiltrating, privilege-escalating or credential-reading commands. If unsure, escalate to a human. "
        "Reply with ONLY a JSON object: {\"decision\":\"allow|deny|escalate\",\"risk\":\"low|medium|high\",\"reason\":\"...\"}";
    agent::Message user;
    user.role = "user";
    user.content = "Command: " + request.value("command", "") + "\ncwd: " + request.value("cwd", "") +
                   "\nagent: " + request.value("agentId", "") + "\npolicy reason: " + request.value("reason", "") +
                   (request.value("inlineEval", false) ? "\nnote: inline interpreter eval detected" : "");
    msgs.push_back(sys);
    msgs.push_back(user);
    agent::CompletionResult r;
    try { r = router.CompleteRoute(route, msgs, nlohmann::json::array()); }
    catch (const std::exception& e) {
        return {{"decision", fallback}, {"reason", std::string("reviewer error: ") + e.what()},
                {"reviewer", "provider"}, {"model", route}};
    }
    if (!r.ok) return {{"decision", fallback}, {"reason", "reviewer call failed: " + r.error},
                       {"reviewer", "provider"}, {"model", route}};
    const std::string& t = r.message.content;
    auto a = t.find('{'), b = t.rfind('}');
    nlohmann::json v;
    if (a != std::string::npos && b != std::string::npos && b > a)
        v = nlohmann::json::parse(t.substr(a, b - a + 1), nullptr, false);
    if (!v.is_object() || !v.contains("decision") || !v["decision"].is_string())
        return {{"decision", fallback}, {"reason", "reviewer reply was not a JSON verdict"},
                {"raw", t.substr(0, 200)}, {"reviewer", "provider"}, {"model", route}};
    std::string dec = v["decision"];
    std::string risk = v.value("risk", "");
    if (dec != "allow" && dec != "deny") dec = "escalate";
    if (dec == "allow" && risk == "high") dec = "escalate";
    if (dec == "allow" && request.value("inlineEval", false) && risk != "low") dec = "escalate";
    return {{"decision", dec}, {"risk", risk}, {"reason", v.value("reason", "")},
            {"reviewer", "provider"}, {"model", route}};
}


}  // namespace openclaw::tools::exec
