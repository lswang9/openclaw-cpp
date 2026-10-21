#include "openclaw/gateway/subsystems.hpp"
#include "openclaw/gateway/node_hub.hpp"

#include <curl/curl.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>

#include "openclaw/agent/pool.hpp"
#include "openclaw/agent/routing.hpp"
#include "openclaw/config/config.hpp"
#include "openclaw/nodes/registry.hpp"
#include "openclaw/provider/provider.hpp"
#include "openclaw/session/store.hpp"

namespace openclaw::gateway {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string Hex(const unsigned char* d, std::size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) { s += k[d[i] >> 4]; s += k[d[i] & 15]; }
    return s;
}
std::string ShaHex(const std::string& s) {
    unsigned char h[32]; SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), h); return Hex(h, 32);
}
std::string RandomHex(std::size_t bytes) {
    std::random_device rd; std::string s;
    for (std::size_t i = 0; i < bytes; ++i) { unsigned char b = static_cast<unsigned char>(rd()); s += Hex(&b, 1); }
    return s;
}
json ReadJsonFile(const fs::path& p, json def) {
    std::ifstream f(p); if (!f) return def;
    try { return json::parse(f); } catch (...) { return def; }
}
void WriteJsonFile(const fs::path& p, const json& j) {
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    const auto tmp = p.string() + ".tmp";
    { std::ofstream f(tmp, std::ios::trunc); f << j.dump(2); }
    fs::rename(tmp, p, ec);
}
ExtraResult Ok(json payload) { ExtraResult r; r.handled = true; r.payload = std::move(payload); return r; }
ExtraResult Fail(std::string e) { ExtraResult r; r.handled = true; r.ok = false; r.error = std::move(e); return r; }
bool NonEmpty(const json& p, const char* k) { return p.contains(k) && p[k].is_string() && !p[k].get<std::string>().empty(); }
// closedObject: reject unknown keys like upstream's TypeBox validators.
std::string ClosedCheck(const json& p, std::initializer_list<const char*> allowed) {
    if (!p.is_object()) return "params must be an object";
    for (auto& [k, v] : p.items()) {
        (void)v;
        bool ok = false;
        for (auto* a : allowed) if (k == a) ok = true;
        if (!ok) return "invalid params: unexpected property '" + k + "'";
    }
    return {};
}

std::mutex g_mu;  // guards the small JSON ledgers below

// ---------------------------------------------------------------- process helper
thread_local std::string t_principal = "operator";  // current RPC requester (SetRpcRequester)
thread_local bool t_full_write = true;
std::string Trim(std::string s) { while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back(); auto a = s.find_first_not_of(" \t\r\n"); return a == std::string::npos ? std::string() : s.substr(a); }
struct Proc { int code = -1; std::string out, err; };
Proc Run(const std::vector<std::string>& argv, const fs::path& cwd, const std::vector<std::string>& env = {},
         const std::string& input = {}) {
    Proc r;
    int in[2], out[2], er[2];
    if (pipe(in) || pipe(out) || pipe(er)) return r;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0); dup2(out[1], 1); dup2(er[1], 2);
        close(in[1]); close(out[0]); close(er[0]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        for (auto& e : env) putenv(const_cast<char*>(e.c_str()));
        std::vector<char*> a; for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str())); a.push_back(nullptr);
        execvp(a[0], a.data()); _exit(127);
    }
    close(in[0]); close(out[1]); close(er[1]);
    if (!input.empty()) { auto w = write(in[1], input.data(), input.size()); (void)w; }
    close(in[1]);
    auto drain = [](int fd, std::string& s) { char b[4096]; ssize_t n; while ((n = read(fd, b, sizeof b)) > 0) s.append(b, n); close(fd); };
    drain(out[0], r.out); drain(er[0], r.err);
    int st = 0; waitpid(pid, &st, 0);
    r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    while (!r.out.empty() && (r.out.back() == '\n' || r.out.back() == '\r')) r.out.pop_back();
    return r;
}

// ---------------------------------------------------------------- HTTP (GitHub API)
std::size_t Sink(char* p, std::size_t s, std::size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }
struct Http { long status = 0; std::string body, error; };
Http GitHubRequest(const std::string& url, const std::string& method, const std::string& token, const std::string& body = {}) {
    Http r;
    CURL* c = curl_easy_init(); if (!c) { r.error = "curl init failed"; return r; }
    curl_slist* h = nullptr;
    h = curl_slist_append(h, "Accept: application/vnd.github+json");
    h = curl_slist_append(h, "X-GitHub-Api-Version: 2022-11-28");
    if (!token.empty()) h = curl_slist_append(h, ("Authorization: Bearer " + token).c_str());
    if (!body.empty()) h = curl_slist_append(h, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "openclaw-cpp");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    if (method != "GET") curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (!body.empty()) curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    auto rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(h); curl_easy_cleanup(c);
    return r;
}
std::string UrlEncode(const std::string& s) {
    std::string o; char b[4];
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += static_cast<char>(ch);
        else { std::snprintf(b, sizeof b, "%%%02X", ch); o += b; }
    }
    return o;
}
std::string GitHubToken() {
    for (const char* k : {"OPENCLAW_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN"})
        if (const char* v = std::getenv(k); v && *v) return v;
    return {};
}
std::string GitHubHost(const json& raw) {
    std::string h;
    if (auto v = config::GetPath(raw, "gateway.github.host"); v && v->is_string()) h = v->get<std::string>();
    if (h.empty()) if (const char* e = std::getenv("OPENCLAW_GITHUB_HOST"); e && *e) h = e;
    if (h.empty()) h = "github.com";
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    static const std::regex ok("^[a-z0-9](?:[a-z0-9.:-]*[a-z0-9])?$");
    if (!std::regex_match(h, ok) || h.find("..") != std::string::npos) throw std::runtime_error("gateway.github.host must be a hostname");
    return h;
}

// ---------------------------------------------------------------- companion side chat
// Upstream keeps the companion thread in memory per (agent, session); ≤24 exchanges, question ≤400, answer ≤1200.
std::map<std::string, std::deque<json>> g_companion;
constexpr std::size_t kCompanionMax = 24;

std::string AgentOf(const json& p) {
    if (p.contains("agentId") && p["agentId"].is_string()) return p["agentId"].get<std::string>();
    const std::string key = p.value("sessionKey", p.value("key", std::string()));
    auto a = agent::routing::AgentFromSessionKey(key);
    return a.empty() ? std::string("main") : a;
}
session::Store& SessionsOf(ServerDeps& d, const std::string& agent, const std::string& key = {}) {
    if (d.agent_pool) {
        auto& s = d.agent_pool->Sessions(agent);
        if (key.empty() || s.Exists(key) || !d.sessions.Exists(key)) return s;
    }
    return d.sessions;  // legacy shared store (sessions created through the plain RPC path)
}
fs::path WorkspaceOf(ServerDeps& d, const std::string& agent) {
    return agent::ResolveIsolation(d.config.raw, agent).workspace_root;
}
std::string Utf8Cut(const std::string& s, std::size_t max) {
    if (s.size() <= max) return s;
    std::size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

ExtraResult Companion(ServerDeps& d, const std::string& method, const json& p) {
    const bool ask = method == "sessions.companion.ask";
    auto bad = ask ? ClosedCheck(p, {"sessionKey", "agentId", "question", "selectionContext", "attachments"})
                   : ClosedCheck(p, {"sessionKey", "agentId"});
    if (!bad.empty()) return Fail(bad);
    if (!NonEmpty(p, "sessionKey")) return Fail("invalid params: sessionKey required");
    const auto agent = AgentOf(p), key = p["sessionKey"].get<std::string>();
    auto& store = SessionsOf(d, agent, key);
    if (!store.Exists(key)) return Fail("session not found: " + key);
    const auto tk = agent + "\n" + key;
    if (method == "sessions.companion.reset") {
        std::lock_guard<std::mutex> l(g_mu);
        g_companion.erase(tk);
        return Ok({{"ok", true}});
    }
    if (method == "sessions.companion.state") {
        std::lock_guard<std::mutex> l(g_mu);
        json ex = json::array();
        if (auto it = g_companion.find(tk); it != g_companion.end()) for (auto& e : it->second) ex.push_back(e);
        return Ok({{"exchanges", ex}});
    }
    if (!NonEmpty(p, "question")) return Fail("invalid params: question required");
    const auto q = p["question"].get<std::string>();
    if (q.size() > 400) return Fail("invalid params: question exceeds 400 characters");
    // Read-only context: recent transcript + workspace path. The companion never runs tools or writes the session.
    auto msgs = store.Load(key);
    std::string transcript;
    const std::size_t from = msgs.size() > 30 ? msgs.size() - 30 : 0;
    for (std::size_t i = from; i < msgs.size(); ++i) {
        if (msgs[i].role == "system") continue;
        transcript += msgs[i].role + ": " + Utf8Cut(msgs[i].content, 1500) + "\n";
    }
    if (transcript.size() > 12000) transcript = transcript.substr(transcript.size() - 12000);
    std::vector<agent::Message> conv;
    agent::Message sys; sys.role = "system";
    sys.content = "You are the read-only companion for one OpenClaw session. Answer the operator's side question about "
                  "the session and its workspace concisely (under 1200 characters). You cannot run tools, send messages, "
                  "or change the session.\nWorkspace: " + WorkspaceOf(d, agent).string() + "\nSession transcript (most recent last):\n" + transcript;
    conv.push_back(sys);
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (auto it = g_companion.find(tk); it != g_companion.end())
            for (auto& e : it->second) {
                agent::Message u; u.role = "user"; u.content = e["question"]; conv.push_back(u);
                agent::Message a; a.role = "assistant"; a.content = e["answer"]; conv.push_back(a);
            }
    }
    agent::Message u; u.role = "user";
    u.content = q;
    if (NonEmpty(p, "selectionContext")) u.content = "Selected text:\n" + p["selectionContext"].get<std::string>() + "\n\nQuestion: " + q;
    conv.push_back(u);
    auto& prov = d.provider;
    auto res = prov.Complete(conv, json::array());
    if (!res.ok) return Fail("companion unavailable: " + res.error);
    auto answer = Utf8Cut(res.message.content, 1200);
    if (answer.empty()) return Fail("companion returned an empty answer");
    const auto ts = NowMs();
    json ex{{"question", q}, {"answer", answer}, {"ts", ts}};
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto& th = g_companion[tk];
        th.push_back(ex);
        while (th.size() > kCompanionMax) th.pop_front();
    }
    return Ok({{"answer", answer}, {"ts", ts}});
}

