#include "openclaw/tools/exec.hpp"

#include "openclaw/tools/runenv.hpp"

#include <unistd.h>

#include <algorithm>
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

nlohmann::json LoadApprovals(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return {{"version", 1}};
    try {
        auto j = nlohmann::json::parse(in);
        if (j.is_object()) return j;
    } catch (...) {}
    return {{"version", 1}};
}

bool SaveApprovals(const fs::path& p, const nlohmann::json& doc) {
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
    return true;
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
    const auto approvals_path = DefaultApprovalsPath();
    auto doc = LoadApprovals(approvals_path);
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
            if (broker.has_ui && broker.has_ui()) {
                auto id = broker.Create({{"kind", "exec"}, {"command", cmd}, {"cwd", cwd.string()}, {"agentId", ctx.agent_id},
                                         {"sessionKey", ctx.session_key}, {"host", "gateway"},
                                         {"channel", ctx.channel}, {"requester", ctx.sender}, {"reason", d.reason},
                                         {"allowAlways", !d.inline_eval}});
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
            if (decision == "allow-always" && !d.inline_eval && !pc.complex) {
                auto& list = doc["agents"][ctx.agent_id]["allowlist"];
                if (!list.is_array()) list = nlohmann::json::array();
                for (auto& s : pc.segments) {
                    std::string a;
                    for (std::size_t i = 1; i < s.argv.size(); ++i) a += (i > 1 ? " " : "") + s.argv[i];
                    list.push_back({{"id", NewId()}, {"pattern", s.resolved.empty() ? s.argv[0] : s.resolved},
                                    {"argPattern", "^" + RegexEscape(a) + "$"}, {"source", "allow-always"},
                                    {"cwd", cwd.string()}, {"lastUsedAt", NowMs()}, {"lastResolvedPath", s.resolved}});
                }
                SaveApprovals(approvals_path, doc);
                base["persisted"] = true;
            }
        }
    }
    ScopedRunEnv scoped(env);
    auto pr = RunProcess(run_cmd, cwd.string(), timeout_sec * 1000, 200 * 1024);
    base["exitCode"] = pr.exit_code;
    base["output"] = pr.out;
    base["truncated"] = pr.truncated;
    base["status"] = pr.timed_out ? "timeout" : (pr.exit_code == 0 ? "completed" : "failed");
    if (!pr.error.empty()) base["error"] = pr.error;
    return base;
}

}  // namespace openclaw::tools::exec