// ---------------------------------------------------------------- worker placement ledger
// Persistent per-session placement records (placements.json). Device workers come from the node registry
// (paired, online desktop nodes); cloud profiles from gateway.workers.profiles. The ledger implements upstream's
// state machine (local → active → reclaimed / failed → local) with generations and owner epochs.
fs::path PlacementsPath(const fs::path& root) { return root / "worker-placements.json"; }

json PlacementView(const json& rec) {
    json v = rec;
    v.erase("deviceId");
    return v;
}
std::string SessionIdOf(session::Store& s, const std::string& key) {
    auto e = s.IndexEntry(key);
    if (e.is_object() && e.contains("sessionId") && e["sessionId"].is_string()) return e["sessionId"].get<std::string>();
    return key;
}
std::string WorkerBundleHash() { return ShaHex(std::string("openclaw-cpp-worker-bundle:" PROJECT_VERSION_STR)); }
std::string ManifestRef(const fs::path& ws) {
    std::vector<std::string> lines;
    std::error_code ec;
    if (fs::exists(ws, ec))
        for (auto it = fs::recursive_directory_iterator(ws, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator() && lines.size() < 20000; it.increment(ec)) {
            if (ec) break;
            if (it->path().filename() == ".git") { it.disable_recursion_pending(); continue; }
            if (it->is_regular_file(ec)) lines.push_back(fs::relative(it->path(), ws, ec).string() + ":" + std::to_string(it->file_size(ec)));
        }
    std::sort(lines.begin(), lines.end());
    std::string all; for (auto& l : lines) all += l + "\n";
    return "manifest:sha256:" + ShaHex(all);
}

ExtraResult Placement(ServerDeps& d, const fs::path& root, const std::string& method, const json& p) {
    std::string bad;
    if (method == "sessions.dispatch") bad = ClosedCheck(p, {"key", "agentId", "profileId", "deviceId", "autoDevice", "machineClass", "os"});
    else if (method == "sessions.move") bad = ClosedCheck(p, {"key", "agentId", "target", "expectedSource"});
    else bad = ClosedCheck(p, {"key", "agentId", "recoverToGateway"});
    if (!bad.empty()) return Fail(bad);
    if (!NonEmpty(p, "key")) return Fail("invalid params: key required");
    const auto agent = AgentOf(p), key = p["key"].get<std::string>();
    auto& store = SessionsOf(d, agent, key);
    if (!store.Exists(key)) return Fail("session not found: " + key);
    const auto sid = SessionIdOf(store, key);
    std::lock_guard<std::mutex> l(g_mu);
    auto db = ReadJsonFile(PlacementsPath(root), json::object());
    const auto rk = agent + ":" + key;
    const auto now = NowMs();
    json cur = db.contains(rk) ? db[rk] : json{{"state", "local"}, {"generation", 0}, {"createdAtMs", NowMs()}, {"updatedAtMs", NowMs()}, {"stateChangedAtMs", NowMs()}};
    std::string st = cur.value("state", "local");
    // A device worker that disappeared or went offline turns an active placement into a recoverable failure.
    if (st == "active" && cur.contains("deviceId")) {
        bool alive = false;
        for (auto& n : d.nodes.List()) if (n.id == cur["deviceId"].get<std::string>() && n.online) alive = true;
        if (!alive) {
            json f{{"state", "failed"}, {"generation", cur.value("generation", 0)}, {"createdAtMs", cur.value("createdAtMs", now)},
                   {"updatedAtMs", now}, {"stateChangedAtMs", now}, {"recoveryError", "device worker went offline"},
                   {"recoveryAction", "restart"}, {"terminalAtMs", now}};
            for (const char* k : {"environmentId", "activeOwnerEpoch", "workerBundleHash", "workspaceBaseManifestRef", "remoteWorkspaceDir"})
                if (cur.contains(k)) f[k] = cur[k];
            cur = f; st = "failed"; db[rk] = cur; WriteJsonFile(PlacementsPath(root), db);
        }
    }
    auto commit = [&](json next) {
        next["updatedAtMs"] = now;
        if (next.value("state", "") != st) next["stateChangedAtMs"] = now;
        db[rk] = next;
        WriteJsonFile(PlacementsPath(root), db);
        if (d.emit) d.emit("sessions.changed", {{"reason", method.substr(9)}, {"sessionKey", key}});
        return Ok({{"ok", true}, {"key", key}, {"sessionId", sid}, {"placement", PlacementView(next)}});
    };
    auto to_device = [&](const std::string& device_id) -> ExtraResult {
        if (st == "active" || st == "draining" || st == "reconciling" || st == "starting" || st == "syncing" || st == "provisioning" || st == "requested")
            return Fail("session is already placed on a worker (state " + st + "); reclaim it first");
        nodes::Node node; bool found = false;
        for (auto& n : d.nodes.List()) if (n.id == device_id) { node = n; found = true; }
        if (!found) return Fail("unknown device worker: " + device_id);
        if (!node.online) return Fail("device worker is offline: " + device_id);
        if (node.kind == "stub") return Fail("device worker cannot run sessions: " + device_id);
        const int gen = cur.value("generation", 0) + 1;
        json next{{"state", "active"}, {"generation", gen}, {"createdAtMs", cur.value("createdAtMs", now)},
                  {"stateChangedAtMs", now}, {"environmentId", "device:" + device_id}, {"activeOwnerEpoch", gen},
                  {"workerBundleHash", WorkerBundleHash()}, {"workspaceBaseManifestRef", ManifestRef(WorkspaceOf(d, agent))},
                  {"remoteWorkspaceDir", "~/.openclaw/worker/sessions/" + sid}, {"inference", "worker"},
                  {"runner", {{"kind", "device"}, {"status", "available"}, {"deviceId", device_id}}}, {"deviceId", device_id}};
        if (p.contains("machineClass") || p.contains("os")) {
            next["machine"] = json::object();
            if (p.contains("machineClass")) next["machine"]["class"] = p["machineClass"];
            if (p.contains("os")) next["machine"]["os"] = p["os"];
        }
        return commit(next);
    };
    auto to_profile = [&](const std::string& profile) -> ExtraResult {
        auto profs = config::GetPath(d.config.raw, "gateway.workers.profiles");
        if (!profs || !profs->is_object() || !profs->contains(profile))
            return Fail("worker profile not configured: " + profile + " (gateway.workers.profiles)");
        return Fail("cloud worker provider for profile '" + profile + "' is not available in the C++ runtime");
    };
    auto to_gateway = [&](bool recover, int expected_gen) -> ExtraResult {
        if (st == "local" || st == "reclaimed") {
            if (recover) return Fail("recoverToGateway applies only to failed placements");
            return Fail("session is not placed on a worker");
        }
        if (st == "failed") {
            if (!recover) return Fail("placement failed; pass recoverToGateway.expectedGeneration to recover it");
            if (expected_gen != cur.value("generation", 0)) return Fail("placement generation changed; expected " + std::to_string(expected_gen) + ", found " + std::to_string(cur.value("generation", 0)));
            json next{{"state", "local"}, {"generation", cur.value("generation", 0) + 1}, {"createdAtMs", cur.value("createdAtMs", now)}, {"stateChangedAtMs", now}};
            return commit(next);
        }
        json next{{"state", "reclaimed"}, {"generation", cur.value("generation", 0) + 1}, {"createdAtMs", cur.value("createdAtMs", now)},
                  {"stateChangedAtMs", now}, {"terminalReason", "reclaimed by operator"}, {"terminalAtMs", now}};
        for (const char* k : {"environmentId", "activeOwnerEpoch", "workerBundleHash", "workspaceBaseManifestRef", "remoteWorkspaceDir", "machine"})
            if (cur.contains(k)) next[k] = cur[k];
        return commit(next);
    };

    if (method == "sessions.dispatch") {
        const int modes = p.contains("profileId") + p.contains("deviceId") + p.contains("autoDevice");
        if (modes != 1) return Fail("invalid params: exactly one of profileId, deviceId, autoDevice");
        if (p.contains("profileId")) return to_profile(p.value("profileId", ""));
        if (p.contains("deviceId")) return to_device(p.value("deviceId", ""));
        for (auto& n : d.nodes.List()) if (n.online && n.kind != "stub") return to_device(n.id);
        return Fail("no online device worker is available");
    }
    if (method == "sessions.move") {
        if (!p.contains("target") || !p["target"].is_object()) return Fail("invalid params: target required");
        if (p.contains("expectedSource")) {
            const auto& e = p["expectedSource"];
            if (e.value("generation", -1) != cur.value("generation", 0) || e.value("environmentId", "") != cur.value("environmentId", "") ||
                e.value("ownerEpoch", -1) != cur.value("activeOwnerEpoch", 0))
                return Fail("placement source changed since it was observed");
        }
        const auto kind = p["target"].value("kind", "");
        if (kind == "gateway") return to_gateway(false, 0);
        if (kind == "device") {
            if (st == "active") { auto r = to_gateway(false, 0); if (!r.ok) return r; cur = db[rk]; }
            st = cur.value("state", "local");
            return to_device(p["target"].value("deviceId", ""));
        }
        if (kind == "profile") return to_profile(p["target"].value("profileId", ""));
        return Fail("invalid params: unknown target kind");
    }
    // sessions.reclaim
    const bool recover = p.contains("recoverToGateway");
    if (recover && !p["recoverToGateway"].is_object()) return Fail("invalid params: recoverToGateway must be {expectedGeneration}");
    const int eg = recover ? p["recoverToGateway"].value("expectedGeneration", -1) : 0;
    return to_gateway(recover, eg);
}

// ---------------------------------------------------------------- claws removal journal
// State "database" for claw provenance: <data>/claws/state.json {installs, journals, leases}. The request must be
// bound to this gateway's config/state/cron paths, the physical state file identity, the current config digest,
// and the expected install + journal digests, exactly as upstream's mutateClawRemovalJournalInWorker checks.
fs::path ClawStatePath(const fs::path& root) { return root / "claws" / "state.json"; }

bool IsReservedAgent(const std::string& id) { return id == "main" || id == "system" || id == "default"; }

ExtraResult RemovalJournal(ServerDeps& d, const fs::path& root, const json& p) {
    auto bad = ClosedCheck(p, {"phase", "agentId", "operationId", "binding", "sourceIdentity", "lease",
                               "expectedInstallDigest", "expectedJournalDigest", "configDigest"});
    if (!bad.empty()) return Fail("Invalid Claw removal journal parameters. (" + bad + ")");
    static const std::regex agent_re("^[a-z0-9][a-z0-9_-]*$"), digest_re("^sha256:[a-f0-9]{64}$");
    const auto phase = p.value("phase", ""), agent = p.value("agentId", ""), op = p.value("operationId", "");
    bool valid = (phase == "begin" || phase == "rollback") && agent.size() <= 128 && std::regex_match(agent, agent_re) &&
                 !op.empty() && op.size() <= 4096 && p.contains("binding") && p["binding"].is_object() &&
                 p.contains("sourceIdentity") && p["sourceIdentity"].is_object() && p.contains("lease") && p["lease"].is_object();
    for (const char* k : {"expectedInstallDigest", "expectedJournalDigest", "configDigest"})
        valid = valid && p.contains(k) && p[k].is_string() && std::regex_match(p[k].get<std::string>(), digest_re);
    if (valid) {
        const auto& lease = p["lease"];
        valid = lease.value("scope", "") == "core:agent-deletion" && lease.value("key", "") == agent && !lease.value("owner", "").empty() && lease.size() == 3;
        static const std::regex key_re("^file:(?:0|[1-9]\\d{0,19}):(?:0|[1-9]\\d{0,19})$");
        valid = valid && std::regex_match(p["sourceIdentity"].value("key", ""), key_re);
    }
    if (!valid || p.dump().size() > 256 * 1024) return Fail("Invalid Claw removal journal parameters.");
    std::lock_guard<std::mutex> l(g_mu);
    if (p["binding"] != ClawRemovalBinding(root) || ClawDigest(d.config.raw) != p["configDigest"].get<std::string>())
        return Fail("Claw removal no longer owns the serving Gateway's configuration or request authority. (outcomeUnknown=false)");
    if (p["sourceIdentity"] != ClawStateIdentity(root))
        return Fail("Claw journal mutation no longer owns its original physical database. (outcomeUnknown=false)");
    if (IsReservedAgent(agent)) return Fail("Claw journal mutation differs from its deletion owner or configuration.");
    auto st = ClawStateRead(root);
    auto& leases = st["leases"];
    if (leases.contains(agent) && leases[agent].value("owner", "") != p["lease"].value("owner", ""))
        return Fail("agent deletion lease is held by another owner");
    const json install = st["installs"].contains(agent) ? st["installs"][agent] : json(nullptr);
    const json previous = st["journals"].contains(agent) ? st["journals"][agent] : json(nullptr);
    if (ClawDigest(install) != p["expectedInstallDigest"].get<std::string>() || ClawDigest(previous) != p["expectedJournalDigest"].get<std::string>())
        return Fail("Claw install or deletion journal changed before mutation; preview removal again.");
    json journal = nullptr;
    if (phase == "begin") {
        auto iso = agent::ResolveIsolation(d.config.raw, agent);
        std::string ws = iso.def.workspace.empty() ? (install.is_object() ? install.value("workspace", std::string()) : std::string()) : iso.def.workspace.string();
        if (ws.empty()) ws = iso.workspace_root.string();
        const auto agent_dir = (agent::AgentsRoot() / agent).string();
        json cleanup = json::array();
        for (auto& [path, covers] : std::vector<std::pair<std::string, bool>>{{agent_dir, true}, {iso.sessions_root.string(), true}, {ws, true}}) {
            if (path.empty()) continue;
            std::error_code ec;
            auto canon = fs::weakly_canonical(path, ec).string();
            struct stat sb{};
            const bool exists = ::lstat(path.c_str(), &sb) == 0;
            cleanup.push_back({{"path", path}, {"canonicalPath", canon.empty() ? path : canon}, {"parentPath", fs::path(path).parent_path().string()},
                               {"kind", exists && S_ISLNK(sb.st_mode) ? "symlink" : "target"}, {"sourcePaths", json::array({path})},
                               {"dev", exists ? json(static_cast<double>(sb.st_dev)) : json(nullptr)},
                               {"ino", exists ? json(static_cast<double>(sb.st_ino)) : json(nullptr)},
                               {"coversDescendants", covers}, {"done", false}});
        }
        journal = {{"agentId", agent}, {"operationId", op}, {"agentDir", agent_dir}, {"workspaceDir", ws},
                   {"sessionsDir", iso.sessions_root.string()}, {"databasePaths", json::array({ClawStatePath(root).string()})},
                   {"cleanupPaths", cleanup}, {"createdAt", NowMs()}, {"cleanupCompleted", false},
                   {"deleteFiles", previous.is_object() ? previous.value("deleteFiles", false) : false}};
        st["journals"][agent] = journal;
        leases[agent] = p["lease"];
    } else {
        if (!previous.is_object() || previous.value("operationId", "") != op || previous.value("cleanupCompleted", false))
            return Fail("Claw rollback no longer owns its deletion journal.");
        st["journals"].erase(agent);
        leases.erase(agent);
    }
    { std::ofstream f(ClawStatePath(root), std::ios::trunc); f << st.dump(2); }
    if (d.emit) d.emit("sessions.changed", {{"all", true}, {"scope", "stores"}});
    return Ok({{"ok", true}, {"journal", journal}});
}


// ---------------------------------------------------------------- projects.{list,register,add,remove}
fs::path ProjectsPath(const fs::path& root) { return root / "projects.json"; }
std::string ProjectId(const json& db, std::string base) {
    std::string id;
    for (char c : base) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); if (std::isalnum(static_cast<unsigned char>(c))) id += c; else if (!id.empty() && id.back() != '-') id += '-'; }
    while (!id.empty() && id.back() == '-') id.pop_back();
    if (id.empty()) id = "project";
    if (id.size() > 56) id.resize(56);
    std::string out = id;
    for (int n = 2; db["projects"].contains(out) || out == "workspace"; ++n) out = id + "-" + std::to_string(n);
    return out;
}
ExtraResult Projects(ServerDeps& d, const fs::path& root, const std::string& method, const json& p) {
    std::lock_guard<std::mutex> l(g_mu);
    auto db = ReadJsonFile(ProjectsPath(root), json{{"projects", json::object()}});
    if (!db.contains("projects") || !db["projects"].is_object()) db["projects"] = json::object();
    if (method == "projects.list") {
        if (auto bad = ClosedCheck(p, {"includeObserved"}); !bad.empty()) return Fail(bad);
        json out = json::array();
        const auto ws = WorkspaceOf(d, "main");
        json w{{"id", "workspace"}, {"displayName", ws.filename().string().empty() ? std::string("workspace") : ws.filename().string()}, {"source", "workspace"}, {"agentId", "main"}};
        if (t_full_write) w["repoRoot"] = ws.string();
        out.push_back(w);
        for (auto& [id, rec] : db["projects"].items()) {
            json r = rec;
            if (!t_full_write) { r.erase("repoRoot"); r.erase("originUrl"); }
            out.push_back(r);
        }
        return Ok({{"projects", out}, {"githubHost", GitHubHost(d.config.raw)}});
    }
    if (method == "projects.register") {
        if (auto bad = ClosedCheck(p, {"path", "name"}); !bad.empty()) return Fail(bad);
        if (!NonEmpty(p, "path")) return Fail("invalid params: path required");
        const fs::path path = fs::absolute(p["path"].get<std::string>());
        auto top = Run({"git", "rev-parse", "--show-toplevel"}, path);
        if (top.code != 0) return Fail("not a git repository: " + path.string());
        const auto repo = Trim(top.out);
        for (auto& [id, rec] : db["projects"].items()) if (rec.value("repoRoot", "") == repo) return Ok(rec);  // idempotent
        const auto name = p.value("name", fs::path(repo).filename().string());
        json rec{{"id", ProjectId(db, name)}, {"displayName", name}, {"repoRoot", repo}, {"source", "registered"}};
        if (auto o = Run({"git", "remote", "get-url", "origin"}, repo); o.code == 0 && !Trim(o.out).empty()) rec["originUrl"] = Trim(o.out);
        db["projects"][rec["id"].get<std::string>()] = rec;
        WriteJsonFile(ProjectsPath(root), db);
        return Ok(rec);
    }
    if (method == "projects.add") {
        if (auto bad = ClosedCheck(p, {"gitUrl", "name"}); !bad.empty()) return Fail(bad);
        if (!NonEmpty(p, "gitUrl")) return Fail("invalid params: gitUrl required");
        const auto url = p["gitUrl"].get<std::string>();
        if (url.size() > 2048 || url.rfind("-", 0) == 0 || !(url.rfind("https://", 0) == 0 || url.rfind("git@", 0) == 0 || url.rfind("ssh://", 0) == 0 || url.rfind("file://", 0) == 0))
            return Fail("invalid params: gitUrl must be an https, ssh or file git URL");
        std::string base = url;
        while (!base.empty() && base.back() == '/') base.pop_back();
        base = base.substr(base.find_last_of("/:") + 1);
        if (base.size() > 4 && base.substr(base.size() - 4) == ".git") base.resize(base.size() - 4);
        const auto name = p.value("name", base);
        const auto id = ProjectId(db, name);
        const fs::path dest = root / "projects" / id;
        std::error_code ec; fs::create_directories(dest.parent_path(), ec);
        auto c = Run({"git", "clone", "--quiet", "--", url, dest.string()}, root, {"GIT_TERMINAL_PROMPT=0"});
        if (c.code != 0) { fs::remove_all(dest, ec); return Fail("git clone failed: " + Trim(c.err).substr(0, 500)); }
        json rec{{"id", id}, {"displayName", name}, {"repoRoot", dest.string()}, {"originUrl", url}, {"source", "cloned"}};
        db["projects"][id] = rec;
        WriteJsonFile(ProjectsPath(root), db);
        return Ok(rec);
    }
    if (method == "projects.remove") {
        if (auto bad = ClosedCheck(p, {"id", "deleteCheckout"}); !bad.empty()) return Fail(bad);
        const auto id = p.value("id", std::string());
        if (!db["projects"].contains(id)) return Ok({{"removed", false}});
        const auto rec = db["projects"][id];
        if (p.value("deleteCheckout", false)) {
            if (rec.value("source", "") != "cloned") return Fail("deleteCheckout applies only to projects cloned by the gateway");
            std::error_code ec; fs::remove_all(rec.value("repoRoot", ""), ec);
        }
        db["projects"].erase(id);
        WriteJsonFile(ProjectsPath(root), db);
        return Ok({{"removed", true}});
    }
    return Fail("unknown projects method");
}

// ---------------------------------------------------------------- projects.searchRemote
struct SearchCacheEntry { std::int64_t expires = 0; json result; };
std::map<std::string, SearchCacheEntry> g_search_cache;

json ParseRepo(const json& v) {
    if (!v.is_object()) return nullptr;
    auto s = [&](const char* k) { return v.contains(k) && v[k].is_string() ? v[k].get<std::string>() : std::string(); };
    const auto full = s("full_name"), name = s("name"), clone = s("clone_url"), web = s("html_url");
    if (full.empty() || name.empty() || web.empty()) return nullptr;
    if (clone.rfind("https://", 0) != 0 && clone.rfind("http://", 0) != 0) return nullptr;
    json r{{"name", name.substr(0, 100)}, {"fullName", full.substr(0, 200)}, {"cloneUrl", clone}, {"webUrl", web.substr(0, 2048)},
           {"private", v.value("private", false) == true}};
    if (auto ds = s("description"); !ds.empty()) r["description"] = Utf8Cut(ds, 500);
    if (auto db = s("default_branch"); !db.empty()) r["defaultBranch"] = db.substr(0, 255);
    return r;
}
json RepoArray(const json& v) {
    json out = json::array();
    const json* items = v.is_array() ? &v : (v.is_object() && v.contains("items") && v["items"].is_array() ? &v["items"] : nullptr);
    if (items) for (auto& it : *items) if (auto r = ParseRepo(it); !r.is_null()) out.push_back(r);
    return out;
}
std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }

ExtraResult SearchRemote(ServerDeps& d, const json& p) {
    auto bad = ClosedCheck(p, {"query"});
    if (!bad.empty()) return Fail(bad);
    if (!NonEmpty(p, "query")) return Fail("invalid params: query required");
    std::string query = p["query"].get<std::string>();
    query.erase(0, query.find_first_not_of(" \t\r\n"));
    query.erase(query.find_last_not_of(" \t\r\n") + 1);
    if (query.empty() || query.size() > 256) return Fail("invalid params: query must be 1-256 characters");
    std::string api, host;
    try { api = GitHubApiBase(d.config.raw); host = GitHubHost(d.config.raw); } catch (const std::exception& e) { return Fail(e.what()); }
    const auto token = GitHubToken();
    const auto cache_key = Lower(query) + '\0' + host + '\0' + api + '\0' + (token.empty() ? "anon" : ShaHex(token).substr(0, 16));
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (auto it = g_search_cache.find(cache_key); it != g_search_cache.end() && it->second.expires > NowMs()) {
            auto r = it->second.result; return Ok(r);
        }
    }
    auto get = [&](const std::string& path, bool optional_auth, bool* failed) -> json {
        auto r = GitHubRequest(api + "/" + path, "GET", token);
        // withOptionalGitHubAuth: a rejected credential falls back to an anonymous read.
        if (optional_auth && !token.empty() && (r.status == 401 || r.status == 403)) r = GitHubRequest(api + "/" + path, "GET", "");
        if (!r.error.empty() || r.status < 200 || r.status >= 300) { if (failed) *failed = true; return nullptr; }
        try { return json::parse(r.body); } catch (...) { if (failed) *failed = true; return nullptr; }
    };
    static const std::regex exact_re("^[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?/[A-Za-z0-9._-]+$");
    json ranked = json::array();
    if (std::regex_match(query, exact_re)) if (auto r = ParseRepo(get("repos/" + query, true, nullptr)); !r.is_null()) ranked.push_back(r);
    if (!token.empty()) {
        auto aff = RepoArray(get("user/repos?affiliation=owner%2Ccollaborator%2Corganization_member&sort=updated&direction=desc&per_page=10", false, nullptr));
        const auto needle = Lower(query);
        for (auto& r : aff)
            if (Lower(r.value("name", "") + "\n" + r.value("fullName", "") + "\n" + r.value("description", "")).find(needle) != std::string::npos) ranked.push_back(r);
    }
    bool failed = false;
    auto global = get("search/repositories?q=" + UrlEncode(query + " in:name,description") + "&per_page=10", true, &failed);
    if (failed) return Fail("GitHub repository search failed");
    for (auto& r : RepoArray(global)) ranked.push_back(r);
    json projects = json::array(); std::set<std::string> seen;
    for (auto& r : ranked) {
        if (projects.size() >= 10) break;
        if (seen.insert(Lower(r["fullName"].get<std::string>())).second) projects.push_back(r);
    }
    json result{{"credential", token.empty() ? "missing" : "configured"}, {"projects", projects}};
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_search_cache[cache_key] = {NowMs() + 60000, result};
        while (g_search_cache.size() > 100) g_search_cache.erase(g_search_cache.begin());
    }
    return Ok(result);
}

// ---------------------------------------------------------------- sessions.github.publish / confirm
// Ledger: <data>/github-publications.json {requests: {requestId: record}, keys: {sessionScope\0idempotencyKey: requestId}}.
fs::path PubPath(const fs::path& root) { return root / "github-publications.json"; }
std::string Uuid() {
    auto h = RandomHex(16);
    h[12] = '4'; h[16] = "89ab"[std::stoi(h.substr(16, 1), nullptr, 16) & 3];
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}
json Failed(const std::string& id, const std::string& code, const std::string& msg, const std::string& next) {
    return {{"requestId", id}, {"status", "failed"}, {"code", code}, {"message", msg}, {"nextAction", next}};
}
// owner/repo from an origin URL on the configured host (https, ssh scp-like, ssh://).
std::string RepoFromRemote(const std::string& url, const std::string& host) {
    std::smatch m;
    const std::regex https_re("^https?://(?:[^@/]+@)?([^/]+)/([A-Za-z0-9-]+)/([A-Za-z0-9._-]+?)(?:\\.git)?/?$");
    const std::regex scp_re("^(?:ssh://)?git@([^:/]+)[:/]([A-Za-z0-9-]+)/([A-Za-z0-9._-]+?)(?:\\.git)?/?$");
    if ((std::regex_match(url, m, https_re) || std::regex_match(url, m, scp_re)) && Lower(m[1].str()) == host)
        return m[2].str() + "/" + m[3].str();
    return {};
}
std::string GitTransportUrl(const std::string& host, const std::string& repo) {
    // Tests (and GHES mirrors) may redirect git transport: OPENCLAW_GITHUB_GIT_URL_BASE=file:///tmp/remotes/
    if (const char* b = std::getenv("OPENCLAW_GITHUB_GIT_URL_BASE"); b && *b) return std::string(b) + repo + ".git";
    return "https://" + host + "/" + repo + ".git";
}
std::string Slug(const std::string& s) {
    std::string o;
    for (char c : s) o += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? static_cast<char>(std::tolower(c)) : '-';
    while (o.find("--") != std::string::npos) o.erase(o.find("--"), 1);
    if (o.size() > 60) o.resize(60);
    return o.empty() ? "session" : o;
}

// ---------------------------------------------------------------- personal GitHub sign-in (users.github.*)
// Per-principal GitHub device-flow authorizations: <data>/github-personal.json (0600)
//   {accounts: {principal: {generation, account{accountId,login}, token, connectedAtMs}}, pending: {requestId: {...}}}
fs::path PersonalPath(const fs::path& root) { return root / "github-personal.json"; }
json PersonalRead(const fs::path& root) {
    auto j = ReadJsonFile(PersonalPath(root), json::object());
    for (const char* k : {"accounts", "pending"}) if (!j.contains(k) || !j[k].is_object()) j[k] = json::object();
    return j;
}
void PersonalWrite(const fs::path& root, const json& j) {
    WriteJsonFile(PersonalPath(root), j);
    std::error_code ec; fs::permissions(PersonalPath(root), fs::perms::owner_read | fs::perms::owner_write, ec);
}
std::string OAuthBase(const json& raw) {
    if (const char* e = std::getenv("OPENCLAW_GITHUB_OAUTH_BASE_URL"); e && *e) return e;
    return "https://" + GitHubHost(raw);
}
std::string OAuthClientId(const json& raw) {
    if (auto v = config::GetPath(raw, "gateway.github.oauthClientId"); v && v->is_string() && !v->get<std::string>().empty()) return v->get<std::string>();
    if (const char* e = std::getenv("OPENCLAW_GITHUB_OAUTH_CLIENT_ID"); e && *e) return e;
    return {};
}
Http FormPost(const std::string& url, const std::string& form) {
    Http r;
    CURL* c = curl_easy_init(); if (!c) { r.error = "curl init failed"; return r; }
    curl_slist* h = curl_slist_append(nullptr, "Accept: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, form.c_str());
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "openclaw-cpp");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    auto rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(h); curl_easy_cleanup(c);
    return r;
}
json PersonalStatus(const json& db, const std::string& principal) {
    json st{{"state", "disconnected"}, {"generation", nullptr}, {"account", nullptr}, {"accessExpiresAtMs", nullptr},
            {"refreshState", "not_applicable"}, {"pending", nullptr}};
    if (db["accounts"].contains(principal)) {
        const auto& a = db["accounts"][principal];
        st["state"] = "connected"; st["generation"] = a["generation"]; st["account"] = a["account"];
        if (a.contains("accessExpiresAtMs")) { st["accessExpiresAtMs"] = a["accessExpiresAtMs"]; st["refreshState"] = a.value("accessExpiresAtMs", 0LL) > NowMs() ? "available" : "expired"; }
    }
    for (auto& [rid, p] : db["pending"].items())
        if (p.value("principal", "") == principal && p.value("expiresAtMs", 0LL) > NowMs())
            st["pending"] = {{"requestId", rid}, {"userCode", p["userCode"]}, {"verificationUri", p["verificationUri"]},
                             {"expiresInMs", std::max<long long>(0, p.value("expiresAtMs", 0LL) - NowMs())}, {"pollAfterMs", p.value("intervalMs", 5000)}};
    return st;
}

ExtraResult UsersGitHub(ServerDeps& d, const fs::path& root, const std::string& method, const json& p) {
    const auto& principal = t_principal;
    std::lock_guard<std::mutex> l(g_mu);
    auto db = PersonalRead(root);
    if (method == "users.github.status") {
        if (auto bad = ClosedCheck(p, {}); !bad.empty()) return Fail(bad);
        const auto tok = GitHubToken();
        json system{{"configured", !tok.empty()}, {"source", tok.empty() ? "none" : "env"}};
        return Ok({{"personal", PersonalStatus(db, principal)}, {"system", system}});
    }
    if (method == "users.github.disconnect") {
        if (auto bad = ClosedCheck(p, {}); !bad.empty()) return Fail(bad);
        db["accounts"].erase(principal);
        PersonalWrite(root, db);
        return Ok({{"disconnected", true}});
    }
    if (method == "users.github.authorize.start") {
        if (auto bad = ClosedCheck(p, {}); !bad.empty()) return Fail(bad);
        const auto cid = OAuthClientId(d.config.raw);
        if (cid.empty()) return Fail("GitHub sign-in is not configured (gateway.github.oauthClientId)");
        auto r = FormPost(OAuthBase(d.config.raw) + "/login/device/code", "client_id=" + UrlEncode(cid) + "&scope=" + UrlEncode("repo workflow read:user"));
        json j; try { j = json::parse(r.body); } catch (...) {}
        if (r.status != 200 || !j.contains("device_code")) return Fail("My GitHub authorization failed.");
        const auto rid = Uuid();
        const long long interval = std::clamp<long long>(j.value("interval", 5) * 1000LL, 1, 60000);
        const long long expires = std::min<long long>(j.value("expires_in", 900) * 1000LL, 900000);
        db["pending"][rid] = {{"principal", principal}, {"deviceCode", j["device_code"]}, {"userCode", j.value("user_code", "")},
                              {"verificationUri", "https://github.com/login/device"}, {"intervalMs", interval}, {"expiresAtMs", NowMs() + expires}};
        PersonalWrite(root, db);
        return Ok({{"requestId", rid}, {"userCode", j.value("user_code", "")}, {"verificationUri", "https://github.com/login/device"},
                   {"expiresInMs", expires}, {"pollAfterMs", interval}});
    }
    if (method == "users.github.authorize.poll" || method == "users.github.authorize.cancel") {
        if (auto bad = ClosedCheck(p, {"requestId"}); !bad.empty()) return Fail(bad);
        const auto rid = p.value("requestId", "");
        if (!db["pending"].contains(rid) || db["pending"][rid].value("principal", "") != principal) {
            if (method == "users.github.authorize.cancel") return Ok({{"cancelled", false}});
            return Ok({{"status", "expired"}});
        }
        auto pend = db["pending"][rid];
        if (method == "users.github.authorize.cancel") { db["pending"].erase(rid); PersonalWrite(root, db); return Ok({{"cancelled", true}}); }
        if (pend.value("expiresAtMs", 0LL) <= NowMs()) { db["pending"].erase(rid); PersonalWrite(root, db); return Ok({{"status", "expired"}}); }
        auto r = FormPost(OAuthBase(d.config.raw) + "/login/oauth/access_token",
                          "client_id=" + UrlEncode(OAuthClientId(d.config.raw)) + "&device_code=" + UrlEncode(pend["deviceCode"].get<std::string>()) +
                              "&grant_type=" + UrlEncode("urn:ietf:params:oauth:grant-type:device_code"));
        json j; try { j = json::parse(r.body); } catch (...) {}
        const long long iv = pend.value("intervalMs", 5000LL);
        if (!r.error.empty() || r.status >= 500) return Ok({{"status", "network_error"}, {"retryAfterMs", iv}});
        const auto e = j.value("error", std::string());
        if (e == "authorization_pending") return Ok({{"status", "pending"}, {"retryAfterMs", iv}});
        if (e == "slow_down") {
            const long long niv = std::min<long long>(60000, j.value("interval", iv / 1000 + 5) * 1000LL);
            db["pending"][rid]["intervalMs"] = niv; PersonalWrite(root, db);
            return Ok({{"status", "slow_down"}, {"retryAfterMs", niv}});
        }
        if (e == "access_denied" || e == "expired_token" || e == "incorrect_device_code") {
            db["pending"].erase(rid); PersonalWrite(root, db);
            return Ok({{"status", e == "access_denied" ? "access_denied" : e == "expired_token" ? "expired" : "incorrect_device_code"}});
        }
        const auto token = j.value("access_token", std::string());
        if (token.empty()) return Ok({{"status", "failed"}, {"reason", "setup_failed"}});
        std::string api;
        try { api = GitHubApiBase(d.config.raw); } catch (...) { return Ok({{"status", "failed"}, {"reason", "setup_failed"}}); }
        auto u = GitHubRequest(api + "/user", "GET", token);
        json acct;
        try { auto uj = json::parse(u.body); acct = {{"accountId", uj.at("id")}, {"login", uj.at("login")}}; } catch (...) {}
        if (u.status != 200 || acct.is_null()) return Ok({{"status", "failed"}, {"reason", "setup_failed"}});
        // A reconnect to a different GitHub account is a new generation; pending confirmations bound to the old one fail.
        json rec{{"generation", Uuid()}, {"account", acct}, {"token", token}, {"connectedAtMs", NowMs()}};
        if (j.contains("expires_in")) rec["accessExpiresAtMs"] = NowMs() + j.value("expires_in", 0LL) * 1000;
        db["accounts"][principal] = rec;
        db["pending"].erase(rid);
        PersonalWrite(root, db);
        return Ok({{"status", "success"}, {"personal", PersonalStatus(db, principal)}});
    }
    return Fail("unknown users.github method");
}

// Session participants (principals that prompted the session) → GitHub co-author credit when they linked GitHub.
fs::path ParticipantsPath(const fs::path& root) { return root / "session-participants.json"; }
std::string ParticipantKey(const std::string& agent, std::string key) {
    const auto pre = "agent:" + agent + ":";
    if (key.rfind(pre, 0) == 0) key = key.substr(pre.size());
    return agent + ":" + key;
}

json CoAuthors(const fs::path& root, const std::string& agent, const std::string& key, long long exclude_account) {
    json logins = json::array(), trailers = json::array();
    if (key.find(":incognito:") != std::string::npos) return {{"logins", logins}, {"trailers", trailers}};
    auto parts = ReadJsonFile(ParticipantsPath(root), json::object());
    const auto pk = ParticipantKey(agent, key);
    if (!parts.contains(pk)) return {{"logins", logins}, {"trailers", trailers}};
    auto db = PersonalRead(root);
    std::vector<std::pair<long long, json>> ordered;  // first prompt order
    for (auto& [principal, rec] : parts[pk].items()) {
        if (!db["accounts"].contains(principal)) continue;
        const auto& a = db["accounts"][principal]["account"];
        if (a.value("accountId", 0LL) == exclude_account) continue;
        ordered.push_back({rec.value("firstPromptedAt", 0LL), a});
    }
    std::sort(ordered.begin(), ordered.end(), [](auto& x, auto& y) { return x.first < y.first; });
    std::set<long long> seen;
    for (auto& [t, a] : ordered) {
        if (!seen.insert(a.value("accountId", 0LL)).second) continue;
        const std::string login = a.value("login", "");
        logins.push_back(login);
        trailers.push_back("Co-authored-by: " + login + " <" + std::to_string(a.value("accountId", 0LL)) + "+" + login + "@users.noreply.github.com>");
    }
    return {{"logins", logins}, {"trailers", trailers}};
}

bool IsWorkflowPath(const std::string& f) {
    static const std::regex re("^\\.github/workflows/[^/]+\\.ya?ml$", std::regex::icase);
    return std::regex_match(f, re);
}

json Execute(ServerDeps& d, const fs::path& root, json& rec) {
    const std::string id = rec["requestId"];
    const fs::path ws = rec["workspace"].get<std::string>();
    std::string token = GitHubToken();
    const bool personal = rec.contains("account");
    if (personal) {
        auto pdb = PersonalRead(root);
        const auto pr = rec.value("principal", "");
        if (!pdb["accounts"].contains(pr)) return Failed(id, "identity_unavailable", "Your GitHub account is not connected.", "Connect GitHub with users.github.authorize.start, then request publication again.");
        const auto& a = pdb["accounts"][pr];
        if (a.value("generation", "") != rec.value("generation", "") || a["account"] != rec["account"])
            return Failed(id, "identity_changed", "Your GitHub connection changed since publication was requested.", "Request publication again.");
        token = a.value("token", "");
    }
    std::string api, host;
    try { api = GitHubApiBase(d.config.raw); host = GitHubHost(d.config.raw); }
    catch (const std::exception& e) { return Failed(id, "unavailable", e.what(), "Fix gateway.github settings, then retry."); }
    if (token.empty()) return Failed(id, "identity_unavailable", "No GitHub credential is configured for publication.", "Set GH_TOKEN or GITHUB_TOKEN for the gateway, then retry.");
    auto user = GitHubRequest(api + "/user", "GET", token);
    json account;
    try { auto u = json::parse(user.body); account = {{"accountId", u.at("id")}, {"login", u.at("login")}}; } catch (...) {}
    if (user.status != 200 || account.is_null()) return Failed(id, "identity_unavailable", "GitHub identity could not be read.", "Re-authorize GitHub, then retry.");
    if (rec.contains("account") && rec["account"] != account)
        return Failed(id, "identity_changed", "GitHub account changed since publication was requested.", "Request publication again.");
    if (Run({"git", "rev-parse", "--is-inside-work-tree"}, ws).out != "true")
        return Failed(id, "not_git", "Session workspace is not a Git repository.", "Initialize a Git repository with a GitHub origin, then retry.");
    const auto origin = Run({"git", "remote", "get-url", "origin"}, ws).out;
    const auto repo = RepoFromRemote(origin, host);
    if (repo.empty()) return Failed(id, "not_github", "Session workspace origin is not a " + host + " repository.", "Add a GitHub origin remote, then retry.");
    const auto remote = GitTransportUrl(host, repo);
    std::string base;
    if (auto rr = GitHubRequest(api + "/repos/" + repo, "GET", token); rr.status == 200)
        try { base = json::parse(rr.body).value("default_branch", ""); } catch (...) {}
    if (base.empty()) base = "main";
    // Snapshot the working tree into a private index: never touches the user's index or HEAD.
    const auto idx = (fs::temp_directory_path() / ("openclaw-pub-" + RandomHex(6))).string();
    const std::vector<std::string> ienv{"GIT_INDEX_FILE=" + idx};
    Run({"git", "read-tree", "HEAD"}, ws, ienv);
    Run({"git", "add", "-A"}, ws, ienv);
    const auto tree = Run({"git", "write-tree"}, ws, ienv).out;
    std::error_code ec; fs::remove(idx, ec);
    const auto head = Run({"git", "rev-parse", "HEAD"}, ws).out;
    if (tree.empty() || head.empty()) return Failed(id, "workspace_changed", "Workspace snapshot failed.", "Commit or stash conflicting state, then retry.");
    const std::vector<std::string> genv{"GIT_TERMINAL_PROMPT=0", "GIT_CONFIG_NOSYSTEM=1"};
    auto auth_args = [&](std::vector<std::string> a) {
        if (remote.rfind("https://", 0) == 0) {
            std::string basic = "x-access-token:" + token, b64;
            static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            int val = 0, bits = -6;
            for (unsigned char c : basic) { val = (val << 8) + c; bits += 8; while (bits >= 0) { b64 += t[(val >> bits) & 63]; bits -= 6; } }
            if (bits > -6) b64 += t[((val << 8) >> (bits + 8)) & 63];
            while (b64.size() % 4) b64 += '=';
            a.insert(a.begin() + 1, {"-c", "http.extraHeader=Authorization: Basic " + b64});
        }
        return a;
    };
    auto ls = Run(auth_args({"git", "ls-remote", remote, "refs/heads/" + base}), ws, genv);
    if (ls.code != 0) return Failed(id, "unavailable", "GitHub repository could not be reached.", "Check network access and credentials, then retry.");
    const auto base_sha = ls.out.substr(0, ls.out.find_first_of(" \t"));
    if (!base_sha.empty()) {
        Run(auth_args({"git", "fetch", "--no-tags", "--quiet", remote, base_sha}), ws, genv);
        if (Run({"git", "rev-parse", base_sha + "^{tree}"}, ws).out == tree)
            return Failed(id, "no_changes", "GitHub publication has no changes to publish.", "Make or restore a repository change, then retry.");
        if (Run({"git", "merge-base", head, base_sha}, ws).code == 1)
            return Failed(id, "workspace_changed", "GitHub publication histories are unrelated.", "Apply the intended changes to a branch based on " + base + ", then retry.");
    }
    if (Run({"git", "rev-parse", "HEAD^{tree}"}, ws).out == tree && head == base_sha)
        return Failed(id, "no_changes", "GitHub publication has no changes to publish.", "Make or restore a repository change, then retry.");
    const std::string branch = "openclaw/" + Slug(rec.value("sessionKey", "session"));
    const std::string title = rec.value("title", "") .empty() ? "OpenClaw session " + rec.value("sessionKey", "") : rec.value("title", "");
    const std::string body = rec.value("body", "").empty() ? "Published by the Gateway after authoritative workspace reconciliation." : rec.value("body", "");
    // Co-author credit: session participants with a linked personal GitHub account (excluding the publisher).
    const auto credit = CoAuthors(root, rec.value("agentId", "main"), rec.value("sessionKey", ""), account.value("accountId", 0LL));
    std::string credit_list;
    for (auto& l : credit["logins"]) credit_list += "- @" + l.get<std::string>() + "\n";
    if (!credit_list.empty()) credit_list.pop_back();
    std::string msg = credit_list.empty() ? title : title + "\n\nWorked on by:\n" + credit_list;
    msg += "\n\n";
    for (auto& tr : credit["trailers"]) msg += tr.get<std::string>() + "\n";
    msg += "OpenClaw-Publication: " + id + "\n";
    const std::string login = account["login"], email = account["accountId"].dump() + "+" + login + "@users.noreply.github.com";
    std::vector<std::string> aenv{"GIT_AUTHOR_NAME=" + login, "GIT_COMMITTER_NAME=" + login, "GIT_AUTHOR_EMAIL=" + email, "GIT_COMMITTER_EMAIL=" + email};
    // Publications stack on the session branch: an existing remote branch must already contain the local HEAD
    // (it was built from it); otherwise someone else moved it and the push would clobber their work.
    std::string parent = head;
    auto rb = Run(auth_args({"git", "ls-remote", remote, "refs/heads/" + branch}), ws, genv);
    const auto remote_head = rb.out.substr(0, rb.out.find_first_of(" \t"));
    if (!remote_head.empty() && remote_head != head) {
        Run(auth_args({"git", "fetch", "--no-tags", "--quiet", remote, remote_head}), ws, genv);
        if (Run({"git", "merge-base", "--is-ancestor", head, remote_head}, ws).code != 0)
            return Failed(id, "push_rejected", "GitHub publication branch changed outside this session.", "Pull " + branch + " into the workspace or choose a new session branch, then retry.");
        if (Run({"git", "rev-parse", remote_head + "^{tree}"}, ws).out == tree)
            return Failed(id, "no_changes", "GitHub publication has no changes beyond the published branch.", "Make a further repository change, then retry.");
        parent = remote_head;
    }
    // Workflow guard: restricted requesters (agent tool calls) may not change .github/workflows/*.yml, except when
    // the accepted definition only mirrors the target branch (before == merge-base and accepted == target).
    if (!rec.value("fullWrite", true)) {
        auto changed = Run({"git", "diff", "--name-only", "--no-renames", parent, tree}, ws).out;
        std::istringstream ls_changed(changed);
        std::string f;
        const auto mb = base_sha.empty() ? std::string() : Run({"git", "merge-base", parent, base_sha}, ws).out;
        auto blob = [&](const std::string& rev, const std::string& path) {
            if (rev.empty()) return std::string("<none>");
            auto r = Run({"git", "rev-parse", "--verify", "--quiet", rev + ":" + path}, ws);
            return r.code == 0 ? r.out : std::string("<absent>");
        };
        while (std::getline(ls_changed, f)) {
            if (!IsWorkflowPath(f)) continue;
            if (base_sha.empty() || blob(parent, f) != blob(mb, f) || blob(tree, f) != blob(base_sha, f))
                return Failed(id, "github_rejected", "Publishing GitHub workflow changes requires full operator write permission.",
                              "Your saved changes are intact. Ask a maintainer with full write access to publish the GitHub workflow changes, or restore the workflow definitions and request publication again.");
        }
    }
    std::string commit = (parent == head && tree == Run({"git", "rev-parse", "HEAD^{tree}"}, ws).out) ? head
                        : Run({"git", "commit-tree", "--no-gpg-sign", tree, "-p", parent}, ws, aenv, msg).out;
    if (commit.empty()) return Failed(id, "workspace_changed", "Publication commit could not be created.", "Retry after resolving the workspace state.");
    rec["effect"] = {{"kind", "push"}, {"status", "dispatched"}, {"headCommit", commit}};
    auto push = Run(auth_args({"git", "push", "--porcelain", remote, commit + ":refs/heads/" + branch}), ws, genv);
    if (push.code != 0)
        return Failed(id, "push_rejected", "GitHub rejected the branch push.", "The remote branch " + branch + " moved; pull it or choose a new session branch, then retry.");
    rec["effect"] = {{"kind", "push"}, {"status", "observed"}, {"headCommit", commit}};
    const auto owner = repo.substr(0, repo.find('/'));
    std::string url;
    auto list = GitHubRequest(api + "/repos/" + repo + "/pulls?head=" + UrlEncode(owner + ":" + branch) + "&state=open&per_page=5", "GET", token);
    try { auto a = json::parse(list.body); if (a.is_array() && !a.empty()) url = a[0].value("html_url", ""); } catch (...) {}
    if (url.empty()) {
        json req{{"title", title}, {"head", branch}, {"base", base},
                 {"body", body + (credit_list.empty() ? std::string() : "\n\n## Worked on by\n\n" + credit_list) + "\n\n<!-- openclaw-publication:" + id + " -->"}};
        auto pr = GitHubRequest(api + "/repos/" + repo + "/pulls", "POST", token, req.dump());
        try { url = json::parse(pr.body).value("html_url", ""); } catch (...) {}
        if (pr.status != 201 || url.empty())
            return Failed(id, "github_rejected", "GitHub rejected the pull request (HTTP " + std::to_string(pr.status) + ").", "Open the pull request for " + branch + " manually or retry.");
    }
    rec["effect"] = {{"kind", "pull_request"}, {"status", "observed"}, {"headCommit", commit}, {"url", url}};
    json out{{"requestId", id}, {"status", "published"}, {"url", url}, {"repository", repo}, {"branch", branch}, {"headCommit", commit},
             {"publisher", {{"source", rec.contains("account") ? "personal" : "system-configured"}, {"accountId", account["accountId"]}, {"login", login}}}};
    out["effect"] = rec["effect"];
    return out;
}

ExtraResult GitHubPublish(ServerDeps& d, const fs::path& root, const std::string& method, const json& p) {
    const bool confirm = method == "sessions.github.confirm";
    auto bad = confirm ? ClosedCheck(p, {"sessionKey", "agentId", "requestId", "generation", "account", "requestDigest"})
                       : ClosedCheck(p, {"sessionKey", "agentId", "idempotencyKey", "title", "body", "selection"});
    if (!bad.empty()) return Fail(bad);
    const auto agent = AgentOf(p);
    const std::string key = p.value("sessionKey", std::string("main"));
    std::unique_lock<std::mutex> l(g_mu);
    auto db = ReadJsonFile(PubPath(root), json{{"requests", json::object()}, {"keys", json::object()}});
    auto save = [&] { WriteJsonFile(PubPath(root), db); };
    auto project = [](const json& rec) {
        json r = rec["result"];
        if (rec.contains("effect") && !r.contains("effect")) r["effect"] = rec["effect"];
        return r;
    };
    if (confirm) {
        if (!NonEmpty(p, "requestId") || !NonEmpty(p, "generation") || !p.contains("account") || !NonEmpty(p, "requestDigest"))
            return Fail("invalid params: requestId, generation, account, requestDigest required");
        const auto rid = p["requestId"].get<std::string>();
        if (!db["requests"].contains(rid)) return Fail("GitHub publication request not found");
        auto& rec = db["requests"][rid];
        if (rec.value("sessionKey", "") != key || rec.value("agentId", "") != agent) return Fail("GitHub publication belongs to another session");
        if (rec.value("principal", t_principal) != t_principal) return Fail("GitHub publication was requested by another user");
        if (rec["result"].value("status", "") != "needs_confirmation") return Ok(project(rec));  // already settled: idempotent
        if (rec.value("generation", json()) != p["generation"] || rec["account"] != p["account"] || rec.value("requestDigest", json()) != p["requestDigest"])
            return Fail("GitHub publication confirmation does not match the pending request");
        rec["result"] = {{"requestId", rid}, {"status", "publishing"}, {"message", "Publishing to GitHub."}};
        save();
        l.unlock();
        json copy = rec;
        auto out = Execute(d, root, copy);
        l.lock();
        db = ReadJsonFile(PubPath(root), db);
        copy["result"] = out;
        db["requests"][rid] = copy;
        save();
        return Ok(project(copy));
    }
    if (!NonEmpty(p, "idempotencyKey")) return Fail("invalid params: idempotencyKey required");
    if (p.contains("title")) {
        if (!p["title"].is_string()) return Fail("invalid params: title");
        const auto t = p["title"].get<std::string>();
        if (t.empty() || t.size() > 256 || t.find_first_of("\r\n") != std::string::npos || t.find_first_not_of(" \t") == std::string::npos) return Fail("invalid params: title");
    }
    if (p.contains("body") && (!p["body"].is_string() || p["body"].get<std::string>().empty() || p["body"].get<std::string>().size() > 8192)) return Fail("invalid params: body");
    auto& store = SessionsOf(d, agent, key);
    if (!store.Exists(key)) return Fail("session not found: " + key);
    json canonical{{"sessionKey", key}, {"agentId", agent}, {"title", p.value("title", "")}, {"body", p.value("body", "")},
                   {"selection", p.contains("selection") ? p["selection"] : json(nullptr)}, {"sessionId", SessionIdOf(store, key)}};
    const auto digest = ShaHex(canonical.dump());
    const auto scope = agent + ":" + key + "\n" + p["idempotencyKey"].get<std::string>();
    if (db["keys"].contains(scope)) {
        const auto rid = db["keys"][scope].get<std::string>();
        auto& rec = db["requests"][rid];
        if (rec.value("requestDigest", "") != digest) return Fail("idempotencyKey was already used for a different publication request");
        return Ok(project(rec));
    }
    const auto rid = Uuid();
    json rec{{"requestId", rid}, {"sessionKey", key}, {"agentId", agent}, {"requestDigest", digest}, {"createdAtMs", NowMs()},
             {"workspace", WorkspaceOf(d, agent).string()}, {"title", p.value("title", "")}, {"body", p.value("body", "")},
             {"principal", t_principal}, {"fullWrite", t_full_write}};
    const auto& sel = p.contains("selection") ? p["selection"] : json(nullptr);
    if (sel.is_object() && sel.value("source", "") == "personal") {
        if (!NonEmpty(sel, "generation") || !sel.contains("account") || !sel["account"].is_object())
            return Fail("invalid params: personal selection requires generation and account");
        {
            auto pdb = PersonalRead(root);
            if (!pdb["accounts"].contains(t_principal) || pdb["accounts"][t_principal]["generation"] != sel["generation"] ||
                pdb["accounts"][t_principal]["account"] != sel["account"])
                return Ok(Failed(rid, "identity_changed", "The selected GitHub account is not your current connection.", "Refresh My GitHub and request publication again."));
        }
        rec["generation"] = sel["generation"]; rec["account"] = sel["account"];
        rec["result"] = {{"requestId", rid}, {"status", "needs_confirmation"},
                         {"message", "Confirm publishing as @" + sel["account"].value("login", "") + " with sessions.github.confirm."}};
        db["requests"][rid] = rec; db["keys"][scope] = rid; save();
        return Ok(project(rec));
    }
    rec["result"] = {{"requestId", rid}, {"status", "publishing"}, {"message", "Publishing to GitHub."}};
    db["requests"][rid] = rec; db["keys"][scope] = rid; save();
    l.unlock();
    auto out = Execute(d, root, rec);
    l.lock();
    db = ReadJsonFile(PubPath(root), db);
    rec["result"] = out;
    db["requests"][rid] = rec;
    save();
    return Ok(project(rec));
}

}  // namespace

std::string ClawDigest(const json& value) { return "sha256:" + ShaHex(value.dump()); }

json ClawRemovalBinding(const fs::path& root) {
    std::error_code ec;
    auto abs = [&](const fs::path& p) { auto a = fs::absolute(p, ec); return a.lexically_normal().string(); };
    return {{"configPath", abs(config::DefaultPath())}, {"statePath", abs(ClawStatePath(root))}, {"cronStorePath", abs(root / "cron.json")}};
}
json ClawStateRead(const fs::path& root) {
    auto st = ReadJsonFile(ClawStatePath(root), json::object());
    for (const char* k : {"installs", "journals", "leases"}) if (!st.contains(k) || !st[k].is_object()) st[k] = json::object();
    return st;
}
json ClawStateIdentity(const fs::path& root) {
    const auto p = ClawStatePath(root);
    std::error_code ec;
    if (!fs::exists(p, ec)) { fs::create_directories(p.parent_path(), ec); std::ofstream f(p); f << ClawStateRead(root).dump(2); }
    struct stat sb{};
    if (::stat(p.c_str(), &sb) != 0) return nullptr;
    return {{"canonicalPath", fs::weakly_canonical(p, ec).string()}, {"key", "file:" + std::to_string(sb.st_dev) + ":" + std::to_string(sb.st_ino)},
            {"birthtime", std::string("0")}};
}

std::string GitHubApiBase(const json& raw) {
    std::string b;
    if (auto v = config::GetPath(raw, "gateway.github.apiBaseUrl"); v && v->is_string()) b = v->get<std::string>();
    if (b.empty()) if (const char* e = std::getenv("OPENCLAW_GITHUB_API_BASE_URL"); e && *e) b = e;
    if (b.empty()) return "https://api.github.com";
    std::smatch m;
    static const std::regex re("^(https?)://([^/@?#]+)(/api/v3)?/?$");
    if (!std::regex_match(b, m, re)) throw std::runtime_error("gateway.github.apiBaseUrl must be an HTTPS GitHub API base URL");
    const auto hostport = m[2].str();
    const bool loopback = hostport.rfind("127.0.0.1", 0) == 0 || hostport.rfind("localhost", 0) == 0 || hostport.rfind("[::1]", 0) == 0;
    if (m[1] == "http" && !loopback) throw std::runtime_error("gateway.github.apiBaseUrl must be an HTTPS GitHub API base URL");
    return m[1].str() + "://" + hostport + m[3].str();
}

fs::path SubsystemDataRoot(ServerDeps& deps) {
    if (deps.persist) return deps.persist->Root();
    const char* e = std::getenv("OPENCLAW_DATA_DIR");
    if (e && *e) return e;
    return deps.sessions.Root();
}

void SetRpcRequester(const std::string& principal, bool full_write) { t_principal = principal.empty() ? "operator" : principal; t_full_write = full_write; }
std::string CurrentRpcPrincipal() { return t_principal; }

void RecordSessionParticipant(const fs::path& root, const std::string& agent, const std::string& key) {
    if (key.empty()) return;
    std::lock_guard<std::mutex> l(g_mu);
    auto parts = ReadJsonFile(ParticipantsPath(root), json::object());
    auto& rec = parts[ParticipantKey(agent, key)][t_principal];
    if (!rec.is_object()) rec = {{"firstPromptedAt", NowMs()}, {"count", 0}};
    rec["count"] = rec.value("count", 0) + 1;
    WriteJsonFile(ParticipantsPath(root), parts);
}

namespace { ServerDeps* g_tool_deps = nullptr; fs::path g_tool_root; }
void SetSubsystemContext(ServerDeps* deps, const fs::path& root) { g_tool_deps = deps; g_tool_root = root; }

std::string AgentGitHubPublish(const json& args, const std::string& agent, const std::string& session_key) {
    if (!g_tool_deps) return json{{"ok", false}, {"error", "gateway not running"}}.dump();
    json p{{"sessionKey", session_key.empty() ? std::string("main") : session_key}, {"agentId", agent},
           {"idempotencyKey", args.value("idempotencyKey", "tool-" + RandomHex(8))}};
    if (args.contains("title")) p["title"] = args["title"];
    if (args.contains("body")) p["body"] = args["body"];
    // The agent is a restricted requester: no operator write authority for workflow definitions.
    const auto prev_p = t_principal; const bool prev_w = t_full_write;
    t_principal = "agent:" + agent; t_full_write = false;
    auto r = GitHubPublish(*g_tool_deps, g_tool_root, "sessions.github.publish", p);
    t_principal = prev_p; t_full_write = prev_w;
    return r.ok ? r.payload.dump() : json{{"ok", false}, {"error", r.error}}.dump();
}

ExtraResult TrySubsystemDispatch(ServerDeps& deps, const fs::path& root, const std::string& method, const json& params) {
    try {
        if (method.rfind("users.github.", 0) == 0) return UsersGitHub(deps, root, method, params);
        if (method == "projects.list" || method == "projects.register" || method == "projects.add" || method == "projects.remove") return Projects(deps, root, method, params);
        if (method == "sessions.companion.ask" || method == "sessions.companion.state" || method == "sessions.companion.reset")
            return Companion(deps, method, params);
        if (method == "sessions.dispatch" || method == "sessions.move" || method == "sessions.reclaim")
            return Placement(deps, root, method, params);
        if (method == "claws.removalJournal") return RemovalJournal(deps, root, params);
        if (method == "projects.searchRemote") return SearchRemote(deps, params);
        if (method == "sessions.github.publish" || method == "sessions.github.confirm") return GitHubPublish(deps, root, method, params);
    } catch (const std::exception& e) {
        return Fail(method + " failed: " + e.what());
    }
    return {};
}

}  // namespace openclaw::gateway

// ---------------------------------------------------------------- worker execution on node hosts
namespace openclaw::gateway {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;
}

std::optional<json> RunTurnOnPlacedWorker(ServerDeps& d, const fs::path& root, const std::string& agent, const std::string& key,
                                          const std::string& text, std::string* err) {
    json rec;
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto db = ReadJsonFile(PlacementsPath(root), json::object());
        const auto rk = agent + ":" + key;
        if (!db.contains(rk) || db[rk].value("state", "") != "active" || !db[rk].contains("deviceId")) return std::nullopt;
        rec = db[rk];
    }
    const auto device = rec["deviceId"].get<std::string>();
    auto& hub = NodeHub::Global();
    auto mark_failed = [&](const std::string& why) {
        std::lock_guard<std::mutex> l(g_mu);
        auto db = ReadJsonFile(PlacementsPath(root), json::object());
        const auto rk = agent + ":" + key;
        if (!db.contains(rk) || db[rk].value("generation", -1) != rec.value("generation", -2)) return;
        const auto now = NowMs();
        auto& cur = db[rk];
        cur["state"] = "failed"; cur["recoveryError"] = why; cur["recoveryAction"] = "restart";
        cur["terminalAtMs"] = now; cur["updatedAtMs"] = now; cur["stateChangedAtMs"] = now;
        cur.erase("runner");
        WriteJsonFile(PlacementsPath(root), db);
        if (d.emit) d.emit("sessions.changed", {{"reason", "worker-failed"}, {"sessionKey", key}});
    };
    if (!hub.Connected(device) || !hub.HasCommand(device, "worker.launch.v1")) {
        mark_failed("device worker is not connected");
        *err = "session is placed on device worker " + device + ", which is not connected; reclaim or recover it to the gateway";
        return json();
    }
    auto& store = SessionsOf(d, agent, key);
    auto history = store.Load(key);
    json hist = json::array();
    for (auto& m : history) if (m.role == "user" || m.role == "assistant") hist.push_back({{"role", m.role}, {"content", m.content}});
    const auto sid = SessionIdOf(store, key);
    json descriptor{{"version", 1}, {"agentId", agent}, {"sessionKey", key}, {"sessionId", sid}, {"message", text},
                    {"history", hist}, {"remoteWorkspaceDir", rec.value("remoteWorkspaceDir", "")}};
    const std::string plan_hash = ShaHex(descriptor.dump());
    const std::string launch = "launch-" + RandomHex(12), run = "run-" + RandomHex(12);
    json launch_req{{"environmentSession", 1}, {"sessionKey", key}, {"idleRetention", true}, {"launchId", launch},
                    {"gatewayNamespace", "default"}, {"expectedBundleHash", rec.value("workerBundleHash", WorkerBundleHash())},
                    {"descriptor", descriptor}, {"placementGeneration", rec.value("generation", 0)},
                    {"identity", {{"planHash", plan_hash}, {"environmentId", rec.value("environmentId", "")}, {"sessionId", sid},
                                  {"ownerEpoch", rec.value("activeOwnerEpoch", 0)}, {"runId", run}}}};
    auto r = hub.Invoke(device, "worker.launch.v1", launch_req, 30000);
    if (!r.value("ok", false)) {
        const auto msg = r["error"].value("message", std::string("launch failed"));
        if (r["error"].value("code", "") == "UNAVAILABLE") mark_failed(msg);
        *err = "worker launch on " + device + " failed: " + msg;
        return json();
    }
    json receipt = r["payload"];
    const auto deadline = NowMs() + 10 * 60 * 1000;
    while (receipt.value("state", "") == "pending" || receipt.value("state", "") == "running") {
        if (NowMs() > deadline) { (void)hub.Invoke(device, "worker.cancel.v1", {{"launchId", launch}}, 10000); *err = "worker run timed out"; return json(); }
        auto s = hub.Invoke(device, "worker.status.v1", {{"launchId", launch}, {"waitMs", 20000}}, 25000);
        if (!s.value("ok", false)) { mark_failed(s["error"].value("message", std::string("status failed"))); *err = "worker on " + device + " stopped answering"; return json(); }
        receipt = s["payload"];
    }
    if (receipt.value("launchId", "") != launch || receipt.value("placementGeneration", -1) != rec.value("generation", 0)) {
        *err = "worker returned a receipt for a different launch"; return json();
    }
    if (receipt.value("state", "") != "completed") { *err = "worker run " + receipt.value("state", std::string("failed")) + ": " + receipt.value("errorText", std::string()); return json(); }
    json result;
    try { result = json::parse(receipt.value("resultJson", std::string("{}"))); } catch (...) { *err = "worker result is not JSON"; return json(); }
    store.Append(key, agent::Message{.role = "user", .content = text});
    store.Append(key, agent::Message{.role = "assistant", .content = result.value("text", std::string())});
    if (d.emit) d.emit("session.message", {{"sessionKey", key}, {"role", "assistant"}, {"worker", device}});
    return json{{"sessionKey", key}, {"text", result.value("text", std::string())},
                {"worker", {{"deviceId", device}, {"launchId", launch}, {"runId", run}, {"placementGeneration", rec.value("generation", 0)}}}};
}

}  // namespace openclaw::gateway

// ---------------------------------------------------------------- upstream agent tools backed by gateway state
namespace openclaw::gateway {
std::string GatewayTool(const std::string& name, const nlohmann::json& a, const std::string& agent, const std::string& session_key) {
    using nlohmann::json;
    auto fail = [](const std::string& e) { return json{{"ok", false}, {"error", e}}.dump(); };
    if (!g_tool_deps) return fail("gateway not running");
    auto& d = *g_tool_deps;
    if (name == "sessions_list") {
        const int limit = std::clamp(a.value("limit", 50), 1, 200);
        json out = json::array();
        for (auto& k : d.sessions.List()) { if (static_cast<int>(out.size()) >= limit) break; out.push_back({{"key", k}, {"entry", d.sessions.IndexEntry(k)}}); }
        return json{{"sessions", out}}.dump();
    }
    if (name == "sessions_history") {
        const auto key = a.value("sessionKey", session_key);
        if (key.empty() || !d.sessions.Exists(key)) return fail("session not found: " + key);
        const int limit = std::clamp(a.value("limit", 20), 1, 200);
        const bool tools = a.value("includeTools", false);
        auto msgs = d.sessions.Load(key);
        json out = json::array();
        for (auto it = msgs.rbegin(); it != msgs.rend() && static_cast<int>(out.size()) < limit; ++it) {
            if (it->role == "system" || (!tools && it->role == "tool")) continue;
            out.insert(out.begin(), json{{"role", it->role}, {"content", it->content.substr(0, 4000)}});
        }
        return json{{"sessionKey", key}, {"messages", out}}.dump();
    }
    if (name == "session_status") {
        const auto key = a.value("sessionKey", session_key);
        if (key.empty() || !d.sessions.Exists(key)) return fail("session not found: " + key);
        json out{{"sessionKey", key}, {"entry", d.sessions.IndexEntry(key)}, {"messageCount", d.sessions.Load(key).size()}, {"agentId", agent}};
        auto db = ReadJsonFile(PlacementsPath(g_tool_root), json::object());
        if (db.contains(agent + ":" + key)) out["placement"] = PlacementView(db[agent + ":" + key]);
        return out.dump();
    }
    if (name == "agents_list") {
        json out = json::array();
        for (auto& ag : agent::routing::ListAgents(d.config.raw)) out.push_back({{"id", ag.id}, {"name", ag.name}, {"model", ag.model}, {"default", ag.is_default}});
        if (out.empty()) out.push_back({{"id", "main"}, {"default", true}});
        return json{{"agents", out}}.dump();
    }
    if (name == "nodes") {
        const auto action = a.value("action", std::string("list"));
        auto& hub = NodeHub::Global();
        if (action == "list") return json{{"nodes", hub.List()}}.dump();
        const auto nid = a.value("nodeId", std::string());
        if (action == "describe") { auto x = hub.Describe(nid); return x.is_null() ? fail("node not connected: " + nid) : x.dump(); }
        if (action == "invoke") {
            const auto cmd = a.value("command", std::string());
            if (cmd.rfind("worker.", 0) == 0) return fail("worker commands are gateway-owned");
            auto r = hub.Invoke(nid, cmd, a.value("params", json::object()), std::clamp(a.value("timeoutMs", 30000), 1, 120000));
            return r.dump();
        }
        return fail("action must be list, describe or invoke");
    }
    return fail("unknown gateway tool " + name);
}
}  // namespace openclaw::gateway
