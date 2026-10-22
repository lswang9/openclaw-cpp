#include "openclaw/channels/outbound.hpp"
#include "openclaw/channels/ext_channels.hpp"
#include "openclaw/channels/waweb/noise.hpp"
#include "openclaw/desktop/desktop.hpp"
#include "openclaw/clawhub/client.hpp"
#include "openclaw/net/safe_fetch.hpp"
#include <openssl/ecdsa.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <cstring>
#include <csignal>
#include <malloc.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <thread>
#include "openclaw/plugins/caps.hpp"
#include "openclaw/plugins/registry.hpp"
#include <optional>
#include "openclaw/mcp/client.hpp"
#include "openclaw/voice/tts.hpp"
#include "openclaw/voice/stt.hpp"
#include "openclaw/tokenizer/bpe.hpp"
#include "openclaw/auth/device_link.hpp"
#include "openclaw/auth/profiles.hpp"
#include "openclaw/tools/exec.hpp"
#include <atomic>
#include <unistd.h>
#include <ctime>
#include "openclaw/gateway/subsystems.hpp"
#include "openclaw/gateway/conversations.hpp"
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
#include "openclaw/persist/state_db.hpp"

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
    auto t = persist::StateRead(p); if (!t) return def;
    auto j = json::parse(*t, nullptr, false); return j.is_discarded() ? def : j;
}
void WriteJsonFile(const fs::path& p, const json& j) {
    persist::StateWrite(p, j.dump(2));
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


// ---------------------------------------------------------------- worktrees (git worktree, upstream worktrees.*)
fs::path WorktreesPath(const fs::path& root) { return root / "worktrees.json"; }
json PublicWt(const json& r) { json o = r; o.erase("snapshots"); return o; }
std::string SafeName(std::string s) { for (auto& c : s) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') c = '-'; return s.substr(0, 64); }
bool GitOk(const fs::path& repo, std::vector<std::string> args, std::string* out = nullptr, std::string* err = nullptr) {
    args.insert(args.begin(), {"git", "-C", repo.string()});
    auto r = Run(args, {});
    if (out) *out = Trim(r.out);
    if (err) *err = Trim(r.err);
    return r.code == 0;
}
ExtraResult Worktrees(ServerDeps&, const fs::path& root, const std::string& method, const json& p) {
    std::lock_guard<std::mutex> l(g_mu);
    auto db = ReadJsonFile(WorktreesPath(root), json::object());
    auto save = [&] { WriteJsonFile(WorktreesPath(root), db); };
    auto need = [&](const char* k) { return p.contains(k) && p[k].is_string() && !p[k].get<std::string>().empty(); };
    if (method == "worktrees.list") { json out = json::array(); for (auto& [k, v] : db.items()) out.push_back(PublicWt(v)); return Ok({{"worktrees", out}}); }
    if (method == "worktrees.branches") {
        if (!need("repoRoot")) return Fail("repoRoot is required");
        std::string out, cur;
        if (!GitOk(p["repoRoot"].get<std::string>(), {"for-each-ref", "--format=%(refname:short)\t%(objectname)", "refs/heads"}, &out)) return Fail("not a git repository");
        GitOk(p["repoRoot"].get<std::string>(), {"rev-parse", "--abbrev-ref", "HEAD"}, &cur);
        json br = json::array(); std::istringstream is(out); std::string line;
        while (std::getline(is, line)) { auto tab = line.find('\t'); if (tab == std::string::npos) continue; auto n = line.substr(0, tab);
            bool used = false; for (auto& [k, v] : db.items()) used = used || (v.value("branch", "") == n && v.value("status", "") == "active");
            br.push_back({{"name", n}, {"sha", line.substr(tab + 1)}, {"current", n == cur}, {"checkedOutInWorktree", used}}); }
        json res{{"branches", br}, {"currentBranch", cur}};
        if (p.value("includeRepositoryStatus", false)) { std::string st; GitOk(p["repoRoot"].get<std::string>(), {"status", "--porcelain"}, &st); res["repositoryStatus"] = {{"dirty", !st.empty()}}; }
        return Ok(res);
    }
    if (method == "worktrees.create") {
        if (!need("repoRoot") || !need("name")) return Fail("repoRoot and name are required");
        const fs::path repo = p["repoRoot"].get<std::string>();
        std::string top; if (!GitOk(repo, {"rev-parse", "--show-toplevel"}, &top)) return Fail("repoRoot is not a git repository");
        const auto name = SafeName(p["name"].get<std::string>());
        const auto id = "wt-" + name + "-" + std::to_string(NowMs() % 100000000);
        const fs::path path = root / "worktrees" / id;
        fs::create_directories(path.parent_path());
        const auto branch = "openclaw/" + name;
        std::string err;
        std::vector<std::string> args{"worktree", "add", "-b", branch, path.string()};
        if (need("baseRef")) args.push_back(p["baseRef"].get<std::string>());
        if (!GitOk(top, args, nullptr, &err)) return Fail("git worktree add failed: " + err);
        std::string base; GitOk(path, {"rev-parse", "HEAD"}, &base);
        db[id] = {{"id", id}, {"name", name}, {"repoRoot", top}, {"path", path.string()}, {"branch", branch}, {"baseRef", p.value("baseRef", std::string("HEAD"))},
                  {"baseSha", base}, {"ownerKind", "manual"}, {"status", "active"}, {"createdAt", NowMs()}, {"profiles", p.value("profiles", json::array())}};
        save();
        return Ok(PublicWt(db[id]));
    }
    const auto id = p.value("id", std::string());
    if (method == "worktrees.gc") {
        int pruned = 0;
        std::set<std::string> repos;
        for (auto& [k, v] : db.items()) repos.insert(v.value("repoRoot", ""));
        for (auto& r : repos) if (!r.empty()) GitOk(r, {"worktree", "prune"});
        for (auto& [k, v] : db.items()) if (v.value("status", "") == "active" && !fs::exists(v.value("path", ""))) { v["status"] = "missing"; ++pruned; }
        save();
        return Ok({{"jobId", p.value("jobId", "gc-" + std::to_string(NowMs()))}, {"status", "completed"}, {"pruned", pruned}});
    }
    if (id.empty() || !db.contains(id)) return Fail("unknown worktree: " + id);
    auto& rec = db[id];
    const fs::path path = rec.value("path", "");
    const std::string repo = rec.value("repoRoot", "");
    if (method == "worktrees.remove") {
        if (rec.value("status", "") != "active") return Fail("worktree is not active");
        std::string st, ahead;
        GitOk(path, {"status", "--porcelain"}, &st);
        GitOk(path, {"rev-list", "--count", rec.value("baseSha", "HEAD") + "..HEAD"}, &ahead);
        const bool lossless = st.empty();
        if (p.value("ifLossless", false) && !lossless) return Ok({{"removed", false}, {"reason", "worktree has uncommitted changes"}});
        json out{{"removed", true}};
        if (!lossless && !p.value("force", false)) {
            // Snapshot uncommitted work into refs/openclaw/worktree-snapshots/<id> before removing.
            std::string err;
            if (!GitOk(path, {"add", "-A"}, nullptr, &err) ||
                !GitOk(path, {"-c", "user.name=openclaw", "-c", "user.email=openclaw@localhost", "commit", "-q", "--no-verify", "-m", "openclaw worktree snapshot " + id}, nullptr, &err))
                return Ok({{"removed", false}, {"snapshotError", {{"message", err}}}});
            std::string sha; GitOk(path, {"rev-parse", "HEAD"}, &sha);
            const auto ref = "refs/openclaw/worktree-snapshots/" + id;
            GitOk(repo, {"update-ref", ref, sha});
            rec["snapshotRef"] = ref; out["snapshotRef"] = ref;
        }
        std::string err;
        rec["status"] = "removing"; save();
        if (!GitOk(repo, {"worktree", "remove", "--force", path.string()}, nullptr, &err)) return Fail("git worktree remove failed: " + err);
        rec["status"] = "removed"; rec["removedAt"] = NowMs(); save();
        return Ok(out);
    }
    if (method == "worktrees.restore") {
        if (rec.value("status", "") == "active") return Ok(PublicWt(rec));
        std::string err;
        const auto from = rec.contains("snapshotRef") ? rec["snapshotRef"].get<std::string>() : rec.value("branch", "");
        if (!GitOk(repo, {"worktree", "add", path.string(), rec.value("branch", "")}, nullptr, &err)) return Fail("git worktree add failed: " + err);
        if (rec.contains("snapshotRef")) { GitOk(path, {"reset", "--hard", from}); GitOk(path, {"reset", "--soft", "HEAD~1"}); GitOk(path, {"reset", "-q"}); }
        rec["status"] = "active"; rec["restoredAt"] = NowMs(); save();
        return Ok(PublicWt(rec));
    }
    if (method == "worktrees.recoverRemoval") {
        if (rec.value("status", "") != "removing") return Ok({{"recovered", false}, {"status", rec.value("status", "")}});
        GitOk(repo, {"worktree", "remove", "--force", path.string()}); GitOk(repo, {"worktree", "prune"});
        rec["status"] = "removed"; save();
        return Ok({{"recovered", true}, {"status", "removed"}});
    }
    if (method == "worktrees.retireSnapshot") {
        if (!rec.contains("snapshotRef")) return Ok({{"retired", false}, {"reason", "no snapshot"}});
        GitOk(repo, {"update-ref", "-d", rec["snapshotRef"].get<std::string>()});
        rec.erase("snapshotRef"); save();
        return Ok({{"retired", true}});
    }
    return {};
}

// ---------------------------------------------------------------- storage locations
ExtraResult Storage(ServerDeps& d, const fs::path& root, const std::string& method, const json& p) {
    auto locs = json::array();
    auto add = [&](const std::string& name, const fs::path& path, const std::string& kind) {
        std::error_code ec; auto sp = fs::space(path.empty() ? fs::path("/") : path, ec);
        locs.push_back({{"name", name}, {"kind", kind}, {"provider", "local"}, {"path", path.string()}, {"exists", fs::exists(path, ec)},
                        {"freeBytes", ec ? 0 : static_cast<long long>(sp.available)}, {"totalBytes", ec ? 0 : static_cast<long long>(sp.capacity)}});
    };
    add("data", root, "data"); add("sessions", d.sessions.Root(), "sessions"); add("workspace", WorkspaceOf(d, "main"), "workspace");
    if (method == "storage.locations.list") return Ok({{"locations", locs}});
    const auto name = p.value("name", std::string());
    for (auto& l : locs) if (l["name"] == name) {
        const fs::path probe = fs::path(l["path"].get<std::string>()) / (".openclaw-probe-" + std::to_string(NowMs()));
        std::error_code ec; fs::create_directories(probe.parent_path(), ec);
        bool w = static_cast<bool>(std::ofstream(probe) << "x"); fs::remove(probe, ec);
        return Ok({{"name", name}, {"ok", w}, {"writable", w}, {"location", l}});
    }
    return Fail("unknown storage location: " + name);
}

// ---------------------------------------------------------------- user profiles (users.*)
fs::path ProfilesPath(const fs::path& root) { return root / "user-profiles.json"; }
ExtraResult Users(ServerDeps&, const fs::path& root, const std::string& method, const json& p) {
    std::lock_guard<std::mutex> l(g_mu);
    auto db = ReadJsonFile(ProfilesPath(root), json{{"profiles", json::object()}});
    auto& pr = db["profiles"];
    if (!pr.contains("operator")) pr["operator"] = {{"id", "operator"}, {"displayName", "operator"}, {"role", "owner"}, {"emails", json::array()},
                                                   {"channelIdentities", json::array()}, {"authLinks", json::array()}, {"personalFiles", json::object()}};
    auto save = [&] { WriteJsonFile(ProfilesPath(root), db); };
    const auto pid = p.value("profileId", p.value("targetProfileId", t_principal.empty() ? std::string("operator") : t_principal));
    auto prof = [&](const std::string& id) -> json& {
        if (!pr.contains(id)) pr[id] = {{"id", id}, {"displayName", id}, {"role", "member"}, {"emails", json::array()}, {"channelIdentities", json::array()},
                                         {"authLinks", json::array()}, {"personalFiles", json::object()}};
        return pr[id];
    };
    auto admin = [&] { return t_full_write; };
    if (method == "users.mentionable") {
        json out = json::array(); for (auto& [k, v] : pr.items()) out.push_back({{"id", k}, {"displayName", v.value("displayName", k)}}); return Ok({{"users", out}});
    }
    if (method == "users.linkEmail") {
        auto email = Trim(p.value("email", std::string()));
        if (email.empty() || email.find('@') == std::string::npos) return Fail("email must not be empty");
        for (auto& [k, v] : pr.items()) for (auto& e : v["emails"]) if (e == email && k != pid) return Fail("email already linked to profile " + k);
        auto& P = prof(pid); bool have = false; for (auto& e : P["emails"]) have = have || e == email;
        if (!have) P["emails"].push_back(email);
        save(); return Ok({{"profile", P}});
    }
    if (method == "users.merge") {
        if (!admin()) return Fail("users.merge requires admin");
        const auto src = p.value("sourceProfileId", std::string()), dst = p.value("targetProfileId", std::string());
        if (src.empty() || dst.empty() || src == dst || !pr.contains(src) || !pr.contains(dst)) return Fail("sourceProfileId and targetProfileId must be distinct existing profiles");
        json moved = json::array();
        for (auto k : {"emails", "channelIdentities", "authLinks"}) {
            for (auto& x : pr[src][k]) { bool dup = false; for (auto& y : pr[dst][k]) dup = dup || y == x; if (!dup) pr[dst][k].push_back(x); }
            if (!pr[src][k].empty()) moved.push_back(k);
        }
        for (auto& [k, v] : pr[src]["personalFiles"].items()) if (!pr[dst]["personalFiles"].contains(k)) pr[dst]["personalFiles"][k] = v;
        pr.erase(src); save();
        return Ok({{"profile", pr[dst]}, {"movedAliasKinds", moved}});
    }
    if (method == "users.setRole") {
        if (!admin()) return Fail("users.setRole requires admin");
        const auto role = p.value("role", std::string());
        if (role != "owner" && role != "admin" && role != "member" && role != "viewer") return Fail("role must be owner, admin, member or viewer");
        prof(pid)["role"] = role; save(); return Ok({{"profile", pr[pid]}});
    }
    if (method == "users.setAvatar") {
        auto& P = prof(pid);
        if (p.contains("avatar") && p["avatar"].is_null()) P.erase("avatar"); else P["avatar"] = p.value("avatar", p.value("url", std::string()));
        save(); return Ok({{"profile", P}});
    }
    if (method == "users.linkChannelIdentity" || method == "users.unlinkChannelIdentity") {
        json idj{{"channel", p.value("channel", std::string())}, {"accountId", p.value("accountId", std::string())}, {"senderId", p.value("senderId", p.value("peerId", std::string()))}};
        if (idj["channel"] == "" || idj["senderId"] == "") return Fail("channel and senderId are required");
        auto& P = prof(pid); auto& arr = P["channelIdentities"];
        for (size_t i = 0; i < arr.size(); ++i) if (arr[i] == idj) { if (method[6] == 'u') arr.erase(arr.begin() + i); save(); return Ok({{"profile", P}}); }
        if (method == "users.linkChannelIdentity") {
            for (auto& [k, v] : pr.items()) for (auto& x : v["channelIdentities"]) if (x == idj && k != pid) return Fail("identity already linked to profile " + k);
            arr.push_back(idj);
        }
        save(); return Ok({{"profile", P}});
    }
    if (method == "users.listChannelIdentities") return Ok({{"identities", prof(pid)["channelIdentities"]}});
    if (method == "users.linkAuthProfile" || method == "users.unlinkAuthProfile") {
        const auto ap = p.value("authProfileId", p.value("authProfile", std::string()));
        if (ap.empty()) return Fail("authProfileId is required");
        auto& arr = prof(pid)["authLinks"];
        for (size_t i = 0; i < arr.size(); ++i) if (arr[i] == ap) { if (method == "users.unlinkAuthProfile") arr.erase(arr.begin() + i); save(); return Ok({{"profile", pr[pid]}}); }
        if (method == "users.linkAuthProfile") arr.push_back(ap);
        save(); return Ok({{"profile", pr[pid]}});
    }
    if (method == "users.listAuthLinks") return Ok({{"links", prof(pid)["authLinks"]}});
    if (method == "users.listModelAccounts") {
        json accts = json::array();
        for (auto& a : prof(pid)["authLinks"]) accts.push_back({{"id", a}, {"selected", prof(pid).value("modelAccount", std::string()) == a.get<std::string>()}});
        return Ok({{"accounts", accts}, {"selected", prof(pid).value("modelAccount", std::string())}});
    }
    if (method == "users.selectModelAccount") {
        const auto acc = p.value("accountId", std::string());
        bool ok = acc.empty(); for (auto& a : prof(pid)["authLinks"]) ok = ok || a == acc;
        if (!ok) return Fail("account is not linked to this profile");
        prof(pid)["modelAccount"] = acc; save(); return Ok({{"selected", acc}});
    }
    if (method == "users.personalFile.get" || method == "users.personalFile.set") {
        const auto name = p.value("name", p.value("file", std::string("USER.md")));
        if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) return Fail("invalid file name");
        auto& files = prof(pid)["personalFiles"];
        if (method == "users.personalFile.get") return Ok({{"name", name}, {"content", files.value(name, std::string())}, {"exists", files.contains(name)}});
        const auto c = p.value("content", std::string());
        if (c.size() > 256 * 1024) return Fail("content too large");
        files[name] = c; save(); return Ok({{"name", name}, {"saved", true}});
    }
    return {};
}

// ---------------------------------------------------------------- device rename, message actions
ExtraResult DeviceRename(const fs::path& root, const json& p) {
    std::lock_guard<std::mutex> l(g_mu);
    const auto id = p.value("deviceId", p.value("id", std::string()));
    const auto nm = Trim(p.value("displayName", p.value("name", std::string())));
    if (id.empty() || nm.empty() || nm.size() > 128) return Fail("deviceId and displayName (≤128) are required");
    auto db = ReadJsonFile(root / "device-names.json", json::object());
    db[id] = nm; WriteJsonFile(root / "device-names.json", db);
    return Ok({{"deviceId", id}, {"displayName", nm}});
}
bool ChannelCredPresent(const json& cfg) { return !channels::ChannelCred(cfg, "telegram", "botToken", "TELEGRAM_BOT_TOKEN").empty(); }
ExtraResult MessageAction(const fs::path& root, const json& p) {
    const auto channel = p.value("channel", std::string()), action = p.value("action", std::string());
    if (channel.empty() || action.empty()) return Fail("channel and action are required");
    static const std::set<std::string> known{"send", "react", "edit", "delete", "unsend", "pin", "unpin", "reply", "thread-reply", "read", "typing", "poll", "invoice", "sendInvoice", "topic_icon", "get_topic_icon", "create_topic", "createForumTopic", "edit_topic", "editForumTopic", "rename_topic", "close_topic", "closeForumTopic", "reopen_topic", "reopenForumTopic"};
    if (!known.count(action)) return Fail("unsupported message action: " + action);
    if (action == "send") { auto r = GatewayRpc("send", {{"channel", channel}, {"to", p.value("to", p.value("target", std::string()))}, {"message", p.value("message", p.value("params", json::object()).value("message", std::string()))}});
        return r.value("ok", false) ? Ok(r["payload"]) : Fail(r.value("error", json()).is_object() ? r["error"].value("message", "send failed") : "send failed"); }
    // 1.148: Signal reactions via signal-cli JSON-RPC sendReaction (or REST /v1/reactions).
    if (channel == "signal" && action == "react" && g_tool_deps) {
        json pp = p.value("params", json::object());
        for (auto k : {"to", "target", "emoji", "reaction", "remove", "messageId", "targetTimestamp", "targetAuthor", "targetAuthorUuid"})
            if (p.contains(k) && !pp.contains(k)) pp[k] = p[k];
        auto r = channels::ExtChannelReact("signal", g_tool_deps->config.raw, pp);
        return r.value("ok", false) ? Ok(r) : Fail(r.value("error", std::string("signal react failed")));
    }
    // 1.140: Telegram actions go to the Bot API when a bot token is configured.
    if (channel == "telegram" && g_tool_deps) {
        if (auto* oa = dynamic_cast<channels::OutboundRecordAdapter*>(g_tool_deps->adapters.Get("telegram"))) {
            json pp = p.value("params", json::object());
            for (auto k : {"to", "target", "messageId", "message_id", "emoji", "message", "buttons", "remove", "question", "options", "choices", "anonymous", "is_anonymous", "pollAnonymous", "pollPublic", "multiple", "allows_multiple_answers", "name", "topic", "title", "threadId", "message_thread_id", "icon_color", "iconCustomEmojiId", "icon_custom_emoji_id", "chatType", "description", "payload", "currency", "prices", "amount", "label", "provider_token", "business_connection_id"}) if (p.contains(k) && !pp.contains(k)) pp[k] = p[k];
            oa->SetConfig(g_tool_deps->config.raw);
            if (!ChannelCredPresent(g_tool_deps->config.raw)) {} else {
                auto r = oa->TelegramAction(action, pp);
                return r.value("ok", false) ? Ok(r) : Fail(r.value("error", std::string("telegram ") + r.value("method", action) + " failed (http " + std::to_string(r.value("http", 0)) + ")"));
            }
        }
    }
    // Non-send actions are recorded in the outbound action log (offline adapters replay them; live adapters are out of scope).
    std::lock_guard<std::mutex> l(g_mu);
    json rec{{"id", "act-" + std::to_string(NowMs())}, {"channel", channel}, {"action", action}, {"accountId", p.value("accountId", std::string())},
             {"params", p.value("params", json::object())}, {"at", NowMs()}, {"status", "recorded"}};
    std::ofstream(root / "message-actions.jsonl", std::ios::app) << rec.dump() << "\n";
    return Ok({{"ok", true}, {"result", rec}});
}


// ---------------------------------------------------------------- local-state groups: task suggestions, progress cards, cron scratch,
// node pending queue, session metadata (groups, involvement, viewers, owner, visibility, share), misc reads.
fs::path MetaPath(const fs::path& root, const char* n) { return root / (std::string(n) + ".json"); }
std::string Rid(const char* pfx) { static std::atomic<int> n{0}; return std::string(pfx) + "-" + std::to_string(NowMs()) + "-" + std::to_string(++n); }
ExtraResult LocalGroups(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto rpc_ok = [](const std::string& mm, const json& pp, json* out) { auto r = GatewayRpc(mm, pp); if (out) *out = r.value("payload", json()); return r.value("ok", false); };
    std::unique_lock<std::mutex> l(g_mu);
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    // ---- task suggestions
    if (m.rfind("taskSuggestions.", 0) == 0) {
        auto db = ReadJsonFile(MetaPath(root, "task-suggestions"), json::object());
        auto save = [&] { WriteJsonFile(MetaPath(root, "task-suggestions"), db); };
        if (m == "taskSuggestions.list") {
            json out = json::array();
            for (auto& [k, v] : db.items()) if ((str("sessionKey").empty() || v["sessionKey"] == str("sessionKey")) && (str("agentId").empty() || v.value("agentId", "") == str("agentId"))) out.push_back(v);
            return Ok({{"suggestions", out}});
        }
        if (m == "taskSuggestions.create") {
            for (auto k : {"title", "prompt", "tldr", "cwd", "sessionKey"}) if (str(k).empty()) return Fail(std::string(k) + " is required");
            if (str("title").size() > 60) return Fail("title must be ≤ 60 characters");
            const auto id = Rid("task");
            db[id] = {{"id", id}, {"title", str("title")}, {"prompt", str("prompt")}, {"tldr", str("tldr")}, {"cwd", str("cwd")}, {"sessionKey", str("sessionKey")}, {"createdAt", NowMs()}};
            if (!str("agentId").empty()) db[id]["agentId"] = str("agentId");
            save(); return Ok({{"taskId", id}, {"suggestion", db[id]}});
        }
        const auto id = str("taskId");
        if (!db.contains(id)) return Fail("unknown task suggestion: " + id);
        if (m == "taskSuggestions.dismiss") { db.erase(id); save(); return Ok({{"taskId", id}, {"resolution", "dismissed"}}); }
        if (m == "taskSuggestions.accept") {
            json s = db[id]; db.erase(id); save();  // claim atomically before starting work
            const auto mode = p.value("mode", std::string("session"));
            if (mode == "cloud") return Fail("cloud execution is not available in the C++ runtime");
            const auto agent = s.value("agentId", std::string("main"));
            const auto key = "agent:" + agent + ":task:" + id;
            l.unlock();
            if (mode == "worktree") {
                json wt; if (!rpc_ok("worktrees.create", {{"repoRoot", p.value("cwd", s["cwd"].get<std::string>())}, {"name", id}}, &wt)) return Fail("worktree creation failed");
            }
            json out; if (!rpc_ok("chat.send", {{"sessionKey", key}, {"message", s["prompt"]}, {"agentId", agent}}, &out)) return Fail("starting the task failed");
            return Ok({{"taskId", id}, {"key", key}});
        }
    }
    // ---- progress cards
    if (m.rfind("progressCard.", 0) == 0) {
        auto db = ReadJsonFile(MetaPath(root, "progress-cards"), json::object());
        const auto key = str("agentId").empty() ? str("sessionKey") : str("agentId") + "|" + str("sessionKey");
        if (str("sessionKey").empty()) return Fail("sessionKey is required");
        if (m == "progressCard.get" || m == "progressCard.refresh") return Ok({{"card", db.contains(key) ? db[key] : json(nullptr)}});
        const int rev = db.contains(key) ? db[key].value("revision", 0) : 0;
        if (p.contains("expectedRevision") && p["expectedRevision"].get<int>() != rev) return Fail("revision conflict: current revision is " + std::to_string(rev));
        json card{{"sessionKey", str("sessionKey")}, {"revision", rev + 1}, {"updatedAt", NowMs()}};
        if (p.contains("markdown")) { if (str("markdown").size() > 8192) return Fail("markdown exceeds 8192 bytes"); card["markdown"] = p["markdown"]; }
        if (p.contains("plan")) { if (p["plan"].size() > 50) return Fail("plan exceeds 50 steps"); card["steps"] = p["plan"]; }
        db[key] = card; WriteJsonFile(MetaPath(root, "progress-cards"), db);
        return Ok({{"card", card}});
    }
    // ---- cron scratch (one shared scratchpad with optimistic revisions)
    if (m == "cron.scratch.get" || m == "cron.scratch.set") {
        auto db = ReadJsonFile(MetaPath(root, "cron-scratch"), json{{"content", ""}, {"revision", 0}});
        if (m == "cron.scratch.get") return Ok({{"scratch", db["content"]}, {"currentRevision", db["revision"]}});
        if (p.contains("expectedRevision") && p["expectedRevision"] != db["revision"]) return Fail("revision conflict: current revision is " + db["revision"].dump());
        if (str("content").size() > 64 * 1024) return Fail("content too large");
        db = {{"content", str("content")}, {"revision", db["revision"].get<int>() + 1}, {"updatedAt", NowMs()}};
        WriteJsonFile(MetaPath(root, "cron-scratch"), db);
        return Ok({{"currentRevision", db["revision"]}});
    }
    // ---- node pending work queue
    if (m.rfind("node.pending.", 0) == 0) {
        auto db = ReadJsonFile(MetaPath(root, "node-pending"), json::object());
        auto save = [&] { WriteJsonFile(MetaPath(root, "node-pending"), db); };
        const auto nid = str("nodeId").empty() ? NodeHub::Global().NodeOf(GatewayConnId()) : str("nodeId");
        if (nid.empty()) return Fail("nodeId is required");
        auto& q = db[nid]; if (!q.is_array()) q = json::array();
        const auto now = NowMs();
        json live = json::array(); for (auto& it : q) if (it.value("expiresAt", 0LL) == 0 || it["expiresAt"].get<long long>() > now) live.push_back(it); q = live;
        if (m == "node.pending.enqueue") {
            if (str("type").empty()) return Fail("type is required");
            json it{{"id", Rid("pend")}, {"type", str("type")}, {"priority", p.value("priority", 0)}, {"payload", p.value("payload", json::object())}, {"createdAt", now}};
            if (p.contains("expiresInMs")) it["expiresAt"] = now + p["expiresInMs"].get<long long>();
            q.push_back(it); save();
            if (p.value("wake", false)) NodeHub::Global().Notify(nid, "node.pending.wake", {{"nodeId", nid}});
            return Ok({{"item", it}, {"queued", q.size()}});
        }
        if (m == "node.pending.pull" || m == "node.pending.drain") {
            std::vector<json> v(q.begin(), q.end());
            std::stable_sort(v.begin(), v.end(), [](const json& a, const json& b) { return a.value("priority", 0) > b.value("priority", 0); });
            const size_t mx = std::clamp(p.value("maxItems", 50), 1, 500);
            json out = json::array(); for (size_t i = 0; i < v.size() && i < mx; ++i) out.push_back(v[i]);
            if (m == "node.pending.drain") { json rest = json::array(); for (size_t i = mx; i < v.size(); ++i) rest.push_back(v[i]); q = rest; }
            save(); return Ok({{"items", out}});
        }
        if (m == "node.pending.ack") {
            std::set<std::string> ids; for (auto& i : p.value("ids", json::array())) if (i.is_string()) ids.insert(i.get<std::string>());
            json rest = json::array(); int acked = 0; for (auto& it : q) { if (ids.count(it["id"].get<std::string>())) ++acked; else rest.push_back(it); }
            q = rest; save(); return Ok({{"acked", acked}});
        }
    }
    // ---- node capability updates from a connected node
    if (m == "node.skills.update" || m == "node.pluginTools.update" || m == "node.runnerInventory.update" || m == "node.pluginSurface.refresh" || m == "plugin.surface.refresh") {
        const auto nid = str("nodeId").empty() ? NodeHub::Global().NodeOf(GatewayConnId()) : str("nodeId");
        if (nid.empty() && m.rfind("node.", 0) == 0) return Fail("caller is not a connected node");
        auto db = ReadJsonFile(MetaPath(root, "node-capabilities"), json::object());
        const auto field = m.substr(m.find('.') + 1, m.rfind('.') - m.find('.') - 1);
        db[nid.empty() ? "gateway" : nid][field] = p; db[nid.empty() ? "gateway" : nid]["updatedAt"] = NowMs();
        WriteJsonFile(MetaPath(root, "node-capabilities"), db);
        return Ok({{"ok", true}, {"nodeId", nid}, {"updated", field}});
    }
    if (m == "node.invoke.progress") {
        if (str("invokeId").empty()) return Fail("invokeId is required");
        l.unlock();
        GatewayRpc("node.event", {{"event", "node.invoke.progress"}, {"payload", p}});
        return Ok({{"ok", true}});
    }
    // ---- session metadata
    auto meta = ReadJsonFile(MetaPath(root, "session-meta"), json{{"sessions", json::object()}, {"groups", json::object()}, {"defaults", json::object()}, {"observerVisible", true}});
    auto save_meta = [&] { WriteJsonFile(MetaPath(root, "session-meta"), meta); };
    auto skey = [&] { auto k = str("key").empty() ? str("sessionKey") : str("key"); return k; };
    if (m == "sessions.groups.rename") {
        if (str("name").empty() || str("to").empty()) return Fail("name and to are required");
        auto& g = meta["groups"]; g[str("to")] = g.contains(str("name")) ? g[str("name")] : json::object(); g.erase(str("name"));
        for (auto& [k, v] : meta["sessions"].items()) if (v.value("group", "") == str("name")) v["group"] = str("to");
        save_meta(); return Ok({{"name", str("to")}, {"group", g[str("to")]}});
    }
    if (m == "sessions.groups.update") {
        if (str("name").empty()) return Fail("name is required");
        auto& g = meta["groups"][str("name")]; if (!g.is_object()) g = json::object();
        for (auto k : {"cwd", "worktree"}) if (p.contains(k)) g[k] = p[k];
        save_meta(); return Ok({{"name", str("name")}, {"group", g}});
    }
    if (m == "sessions.groups.defaults") { if (p.contains("defaults")) { meta["defaults"] = p["defaults"]; save_meta(); } return Ok({{"defaults", meta["defaults"]}}); }
    if (m == "sessions.observer.visibility") { if (p.contains("visible")) { meta["observerVisible"] = p.value("visible", true); save_meta(); } return Ok({{"visible", meta["observerVisible"]}}); }
    if (m == "sessions.setInvolvement" || m == "sessions.viewers.set" || m == "sessions.assignOwner" || m == "session.visibility.set" || m == "session.publicShare.set") {
        const auto k = skey();
        if (k.empty()) return Fail("key is required");
        auto& s = meta["sessions"][k]; if (!s.is_object()) s = json::object();
        if (m == "sessions.setInvolvement") s["hidden"] = p.value("hidden", false);
        else if (m == "sessions.viewers.set") s["viewers"] = p.value("viewers", json::array());
        else if (m == "sessions.assignOwner") s["owner"] = p.value("owner", json(nullptr));
        else if (m == "session.visibility.set") { const auto v = str("visibility"); if (v != "private" && v != "team" && v != "public") return Fail("visibility must be private, team or public"); s["visibility"] = v; }
        else { s["publicShare"] = p.value("enabled", false); if (s["publicShare"]) s["shareId"] = s.value("shareId", Rid("share")); else s.erase("shareId"); }
        save_meta(); json o = s; o["key"] = k; return Ok(o);
    }
    if (m == "sessions.patchMany") {
        auto targets = p.value("targets", json::array());
        if (targets.empty() || targets.size() > 200) return Fail("targets must have 1..200 entries");
        l.unlock();
        json res = json::array();
        for (auto& tg : targets) {
            json pp = p.value("patch", json::object()); pp["key"] = tg.is_string() ? tg : tg.value("key", json(""));
            auto r = GatewayRpc("sessions.patch", pp); res.push_back({{"key", pp["key"]}, {"ok", r.value("ok", false)}});
        }
        return Ok({{"results", res}});
    }
    l.unlock();
    // ---- reads over existing state
    if (m == "chat.message.get") {
        const auto k = str("sessionKey");
        auto& store = d.agent_pool ? d.agent_pool->Sessions(str("agentId").empty() ? std::string("main") : str("agentId")) : d.sessions;
        if (k.empty() || !store.Exists(k)) return Fail("session not found");
        auto msgs = store.Load(k);
        size_t idx = 0; const auto mid = p.contains("messageId") ? (p["messageId"].is_number() ? p["messageId"].dump() : p["messageId"].get<std::string>()) : std::string();
        try { idx = std::stoul(mid); } catch (...) { return Fail("messageId must be a message index"); }
        if (idx >= msgs.size()) return Fail("message not found");
        auto j = msgs[idx].ToJson(); const size_t mx = std::clamp(p.value("maxChars", 20000), 1, 200000);
        if (j.contains("content") && j["content"].is_string() && j["content"].get<std::string>().size() > mx) { j["content"] = j["content"].get<std::string>().substr(0, mx); j["truncated"] = true; }
        return Ok({{"message", j}, {"messageId", std::to_string(idx)}});
    }
    if (m == "chat.toolTitles") {
        json titles = json::object();
        for (auto& n : d.tools.Names()) { std::string t2 = n; std::replace(t2.begin(), t2.end(), '_', ' '); if (!t2.empty()) t2[0] = static_cast<char>(std::toupper(t2[0])); titles[n] = t2; }
        return Ok({{"titles", titles}});
    }
    if (m == "mentions.list") {
        json ul; GatewayRpc("users.mentionable", json::object()).value("payload", json::object()).swap(ul);
        json out = json::array();
        for (auto& u : ul.value("users", json::array())) out.push_back({{"kind", "user"}, {"id", u["id"]}, {"label", u["displayName"]}});
        for (auto& a : agent::routing::ListAgents(d.config.raw)) out.push_back({{"kind", "agent"}, {"id", a.id}, {"label", a.name}});
        return Ok({{"mentions", out}});
    }
    if (m == "sessions.diff") {
        const auto ws = WorkspaceOf(d, str("agentId").empty() ? std::string("main") : str("agentId"));
        std::vector<std::string> args{"git", "-C", ws.string(), "diff"};
        if (!str("commit").empty()) { args.push_back(str("commit") + "^!"); } else if (p.value("scope", std::string()) == "staged") args.push_back("--cached");
        auto r = Run(args, {});
        if (r.code != 0) return Fail("workspace is not a git repository");
        auto st = Run({"git", "-C", ws.string(), "diff", "--stat"}, {});
        return Ok({{"diff", r.out.substr(0, 2000000)}, {"stat", Trim(st.out)}, {"truncated", r.out.size() > 2000000}});
    }
    if (m == "transcripts.export") {
        const auto k = p.value("selector", json::object()).value("sessionKey", str("sessionKey"));
        if (k.empty() || !d.sessions.Exists(k)) return Fail("session not found");
        const auto fmt = p.value("format", std::string("markdown"));
        auto msgs = d.sessions.Load(k);
        if (fmt == "json") { json a = json::array(); for (auto& x : msgs) a.push_back(x.ToJson()); return Ok({{"format", "json"}, {"content", a.dump(2)}}); }
        std::string md = "# Transcript " + k + "\n\n";
        for (auto& x : msgs) if (x.role != "system") md += "**" + x.role + "**: " + x.content + "\n\n";
        return Ok({{"format", "markdown"}, {"content", md}});
    }
    if (m == "gateway.identity.get") {
        auto idf = root / "gateway-identity.json";
        auto j = ReadJsonFile(idf, json::object());
        if (!j.contains("gatewayId")) { std::lock_guard<std::mutex> l2(g_mu); j = {{"gatewayId", Rid("gw")}, {"createdAt", NowMs()}}; WriteJsonFile(idf, j); }
        char host[256] = {0}; gethostname(host, sizeof host - 1);
        j["hostname"] = host; j["runtime"] = "openclaw-cpp";
        return Ok(j);
    }
    if (m == "wake") {
        const auto mode = p.value("mode", std::string("now"));
        if (mode != "now" && mode != "next-heartbeat") return Fail("mode must be now or next-heartbeat");
        if (mode == "now" && !str("text").empty()) {
            json out; auto r = GatewayRpc("chat.send", {{"sessionKey", str("sessionKey").empty() ? std::string("agent:main:main") : str("sessionKey")}, {"message", str("text")}, {"agentId", str("agentId").empty() ? std::string("main") : str("agentId")}});
            return r.value("ok", false) ? Ok({{"ok", true}, {"mode", mode}, {"ran", true}}) : Fail("wake turn failed");
        }
        std::lock_guard<std::mutex> l2(g_mu);
        auto q = ReadJsonFile(MetaPath(root, "wake-queue"), json::array()); q.push_back({{"text", str("text")}, {"at", NowMs()}, {"mode", mode}});
        WriteJsonFile(MetaPath(root, "wake-queue"), q);
        return Ok({{"ok", true}, {"mode", mode}, {"queued", true}});
    }
    return {};
}


// ---------------------------------------------------------------- terminals (PTY via exec background sessions), session processes,
// model auth profiles, usage, web search, device tokens, local environments, session storage/catalog helpers.
json ExecProc(const std::string& agent, const json& a) {
    tools::exec::ScopedRunContext rc(tools::exec::RunContext{agent, "", "", "", ""});
    return tools::exec::ProcessTool(a);
}
ExtraResult Batch3(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    // ---- terminals
    if (m.rfind("terminal.", 0) == 0) {
        static std::mutex tmu;
        std::lock_guard<std::mutex> tl(tmu);
        auto db = ReadJsonFile(MetaPath(root, "terminals"), json::object());
        auto save = [&] { WriteJsonFile(MetaPath(root, "terminals"), db); };
        if (!t_full_write) return Fail("terminal access requires operator write scope");
        if (m == "terminal.open") {
            const auto agent = str("agentId").empty() ? std::string("main") : str("agentId");
            const auto cwd = str("cwd").empty() ? WorkspaceOf(d, agent).string() : str("cwd");
            const auto shell = str("shell").empty() ? std::string(std::getenv("SHELL") ? std::getenv("SHELL") : "/bin/bash") : str("shell");
            std::error_code ec; if (!fs::is_directory(cwd, ec)) return Fail("cwd does not exist: " + cwd);
            tools::exec::ScopedRunContext rc(tools::exec::RunContext{"terminal", "", "", "", ""});
            json cfg = d.config.raw; cfg["tools"]["exec"]["security"] = "full"; cfg["tools"]["exec"]["ask"] = "off";
            auto r = tools::exec::Run({{"command", shell + " -i"}, {"pty", true}, {"background", true}, {"workdir", cwd}, {"security", "full"}, {"ask", "off"}, {"timeout", 86400}}, cfg, cwd);
            if (!r.contains("sessionId")) return Fail("terminal start failed: " + r.value("error", r.dump()));
            const auto sid = r["sessionId"].get<std::string>();
            const int cols = std::clamp(p.value("cols", 80), 10, 500), rows = std::clamp(p.value("rows", 24), 5, 200);
            ExecProc("terminal", {{"action", "resize"}, {"sessionId", sid}, {"rows", rows}, {"cols", cols}});
            db[sid] = {{"sessionId", sid}, {"agentId", agent}, {"shell", shell}, {"cwd", cwd}, {"confined", false}, {"attached", true}, {"owner", "conn"},
                       {"createdAtMs", NowMs()}, {"cols", cols}, {"rows", rows}};
            if (!str("title").empty()) db[sid]["title"] = str("title");
            if (!str("sessionKey").empty()) db[sid]["sessionKey"] = str("sessionKey");
            save(); return Ok(db[sid]);
        }
        if (m == "terminal.list") {
            json out = json::array(); auto live = ExecProc("terminal", {{"action", "list"}})["sessions"];
            for (auto& [k, v] : db.items()) { bool running = false; for (auto& s : live) running = running || (s["sessionId"] == k && s["status"] == "running"); json x = v; x["running"] = running; out.push_back(x); }
            return Ok({{"sessions", out}});
        }
        const auto sid = str("sessionId");
        if (!db.contains(sid)) return Fail("unknown terminal session: " + sid);
        if (m == "terminal.attach") { auto r = ExecProc("terminal", {{"action", "log"}, {"sessionId", sid}}); db[sid]["attached"] = true; save();
            json x = db[sid]; x["buffer"] = r.value("output", std::string()); return Ok(x); }
        if (m == "terminal.input") { auto r = ExecProc("terminal", {{"action", "write"}, {"sessionId", sid}, {"data", str("data")}}); return r.value("ok", true) ? Ok({{"ok", true}}) : Fail(r.value("error", "write failed")); }
        if (m == "terminal.resize") { const int c = std::clamp(p.value("cols", 80), 10, 500), r = std::clamp(p.value("rows", 24), 5, 200);
            ExecProc("terminal", {{"action", "resize"}, {"sessionId", sid}, {"rows", r}, {"cols", c}}); db[sid]["cols"] = c; db[sid]["rows"] = r; save(); return Ok({{"ok", true}, {"cols", c}, {"rows", r}}); }
        if (m == "terminal.close") { ExecProc("terminal", {{"action", "kill"}, {"sessionId", sid}}); db.erase(sid); save(); return Ok({{"ok", true}, {"closed", true}}); }
        if (m == "terminal.upload") {
            const auto name = str("name"), src = str("path");
            if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) return Fail("invalid upload name");
            std::error_code ec; if (!fs::is_regular_file(src, ec)) return Fail("upload source not found");
            const auto dst = fs::path(db[sid]["cwd"].get<std::string>()) / name;
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec) return Fail("upload failed: " + ec.message());
            return Ok({{"ok", true}, {"path", dst.string()}, {"size", fs::file_size(dst, ec)}});
        }
    }
    // ---- session processes (agent exec background sessions)
    if (m == "sessions.processes.list") {
        const auto agent = str("agentId").empty() ? std::string("main") : str("agentId");
        return Ok({{"processes", ExecProc(agent, {{"action", "list"}})["sessions"]}});
    }
    if (m == "sessions.processes.stop") {
        const auto agent = str("agentId").empty() ? std::string("main") : str("agentId");
        auto r = ExecProc(agent, {{"action", "kill"}, {"sessionId", str("processId").empty() ? str("sessionId") : str("processId")}});
        return r.value("ok", false) ? Ok({{"stopped", true}}) : Fail(r.value("error", "stop failed"));
    }
    // ---- model auth profiles
    if (m.rfind("models.auth", 0) == 0 && m != "models.authLogin") {
        const auto agent = str("agentId").empty() ? std::string("main") : str("agentId");
        auth::ProfileStore store(auth::DefaultProfilesPath(agent));
        if (!t_full_write && m != "models.authStatus") return Fail(m + " requires operator write scope");
        if (m == "models.authStatus") {
            auto prof = store.ListMasked();
            auto order = ReadJsonFile(root / "auth-order.json", json::object());
            json providers = json::object();
            json profs = prof.is_object() && prof.contains("profiles") && prof["profiles"].is_object() ? prof["profiles"] : prof.is_object() && !prof.contains("profiles") ? prof : json::object();
            for (auto& [id, c] : profs.items()) {
                if (!c.is_object()) continue;
                const auto pv = c.value("provider", id.substr(0, id.find(':')));
                auto& e = providers[pv]; if (!e.is_object()) e = {{"profiles", json::array()}};
                e["profiles"].push_back({{"id", id}, {"type", c.value("type", "")}, {"expired", c.value("type", "") == "oauth" && auth::OAuthNeedsRefresh(c, 0)}});
            }
            for (auto& [pv, e] : providers.items()) if (order.contains(pv)) e["order"] = order[pv];
            return Ok({{"agentId", agent}, {"providers", providers}});
        }
        if (m == "models.authSetApiKey") {
            const auto pv = str("provider"), key = str("apiKey");
            if (pv.empty() || key.empty()) return Fail("provider and apiKey are required");
            if (!store.Upsert(pv + ":default", {{"type", "api_key"}, {"provider", pv}, {"key", key}})) return Fail("could not write auth profiles");
            return Ok({{"profileId", pv + ":default"}, {"saved", true}});
        }
        if (m == "models.authLogout") {
            const auto pv = str("provider");
            json removed = json::array();
            auto ids = p.value("profileIds", json::array());
            json all_p = store.Load().value("profiles", json::object());
            if (ids.empty()) for (auto& [id, c] : all_p.items()) if (id.rfind(pv + ":", 0) == 0) ids.push_back(id);
            for (auto& id : ids) if (id.is_string() && store.Remove(id.get<std::string>())) removed.push_back(id);
            return Ok({{"removed", removed}});
        }
        if (m == "models.authOrderSet") {
            std::lock_guard<std::mutex> l(g_mu);
            auto order = ReadJsonFile(root / "auth-order.json", json::object());
            if (str("provider").empty()) return Fail("provider is required");
            order[str("provider")] = p.value("profileIds", json::array());
            WriteJsonFile(root / "auth-order.json", order);
            return Ok({{"provider", str("provider")}, {"order", order[str("provider")]}});
        }
        if (m == "models.authRefresh") {
            json res = json::array();
            json all_p = store.Load().value("profiles", json::object());
            for (auto& [id, c] : all_p.items())
                if (c.is_object() && c.value("type", "") == "oauth") res.push_back({{"id", id}, {"needsRefresh", auth::OAuthNeedsRefresh(c)}, {"refreshed", false},
                                                                   {"note", "OAuth refresh runs on first provider use"}});
            return Ok({{"profiles", res}});
        }
    }
    // ---- usage (from session transcripts, counted with the cl100k tokenizer; priced from models.providers.*.models[].cost per 1M tokens)
    if (m == "usage.status" || m == "usage.cost" || m == "sessions.usage.logs" || m == "sessions.usage.timeseries") {
        auto& store = d.sessions;
        auto price_of = [&](const std::string& model) -> json {
            auto prov = config::GetPath(d.config.raw, "models.providers");
            if (!prov || !prov->is_object() || model.empty()) return nullptr;
            const auto slash = model.find('/');
            const std::string pv = slash == std::string::npos ? "" : model.substr(0, slash), mid = slash == std::string::npos ? model : model.substr(slash + 1);
            for (auto& [pk, pvv] : prov->items()) {
                if (!pv.empty() && pk != pv) continue;
                if (!pvv.is_object() || !pvv.contains("models") || !pvv["models"].is_array()) continue;
                for (auto& mm : pvv["models"]) if (mm.is_object() && mm.value("id", "") == mid && mm.contains("cost")) return mm["cost"];
            }
            return nullptr;
        };
        const std::string def_model = [&] { auto v = config::GetPath(d.config.raw, "agents.defaults.model"); if (v && v->is_string()) return v->get<std::string>(); if (v && v->is_object()) return v->value("primary", std::string()); return d.config.deepseek_model; }();
        long long tin = 0, tout = 0, msgs = 0; double cost = 0; bool priced_all = true; json per = json::array();
        std::map<long long, long long> buckets;
        for (auto& k : store.List()) {
            json e = store.IndexEntry(k); if (!e.is_object()) e = json::object(); auto ms = store.Load(k);
            long long in = 0, out = 0;
            for (auto& x : ms) { const auto n = static_cast<long long>(tokenizer::CountTokens(x.content)); (x.role == "assistant" ? out : in) += n; }
            msgs += static_cast<long long>(ms.size()); tin += in; tout += out;
            const auto model = e.value("model", def_model);
            auto pr = price_of(model);
            json c = nullptr;
            if (pr.is_object()) { const double cc = (in * pr.value("input", 0.0) + out * pr.value("output", 0.0)) / 1e6; c = cc; cost += cc; } else if (in + out > 0) priced_all = false;
            if (!str("sessionKey").empty() && k != str("sessionKey")) continue;
            per.push_back({{"sessionKey", k}, {"messages", ms.size()}, {"inputTokens", in}, {"outputTokens", out}, {"model", model}, {"costUsd", c}, {"updatedAt", e.value("updatedAt", 0LL)}});
            buckets[e.value("updatedAt", 0LL) / 3600000 * 3600000] += in + out;
        }
        const std::string backend = tokenizer::Backend();
        if (m == "usage.status") return Ok({{"sessions", store.List().size()}, {"messages", msgs}, {"inputTokens", tin}, {"outputTokens", tout}, {"tokenizer", backend}, {"source", "transcripts"}});
        if (m == "usage.cost") return Ok({{"inputTokens", tin}, {"outputTokens", tout}, {"costUsd", priced_all ? json(cost) : (cost > 0 ? json(cost) : json(nullptr))}, {"complete", priced_all},
                                         {"tokenizer", backend}, {"note", priced_all ? "priced from models.providers.*.models[].cost (USD per 1M tokens)" : "some sessions use models without configured cost"}});
        if (m == "sessions.usage.logs") return Ok({{"entries", per}, {"tokenizer", backend}});
        json ts = json::array(); for (auto& [bk, v] : buckets) ts.push_back({{"t", bk}, {"tokens", v}});
        return Ok({{"bucketMs", 3600000}, {"points", ts}, {"tokenizer", backend}});
    }
    // ---- web search
    if (m == "webSearch.status" || m == "webSearch.test") {
        const char* brave = std::getenv("BRAVE_API_KEY"); const char* tav = std::getenv("TAVILY_API_KEY");
        auto cfgp = config::GetPath(d.config.raw, "tools.web.search");
        json st{{"configured", brave || tav || (cfgp && cfgp->is_object() && cfgp->contains("apiKey"))},
                {"providers", json::array({{{"id", "brave"}, {"available", brave != nullptr}}, {{"id", "tavily"}, {"available", tav != nullptr}}})}};
        if (m == "webSearch.status") return Ok(st);
        if (!st["configured"].get<bool>()) return Fail("no web search provider configured (set BRAVE_API_KEY or TAVILY_API_KEY)");
        return Fail("live web search test needs network access to the provider");
    }
    // ---- device tokens (issued by pairing links)
    if (m == "device.token.revoke") {
        if (!d.device_links) return Fail("device links are not enabled");
        const auto id = str("deviceId");
        if (id.empty()) return Fail("deviceId is required");
        auto r = d.device_links->Revoke(id);
        return r.value("ok", false) ? Ok(r) : Fail(r.value("error", std::string("unknown device")));
    }
    // ---- local environments (directory-scoped exec)
    if (m.rfind("environments.", 0) == 0) {
        std::lock_guard<std::mutex> l(g_mu);
        auto db = ReadJsonFile(MetaPath(root, "environments"), json::object());
        auto save = [&] { WriteJsonFile(MetaPath(root, "environments"), db); };
        if (m == "environments.create" || m == "environments.session.create") {
            const auto idem = str("idempotencyKey");
            for (auto& [k, v] : db.items()) if (!idem.empty() && v.value("idempotencyKey", "") == idem) return Ok(v);
            const auto id = Rid("env"); const auto dir = root / "environments" / id; fs::create_directories(dir);
            db[id] = {{"environmentId", id}, {"id", id}, {"profileId", str("profileId").empty() ? std::string("local") : str("profileId")}, {"status", "ready"},
                      {"provider", "local"}, {"path", dir.string()}, {"createdAt", NowMs()}, {"idempotencyKey", idem}};
            if (m == "environments.session.create") db[id]["kind"] = "session";
            save(); return Ok(db[id]);
        }
        const auto id = str("environmentId").empty() ? str("id") : str("environmentId");
        if (!db.contains(id)) return Fail("unknown environment: " + id);
        if (m == "environments.status" || m == "environments.session.status") return Ok(db[id]);
        if (m == "environments.prepare") { db[id]["status"] = "ready"; db[id]["preparedAt"] = NowMs(); save(); return Ok(db[id]); }
        if (m == "environments.destroy" || m == "environments.session.destroy") {
            std::error_code ec; fs::remove_all(db[id]["path"].get<std::string>(), ec); db.erase(id); save(); return Ok({{"destroyed", true}, {"environmentId", id}});
        }
        if (m == "environments.session.exec") {
            if (!t_full_write) return Fail("exec requires operator write scope");
            auto argv = p.value("argv", json::array());
            if (!argv.is_array() || argv.empty()) return Fail("argv is required");
            std::vector<std::string> av; for (auto& x : argv) if (x.is_string()) av.push_back(x.get<std::string>());
            auto r = Run(av, db[id]["path"].get<std::string>(), {}, str("input"));
            return Ok({{"exitCode", r.code}, {"stdout", r.out.substr(0, 1000000)}, {"stderr", r.err.substr(0, 200000)}});
        }
    }
    // ---- session helpers
    if (m == "sessions.title.prepare") {
        auto msg = Trim(str("message"));
        if (msg.empty()) return Fail("message is required");
        std::string title; for (char c : msg) { if (c == '\n') break; title.push_back(c); }
        if (title.size() > 60) { title = title.substr(0, 57); auto sp = title.rfind(' '); if (sp > 30) title = title.substr(0, sp); title += "…"; }
        return Ok({{"title", title}, {"source", "heuristic"}});
    }
    if (m == "sessions.storage.status" || m == "sessions.storage.run") {
        long long bytes = 0, files = 0; std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(d.sessions.Root(), ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) { bytes += static_cast<long long>(it->file_size(ec)); ++files; }
        json st{{"root", d.sessions.Root().string()}, {"bytes", bytes}, {"files", files}, {"sessions", d.sessions.List().size()}};
        if (m == "sessions.storage.status") return Ok(st);
        int removed = 0;  // maintenance: drop orphaned empty transcript files
        for (auto it = fs::directory_iterator(d.sessions.Root(), ec); it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".jsonl" && it->file_size(ec) == 0) { fs::remove(it->path(), ec); ++removed; }
        st["removedEmpty"] = removed; st["ran"] = true; return Ok(st);
    }
    if (m == "sessions.files.assets") {
        json out = json::array(); const auto media = d.sessions.Root() / "media";
        for (auto& r : p.value("refs", json::array())) {
            if (!r.is_string()) continue; auto ref = r.get<std::string>();
            if (ref.find("..") != std::string::npos) { out.push_back({{"ref", ref}, {"error", "invalid ref"}}); continue; }
            auto f = media / ref; std::error_code ec;
            if (fs::is_regular_file(f, ec)) out.push_back({{"ref", ref}, {"path", f.string()}, {"size", fs::file_size(f, ec)}}); else out.push_back({{"ref", ref}, {"error", "not found"}});
        }
        return Ok({{"assets", out}});
    }
    if (m == "sessions.catalog.list") {
        json out = json::array();
        for (auto& k : d.sessions.List()) { json e = d.sessions.IndexEntry(k); if (!e.is_object()) e = json::object(); out.push_back({{"catalogId", "local"}, {"sessionKey", k}, {"updatedAt", e.value("updatedAt", 0LL)}, {"messages", e.value("messages", 0)}}); }
        json ar = json::array(); std::error_code aec;
        for (auto it = fs::directory_iterator(root / "session-archive", aec); !aec && it != fs::directory_iterator(); it.increment(aec)) {
            auto a = ReadJsonFile(it->path(), json()); if (a.is_object()) ar.push_back({{"catalogId", "archive"}, {"archiveId", a["archiveId"]}, {"sessionKey", a["sessionKey"]}, {"archivedAt", a["archivedAt"]}, {"messages", a["messages"].size()}}); }
        return Ok({{"catalogs", json::array({{{"id", "local"}, {"label", "This gateway"}, {"sessions", out}}, {{"id", "archive"}, {"label", "Archived"}, {"sessions", ar}}})}});
    }
    if (m == "sessions.catalog.read") {
        const auto k = str("sessionKey");
        if (k.empty() || !d.sessions.Exists(k)) return Fail("session not found");
        auto ms = d.sessions.Load(k); const size_t lim = std::clamp(p.value("limit", 100), 1, 1000);
        size_t start = 0; try { start = str("cursor").empty() ? 0 : std::stoul(str("cursor")); } catch (...) {}
        json out = json::array(); for (size_t i = start; i < ms.size() && out.size() < lim; ++i) out.push_back(ms[i].ToJson());
        json r{{"messages", out}}; if (start + out.size() < ms.size()) r["nextCursor"] = std::to_string(start + out.size());
        return Ok(r);
    }
    if (m == "sessions.messages.unsubscribe") return Ok({{"unsubscribed", true}});
    return {};
}


// ---------------------------------------------------------------- talk (voice sessions over STT → agent turn → TTS) and MCP Apps views
std::string ReadAll(const fs::path& f) { std::ifstream i(f, std::ios::binary); return std::string(std::istreambuf_iterator<char>(i), {}); }
std::string B64Decode(const std::string& in) {
    static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : in) { if (c == '=' ) break; auto pos = tbl.find(static_cast<char>(c == '-' ? '+' : c == '_' ? '/' : c)); if (pos == std::string::npos) continue;
        val = (val << 6) + static_cast<int>(pos); bits += 6; if (bits >= 0) { out.push_back(static_cast<char>((val >> bits) & 0xFF)); bits -= 8; } }
    return out;
}
json SttTranscribe(const std::string& audio, const std::string& mime, const std::string& lang) {
    const char* sp = std::getenv("OPENCLAW_STT_PROVIDER");
    if (sp && std::string(sp) == "mock") {  // offline test provider: printable payloads are their own transcript
        bool printable = !audio.empty(); for (unsigned char c : audio) printable = printable && (c >= 32 || c == '\n');
        return {{"ok", true}, {"text", printable ? audio : "(audio " + std::to_string(audio.size()) + " bytes)"}, {"provider", "mock"}};
    }
    return voice::DefaultStt().Transcribe(audio, mime, "audio.wav", lang);
}
ExtraResult Talk(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    static std::mutex tmu;
    std::unique_lock<std::mutex> l(tmu);
    auto db = ReadJsonFile(MetaPath(root, "talk"), json{{"sessions", json::object()}, {"clients", json::object()}, {"mode", {{"enabled", false}}}});
    auto save = [&] { WriteJsonFile(MetaPath(root, "talk"), db); };
    const char* stt_url = std::getenv("OPENCLAW_STT_URL"); const char* stt_mock = std::getenv("OPENCLAW_STT_PROVIDER");
    const bool stt_ok = stt_url || (stt_mock && std::string(stt_mock) == "mock");
    if (m == "talk.config") return Ok({{"enabled", db["mode"].value("enabled", false)}, {"tts", d.tts ? d.tts->Status() : json(nullptr)},
                                       {"stt", {{"configured", stt_ok}, {"provider", stt_mock ? stt_mock : (stt_url ? "http" : "none")}}}, {"transports", json::array({"gateway-relay"})}});
    if (m == "talk.mode") { if (p.contains("enabled")) db["mode"]["enabled"] = p.value("enabled", false); if (p.contains("phase")) db["mode"]["phase"] = p["phase"]; save(); return Ok(db["mode"]); }
    if (m == "talk.catalog") {
        json realtime = json::array();
        if (d.js_host && d.js_host->HasRegistration("registerRealtimeVoiceProvider")) {
            auto r = d.js_host->Rpc("list_capability_providers", {{"kinds", json::array({"registerRealtimeVoiceProvider", "registerRealtimeTranscriptionProvider"})}});
            for (auto kind : {"registerRealtimeVoiceProvider", "registerRealtimeTranscriptionProvider"})
                for (auto& e : r.value("providers", json::object()).value(kind, json::array()))
                    realtime.push_back({{"kind", kind}, {"id", e.value("id", "")}, {"plugin", e.value("plugin", "")}, {"label", e.value("label", "")}});
        }
        return Ok({{"providers", d.tts ? d.tts->Providers() : json::array()}, {"stt", {{"configured", stt_ok}}}, {"realtime", realtime}});
    }
    if (m == "talk.speak") {
        if (str("text").empty()) return Fail("text is required");
        if (!d.tts) return Fail("TTS engine not available");
        l.unlock(); return Ok(d.tts->Speak(str("text"), str("voiceId")));
    }
    // A turn: transcript text → agent reply in the bound chat session → optional TTS.
    auto run_turn = [&](json& s, const std::string& text, const std::string& role_note) -> json {
        const auto key = s.value("sessionKey", std::string("agent:main:main"));
        s["transcript"].push_back({{"role", "user"}, {"text", text}, {"at", NowMs()}});
        const auto sid = s["sessionId"].get<std::string>();
        save(); l.unlock();
        auto r = GatewayRpc("chat.send", {{"sessionKey", key}, {"message", role_note + text}});
        json speech = nullptr;
        std::string reply = r.value("ok", false) ? r["payload"].value("text", std::string()) : std::string();
        if (!reply.empty() && d.tts && d.tts->Enabled()) speech = d.tts->Speak(reply, s.value("voice", std::string()));
        l.lock();
        db = ReadJsonFile(MetaPath(root, "talk"), db);
        auto& s2 = db["sessions"][sid];
        const auto turn = "turn-" + std::to_string(NowMs());
        if (r.value("ok", false)) s2["transcript"].push_back({{"role", "assistant"}, {"text", reply}, {"turnId", turn}, {"at", NowMs()}});
        s2["lastTurnId"] = turn; save();
        if (!r.value("ok", false)) return {{"ok", false}, {"error", r.value("error", json()).dump()}};
        return {{"ok", true}, {"turnId", turn}, {"transcript", text}, {"reply", reply}, {"speech", speech}};
    };
    if (m == "talk.session.create" || m == "talk.client.create") {
        const auto id = Rid(m == "talk.session.create" ? "talk" : "tclient");
        json s{{"sessionId", id}, {"sessionKey", str("sessionKey").empty() ? std::string("agent:main:main") : str("sessionKey")}, {"provider", str("provider")}, {"model", str("model")},
               {"voice", str("voice")}, {"language", str("language")}, {"mode", str("mode").empty() ? std::string("conversation") : str("mode")}, {"createdAt", NowMs()},
               {"state", "open"}, {"audioBytes", 0}, {"transcript", json::array()}, {"marks", json::array()}, {"toolResults", json::array()}};
        if (m == "talk.session.create") db["sessions"][id] = s; else { s["voiceSessionId"] = str("voiceSessionId"); db["clients"][s["sessionKey"].get<std::string>()] = s; }
        save();
        json out = s; out.erase("transcript"); out["stt"] = stt_ok; return Ok(out);
    }
    if (m.rfind("talk.client.", 0) == 0) {
        const auto key = str("sessionKey"); if (!db["clients"].contains(key)) return Fail("no talk client for session " + key);
        auto& c = db["clients"][key];
        if (m == "talk.client.close") { db["clients"].erase(key); save(); return Ok({{"closed", true}}); }
        if (m == "talk.client.transcript") {
            if (str("text").empty()) return Fail("text is required");
            c["transcript"].push_back({{"entryId", str("entryId")}, {"role", str("role").empty() ? std::string("user") : str("role")}, {"text", str("text")}, {"at", p.value("timestamp", NowMs())}});
            save(); l.unlock();
            GatewayRpc("chat.inject", {{"sessionKey", key}, {"message", "[voice " + p.value("role", std::string("user")) + "] " + str("text")}});
            return Ok({{"ok", true}});
        }
        if (m == "talk.client.steer") { if (str("text").empty()) return Fail("text is required"); l.unlock(); auto r = GatewayRpc("chat.send", {{"sessionKey", key}, {"message", str("text")}});
            return r.value("ok", false) ? Ok({{"reply", r["payload"].value("text", "")}}) : Fail("steer failed"); }
        if (m == "talk.client.toolCall") {
            if (str("name").empty()) return Fail("name is required");
            l.unlock(); auto r = GatewayRpc("tools.invoke", {{"name", str("name")}, {"args", p.value("args", json::object())}, {"sessionKey", key}});
            return r.value("ok", false) ? Ok({{"callId", str("callId")}, {"result", r["payload"].value("result", json())}}) : Fail("tool call failed: " + r.value("error", json()).dump());
        }
    }
    const auto sid = str("sessionId");
    if (!db["sessions"].contains(sid)) return Fail("unknown talk session: " + sid);
    auto& s = db["sessions"][sid];
    if (s.value("state", "") == "closed") return Fail("talk session is closed");
    if (m == "talk.session.close") { s["state"] = "closed"; s["closedAt"] = NowMs(); save(); return Ok({{"closed", true}, {"transcript", s["transcript"]}}); }
    if (m == "talk.session.acknowledgeMark") { if (str("markName").empty()) return Fail("markName is required"); s["marks"].push_back({{"name", str("markName")}, {"at", NowMs()}}); save(); return Ok({{"ok", true}}); }
    if (m == "talk.session.cancelOutput") { s["cancelled"].push_back({{"turnId", str("turnId").empty() ? s.value("lastTurnId", std::string()) : str("turnId")}, {"reason", str("reason")}}); save(); return Ok({{"cancelled", true}}); }
    if (m == "talk.session.appendAudio") {
        const auto audio = B64Decode(str("audio").empty() ? str("chunk") : str("audio"));
        const auto buf = root / "talk" / (sid + ".pcm");
        std::error_code ec; fs::create_directories(buf.parent_path(), ec);
        if (!audio.empty()) std::ofstream(buf, std::ios::app | std::ios::binary) << audio;
        s["audioBytes"] = s.value("audioBytes", 0LL) + static_cast<long long>(audio.size());
        if (!p.value("commit", p.value("final", false))) { save(); return Ok({{"buffered", s["audioBytes"]}}); }
        const auto all = ReadAll(buf); fs::remove(buf, ec); s["audioBytes"] = 0;
        auto tr = SttTranscribe(all, p.value("mime", std::string("audio/wav")), s.value("language", std::string()));
        if (!tr.value("ok", false)) { save(); return Fail("transcription failed: " + tr.value("error", std::string("no STT provider (set OPENCLAW_STT_URL)"))); }
        auto r = run_turn(s, tr.value("text", std::string()), "");
        return r.value("ok", false) ? Ok(r) : Fail(r.value("error", std::string("turn failed")));
    }
    if (m == "talk.session.steer") {
        if (str("text").empty()) return Fail("text is required");
        auto r = run_turn(s, str("text"), str("mode") == "instruction" ? "[instruction] " : "");
        return r.value("ok", false) ? Ok(r) : Fail(r.value("error", std::string("turn failed")));
    }
    if (m == "talk.session.submitToolResult") {
        if (str("callId").empty()) return Fail("callId is required");
        s["toolResults"].push_back({{"callId", str("callId")}, {"result", p.value("result", json())}, {"at", NowMs()}});
        if (p.value("suppressResponse", false)) { save(); return Ok({{"ok", true}}); }
        auto r = run_turn(s, "Tool result for " + str("callId") + ": " + p.value("result", json()).dump(), "");
        return r.value("ok", false) ? Ok(r) : Fail(r.value("error", std::string("turn failed")));
    }
    return {};
}

// Subscribed ui:// resources are polled through the MCP client; a content change emits
// "mcp.app.resourceUpdated" {viewId, uri} to connected clients (upstream: resources/updated relay).
void StartMcpAppPoller(ServerDeps& d, const fs::path& root) {
    static std::once_flag once;
    std::call_once(once, [&d, root] {
        std::thread([&d, root] {
            std::map<std::string, size_t> last;
            const char* ms = std::getenv("OPENCLAW_MCP_APP_POLL_MS");
            const int period = ms ? std::max(50, std::atoi(ms)) : 2000;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(period));
                auto cli = d.mcp_client; if (!cli) continue;
                auto db = ReadJsonFile(MetaPath(root, "mcp-apps"), json::object());
                const json views = db.value("views", json::object());
                for (auto& [vid, v] : views.items()) {
                    const json subs = v.value("subscriptions", json::array());
                    for (auto& u : subs) {
                        const auto uri = u.get<std::string>();
                        size_t h = 0; try { h = std::hash<std::string>{}(cli->ReadResource(uri).dump()); } catch (...) { continue; }
                        const auto k = vid + "|" + uri;
                        auto it = last.find(k);
                        if (it != last.end() && it->second != h) GatewayEmit("mcp.app.resourceUpdated", {{"viewId", vid}, {"uri", uri}, {"sessionKey", v.value("sessionKey", "")}});
                        last[k] = h;
                    }
                }
            }
        }).detach();
    });
}
ExtraResult McpApp(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    auto cli = d.mcp_client;
    static std::mutex vmu;
    std::lock_guard<std::mutex> l(vmu);
    auto db = ReadJsonFile(MetaPath(root, "mcp-apps"), json{{"views", json::object()}, {"settings", json::object()}, {"onboarded", json::object()}});
    auto save = [&] { WriteJsonFile(MetaPath(root, "mcp-apps"), db); };
    auto resources = [&]() -> json { if (!cli) return json::array(); auto r = cli->ListResources(); return r.contains("resources") ? r["resources"] : r.is_array() ? r : json::array(); };
    auto read = [&](const std::string& uri) -> json { if (!cli) return nullptr; return cli->ReadResource(uri); };
    if (m == "mcp.app.discover") {
        json apps = json::array();
        for (auto& r : resources()) { auto uri = r.value("uri", std::string()); if (uri.rfind("ui://", 0) == 0) apps.push_back({{"uri", uri}, {"name", r.value("name", uri)}, {"mimeType", r.value("mimeType", "text/html")}}); }
        return Ok({{"apps", apps}, {"connected", static_cast<bool>(cli)}});
    }
    if (m == "mcp.app.settings") {
        const auto app = str("appId").empty() ? str("uri") : str("appId");
        if (p.contains("settings")) { db["settings"][app] = p["settings"]; save(); }
        return Ok({{"appId", app}, {"settings", db["settings"].value(app, json::object())}});
    }
    if (m == "mcp.app.onboard") { const auto app = str("appId").empty() ? str("uri") : str("appId"); db["onboarded"][app] = NowMs(); save(); return Ok({{"onboarded", true}, {"appId", app}}); }
    if (m == "mcp.app.launch" || m == "mcp.app.formResource") {
        const auto uri = str("uri"), key = str("sessionKey");
        if (uri.empty()) return Fail("uri is required");
        if (!cli) return Fail("no MCP server connected");
        auto res = read(uri);
        json content = res.contains("contents") && res["contents"].is_array() && !res["contents"].empty() ? res["contents"][0] : res;
        if (m == "mcp.app.formResource") return Ok({{"uri", uri}, {"resource", content}});
        if (key.empty()) return Fail("sessionKey is required");
        const auto vid = Rid("view");
        db["views"][vid] = {{"viewId", vid}, {"uri", uri}, {"sessionKey", key}, {"createdAt", NowMs()}, {"modelContext", json::array()}, {"subscriptions", json::array()}};
        save();
        return Ok({{"viewId", vid}, {"uri", uri}, {"resource", content}});
    }
    const auto vid = str("viewId");
    if (!db["views"].contains(vid)) return Fail("MCP App view expired or unknown: " + vid);
    auto& v = db["views"][vid];
    if (!str("sessionKey").empty() && v["sessionKey"] != str("sessionKey")) return Fail("view belongs to another session");
    const auto host_dir = root / "mcp-apps" / vid;
    auto safe_path = [&](const std::string& rel) -> std::optional<fs::path> { if (rel.empty() || rel.find("..") != std::string::npos || rel[0] == '/') return std::nullopt; return host_dir / rel; };
    if (m == "mcp.app.view") { auto res = read(v["uri"].get<std::string>()); json out = v; out["resource"] = res.contains("contents") && !res["contents"].empty() ? res["contents"][0] : res; return Ok(out); }
    if (m == "mcp.app.modelContext") return Ok({{"state", v["modelContext"].empty() ? json(nullptr) : json{{"items", v["modelContext"]}, {"updateId", v.value("updateId", std::string())}}}});
    if (m == "mcp.app.updateModelContext") {
        json item = p.contains("content") ? p["content"] : p.value("structuredContent", json());
        if (item.is_null()) return Fail("content is required");
        v["modelContext"].push_back(item); v["updateId"] = Rid("upd"); save();
        return Ok({{"updateId", v["updateId"]}, {"items", v["modelContext"].size()}});
    }
    if (m == "mcp.app.removeModelContext") {
        if (str("updateId") != v.value("updateId", std::string())) return Fail("stale updateId");
        if (p.contains("index")) { auto i = p["index"].get<size_t>(); if (i < v["modelContext"].size()) v["modelContext"].erase(i); } else v["modelContext"] = json::array();
        v["updateId"] = Rid("upd"); save();
        return Ok({{"state", {{"items", v["modelContext"]}, {"updateId", v["updateId"]}}}});
    }
    if (m == "mcp.app.writeResource") {
        auto pth = safe_path(str("path").empty() ? str("uri") : str("path")); if (!pth) return Fail("invalid path");
        std::error_code ec; fs::create_directories(pth->parent_path(), ec); std::ofstream(*pth) << str("content");
        return Ok({{"written", true}, {"path", pth->string()}});
    }
    if (m == "mcp.app.subscribeResource" || m == "mcp.app.unsubscribeResource") {
        const auto uri = str("uri"); if (uri.empty()) return Fail("uri is required");
        json next = json::array(); for (auto& x : v["subscriptions"]) if (x != uri) next.push_back(x);
        if (m == "mcp.app.subscribeResource") next.push_back(uri);
        v["subscriptions"] = next; save();
        if (!next.empty()) StartMcpAppPoller(d, root);
        return Ok({{"subscriptions", next}});
    }
    if (m == "mcp.app.openFile") {
        auto pth = safe_path(str("path")); if (!pth) return Fail("invalid path");
        std::error_code ec; if (!fs::is_regular_file(*pth, ec)) return Fail("file not found");
        return Ok({{"path", pth->string()}, {"content", ReadAll(*pth)}});
    }
    if (m == "mcp.app.mention") {
        const auto text = str("text").empty() ? "[MCP App " + v["uri"].get<std::string>() + "]" : str("text");
        auto r = GatewayRpc("chat.inject", {{"sessionKey", v["sessionKey"]}, {"message", text}});
        return r.value("ok", false) ? Ok({{"mentioned", true}}) : Fail("mention failed");
    }
    return {};
}


// ---------------------------------------------------------------- plugins: catalog, install/uninstall, inspect, reload, controlUi, credentials, skills
json PluginManifest(const fs::path& dir) {
    for (auto n : {"openclaw.plugin.json", "plugin.json"}) { auto j = ReadJsonFile(dir / n, json()); if (j.is_object()) { j["__file"] = n; return j; } }
    auto pk = ReadJsonFile(dir / "package.json", json());
    if (pk.is_object()) { json j = pk.value("openclaw", json::object()); j["name"] = pk.value("name", ""); j["version"] = pk.value("version", ""); j["__file"] = "package.json"; return j; }
    return json::object();
}
json PluginCatalog(const fs::path& base) {
    json cat = json::array();
    std::vector<fs::path> files;
    if (const char* e = std::getenv("OPENCLAW_PLUGIN_CATALOG")) files.emplace_back(e);
    files.push_back(base / "catalog.json");
    for (auto& f : files) {
        auto j = ReadJsonFile(f, json());
        json items = j.is_array() ? j : j.value("plugins", json::array());
        for (auto it : items) {
            if (it.contains("source") && it["source"].is_string() && !it["source"].get<std::string>().empty() && it["source"].get<std::string>()[0] != '/' &&
                it["source"].get<std::string>().find(':') == std::string::npos)
                it["source"] = (f.parent_path() / it["source"].get<std::string>()).lexically_normal().string();
            bool dup = false; for (auto& c : cat) dup = dup || c["id"] == it["id"];
            if (!dup) cat.push_back(it);
        }
    }
    return cat;
}
ExtraResult Plugins(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    const auto inst_root = root / "plugins";
    const fs::path base = d.plugins.Root().empty() ? fs::path("/workspace/openclaw-cpp/plugins") : d.plugins.Root();
    d.plugins.AddRoot(inst_root);
    auto rescan = [&] { d.plugins.Scan(base); return d.plugins.LoadNatives(); };
    auto ledger = ReadJsonFile(MetaPath(root, "plugin-installs"), json::object());
    auto find = [&](const std::string& id) -> std::optional<plugins::Plugin> { for (auto& x : d.plugins.List()) if (x.id == id) return x; return std::nullopt; };
    auto id = str("id").empty() ? (str("pluginId").empty() ? str("name") : str("pluginId")) : str("id");
    auto cat = PluginCatalog(base);
    auto installed = [&](const std::string& i) { return find(i).has_value(); };
    auto decorate = [&](json c) {
        bool on = installed(c.value("id", ""));
        for (auto& x : d.plugins.List()) on = on || (c.contains("source") && fs::path(c.value("source", "")).lexically_normal() == x.path.lexically_normal());
        c["installed"] = on; if (ledger.contains(c.value("id", ""))) c["install"] = ledger[c.value("id", "")]; return c; };
    if (m == "plugins.catalog.categories") {
        std::map<std::string, int> n; for (auto& c : cat) n[c.value("category", "other")]++;
        json out = json::array(); for (auto& [k, v] : n) out.push_back({{"id", k}, {"count", v}});
        return Ok({{"categories", out}});
    }
    if (m == "plugins.catalog.browse") {
        const auto q = str("query").empty() ? str("q") : str("query"); const auto category = str("category");
        json out = json::array();
        for (auto& c : cat) {
            if (!category.empty() && c.value("category", "other") != category) continue;
            if (!q.empty() && c.dump().find(q) == std::string::npos) continue;
            out.push_back(decorate(c));
        }
        return Ok({{"plugins", out}, {"total", out.size()}});
    }
    if (m == "plugins.catalog.get") {
        for (auto& c : cat) if (c.value("id", "") == id) return Ok({{"plugin", decorate(c)}});
        return Fail("plugin not in catalog: " + id);
    }
    if (m == "plugins.reload") {
        int n = rescan();
        plugins::NotifyReload(d.js_host, {{"reason", "plugins.reload"}});
        return Ok({{"plugins", d.plugins.ListJson()}, {"nativesLoaded", n}});
    }
    if (m == "plugins.listCapabilities") {
        if (!d.js_host || !d.js_host->Running()) return Fail("js plugin host not running");
        auto r = d.js_host->Rpc("list_capability_providers", json::object());
        auto regs = d.js_host->Rpc("list_registrations", json::object());
        return Ok({{"providers", r.value("providers", json::object())}, {"registrations", regs.value("registrations", json::object())}});
    }
    if (m == "plugins.invokeRegistration") {
        if (!d.js_host || !d.js_host->Running()) return Fail("js plugin host not running");
        const auto kind = str("kind");
        if (kind.empty()) return Fail("kind required (e.g. registerEmbeddingProvider)");
        auto r = plugins::InvokeRegistration(d.js_host, kind, p.value("args", json::object()), p.value("method", "execute"), str("id"));
        if (!r) return Fail("no registration for " + kind);
        return Ok(*r);
    }
    if (m == "plugins.install") {
        std::string src = str("source").empty() ? str("spec") : str("source");
        if (src.empty()) for (auto& c : cat) if (c.value("id", "") == id) src = c.value("source", "");
        if (src.empty()) return Fail("source (path, .tgz, npm:<spec>) or a catalog id is required");
        std::error_code ec; fs::create_directories(inst_root, ec);
        const auto stage = root / "tmp" / Rid("plugin-stage");
        fs::create_directories(stage, ec);
        fs::path pkg;
        std::string kind;
        if (fs::is_directory(src, ec)) { kind = "path"; pkg = stage / "package"; fs::copy(src, pkg, fs::copy_options::recursive, ec); if (ec) return Fail("copy failed: " + ec.message()); }
        else if (fs::is_regular_file(src, ec)) {
            kind = "archive"; auto r = Run({"tar", "-xzf", src, "-C", stage.string()}, {}); if (r.code != 0) return Fail("extract failed: " + r.err);
            for (auto& e : fs::directory_iterator(stage)) if (e.is_directory()) pkg = e.path();
            if (pkg.empty()) pkg = stage;
        } else if (src.rfind("npm:", 0) == 0) {
            kind = "npm"; auto r = Run({"npm", "pack", src.substr(4), "--silent"}, stage);
            if (r.code != 0) return Fail("npm pack failed (network/npm required): " + Trim(r.err));
            auto tgz = stage / Trim(r.out); auto x = Run({"tar", "-xzf", tgz.string(), "-C", stage.string()}, {}); if (x.code != 0) return Fail("extract failed");
            pkg = stage / "package";
        } else return Fail("unsupported plugin source: " + src);
        auto man = PluginManifest(pkg);
        if (man.empty()) { fs::remove_all(stage, ec); return Fail("no plugin manifest (openclaw.plugin.json, plugin.json or package.json#openclaw)"); }
        std::string pid = man.value("id", std::string());
        if (pid.empty()) pid = id.empty() ? pkg.filename().string() : id;
        if (pid.find('/') != std::string::npos) pid = pid.substr(pid.rfind('/') + 1);
        if (pid.empty() || pid.find("..") != std::string::npos) return Fail("invalid plugin id");
        const auto dest = inst_root / pid;
        if (installed(pid) && !p.value("force", false)) { fs::remove_all(stage, ec); return Fail("plugin already installed: " + pid + " (pass force: true)"); }
        fs::remove_all(dest, ec); fs::rename(pkg, dest, ec);
        if (ec) { fs::copy(pkg, dest, fs::copy_options::recursive, ec); }
        fs::remove_all(stage, ec);
        if (p.value("enable", true)) { auto pj = ReadJsonFile(dest / "plugin.json", json::object()); pj["enabled"] = true; if (!pj.contains("name")) pj["name"] = man.value("name", pid); WriteJsonFile(dest / "plugin.json", pj); }
        ledger[pid] = {{"source", src}, {"kind", kind}, {"path", dest.string()}, {"installedAt", NowMs()}, {"version", man.value("version", "")}};
        WriteJsonFile(MetaPath(root, "plugin-installs"), ledger);
        rescan();
        GatewayEmit("plugins.changed", {{"id", pid}, {"action", "install"}});
        return Ok({{"id", pid}, {"installed", true}, {"path", dest.string()}, {"kind", kind}});
    }
    if (m == "plugins.uninstall") {
        if (!ledger.contains(id)) return Fail(installed(id) ? "plugin " + id + " is bundled, not installed; disable it instead" : "plugin not installed: " + id);
        std::error_code ec; fs::path dest = ledger[id].value("path", std::string());
        if (dest.parent_path() != inst_root) return Fail("refusing to remove plugin outside install root");
        d.plugins.UnloadNatives(); fs::remove_all(dest, ec);
        ledger.erase(id); WriteJsonFile(MetaPath(root, "plugin-installs"), ledger);
        rescan();
        GatewayEmit("plugins.changed", {{"id", id}, {"action", "uninstall"}});
        return Ok({{"id", id}, {"removed", true}});
    }
    auto pl = find(id);
    if (m == "plugins.controlUi.list") {
        json out = json::array();
        for (auto& x : d.plugins.List()) { auto man = PluginManifest(x.path); for (auto& u : man.value("controlUi", json::array())) { json e = u; e["pluginId"] = x.id; e["enabled"] = x.enabled; out.push_back(e); } }
        return Ok({{"entries", out}});
    }
    if (m == "plugins.controlUi.status" || m == "plugins.controlUi.report") {
        auto st = ReadJsonFile(MetaPath(root, "plugin-controlui"), json::object());
        if (m == "plugins.controlUi.report") {
            if (id.empty()) return Fail("pluginId is required");
            st[id] = {{"status", str("status").empty() ? std::string("ok") : str("status")}, {"error", p.value("error", json())}, {"at", NowMs()}};
            WriteJsonFile(MetaPath(root, "plugin-controlui"), st);
            GatewayEmit("plugins.controlUi.changed", {{"pluginId", id}});
            return Ok({{"ok", true}});
        }
        return Ok({{"status", id.empty() ? st : st.value(id, json(nullptr))}});
    }
    if (!pl) return Fail("unknown plugin: " + id);
    auto man = PluginManifest(pl->path);
    if (m == "plugins.inspect") {
        json files = json::array(); std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(pl->path, ec); it != fs::recursive_directory_iterator() && files.size() < 200; it.increment(ec))
            if (it->is_regular_file()) files.push_back(fs::relative(it->path(), pl->path).string());
        json skills = json::array(); if (fs::is_directory(pl->path / "skills", ec)) for (auto& e : fs::directory_iterator(pl->path / "skills")) if (e.is_directory()) skills.push_back(e.path().filename().string());
        return Ok({{"plugin", {{"id", pl->id}, {"name", pl->name}, {"enabled", pl->enabled}, {"path", pl->path.string()}, {"native", pl->native_lib}, {"nativeLoaded", pl->native_loaded}, {"js", pl->js_entry}}},
                   {"manifest", man}, {"files", files}, {"skills", skills}, {"install", ledger.value(id, json(nullptr))}});
    }
    if (m == "plugins.credentials.inspect") {
        json out = json::array();
        auto want = man.value("credentials", json::array());
        const json props = man.value("configSchema", json::object()).value("properties", json::object());
        for (auto& [k, v] : props.items())
            if (k.find("key") != std::string::npos || k.find("Key") != std::string::npos || k.find("token") != std::string::npos || k.find("Token") != std::string::npos) want.push_back({{"config", k}});
        const auto pcfg = d.config.raw.value("plugins", json::object()).value("entries", json::object()).value(id, json::object()).value("config", json::object());
        for (auto c : want) {
            if (c.is_string()) c = {{"env", c}};
            bool present = false;
            if (c.contains("env")) { const char* e = std::getenv(c["env"].get<std::string>().c_str()); present = e && *e; }
            if (c.contains("config")) present = present || pcfg.contains(c["config"].get<std::string>());
            c["present"] = present; out.push_back(c);  // never echo values
        }
        return Ok({{"pluginId", id}, {"credentials", out}});
    }
    if (m == "plugins.skills.read") {
        const auto sk = str("skill").empty() ? str("skillName") : str("skill");
        if (sk.empty() || sk.find("..") != std::string::npos || sk.find('/') != std::string::npos) return Fail("skill is required");
        const auto f = pl->path / "skills" / sk / "SKILL.md"; std::error_code ec;
        if (!fs::is_regular_file(f, ec)) return Fail("skill not found: " + sk);
        std::ifstream i(f); return Ok({{"pluginId", id}, {"skill", sk}, {"content", std::string(std::istreambuf_iterator<char>(i), {})}});
    }
    return {};
}


// ---------------------------------------------------------------- sessions: archive catalog, import/continue, terminal, reveal, github, plugin state, recover, activity summary
ExtraResult Sessions2(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    const auto key = str("sessionKey").empty() ? str("key") : str("sessionKey");
    const auto arch = root / "session-archive";
    std::error_code ec;
    auto agent_of = [&](const std::string& k) { if (!str("agentId").empty()) return str("agentId"); if (k.rfind("agent:", 0) == 0) { auto e = k.find(':', 6); return k.substr(6, e - 6); } return std::string("main"); };
    if (m == "sessions.catalog.archive") {
        if (str("catalogId") != "" && str("catalogId") != "local") return Fail("catalog cannot archive");
        if (key.empty() || !d.sessions.Exists(key)) return Fail("session not found");
        fs::create_directories(arch, ec);
        json msgs = json::array(); for (auto& x : d.sessions.Load(key)) msgs.push_back(x.ToJson());
        const auto id = SafeName(key) + "-" + std::to_string(NowMs());
        WriteJsonFile(arch / (id + ".json"), {{"archiveId", id}, {"sessionKey", key}, {"archivedAt", NowMs()}, {"messages", msgs}});
        d.sessions.Remove(key);
        GatewayEmit("sessions.changed", {{"sessionKey", key}, {"reason", "archived"}});
        return Ok({{"archived", true}, {"archiveId", id}, {"messages", msgs.size()}});
    }
    auto save_msgs = [&](const std::string& k, const json& msgs) {
        std::vector<agent::Message> v; for (auto& x : msgs) v.push_back(agent::Message::FromJson(x)); d.sessions.Save(k, v); return v.size();
    };
    if (m == "sessions.catalog.import" || m == "sessions.catalog.continue") {
        json msgs; std::string src_key;
        const auto aid = str("archiveId").empty() ? str("entryId") : str("archiveId");
        if (!aid.empty()) {
            if (aid.find("..") != std::string::npos || aid.find('/') != std::string::npos) return Fail("invalid archiveId");
            auto a = ReadJsonFile(arch / (aid + ".json"), json()); if (!a.is_object()) return Fail("unknown archive entry: " + aid);
            msgs = a["messages"]; src_key = a.value("sessionKey", "");
        } else if (p.contains("messages")) msgs = p["messages"];
        else if (!str("path").empty()) {
            std::ifstream in(str("path")); if (!in) return Fail("cannot read " + str("path"));
            msgs = json::array(); std::string line;
            while (std::getline(in, line)) { if (Trim(line).empty()) continue; try { auto j = json::parse(line); if (j.contains("message")) j = j["message"]; if (j.contains("role")) msgs.push_back(j); } catch (...) {} }
        } else if (m == "sessions.catalog.continue" && !key.empty() && d.sessions.Exists(key)) src_key = key;
        else return Fail("archiveId, messages or path is required");
        std::string dest = str("targetSessionKey").empty() ? (key.empty() ? src_key : key) : str("targetSessionKey");
        if (dest.empty()) dest = "agent:main:import-" + std::to_string(NowMs());
        if (!msgs.is_null()) {
            if (d.sessions.Exists(dest) && !p.value("overwrite", false)) {
                if (m == "sessions.catalog.import") return Fail("session exists: " + dest + " (pass overwrite: true)");
                dest = dest + "-" + std::to_string(NowMs());
            }
            auto n = save_msgs(dest, msgs);
            if (!aid.empty() && p.value("removeArchive", m == "sessions.catalog.continue")) fs::remove(arch / (aid + ".json"), ec);
            if (m == "sessions.catalog.import") return Ok({{"sessionKey", dest}, {"imported", n}});
        }
        json out{{"sessionKey", dest}};
        if (!str("message").empty()) {
            auto r = GatewayRpc("chat.send", {{"sessionKey", dest}, {"message", str("message")}});
            if (!r.value("ok", false)) return Fail("continue turn failed");
            out["reply"] = r["payload"].value("text", "");
        }
        return Ok(out);
    }
    if (m == "sessions.catalog.startTerminal") {
        auto ws = str("cwd").empty() ? WorkspaceOf(d, agent_of(key)).string() : str("cwd");
        auto r = GatewayRpc("terminal.open", {{"cwd", ws}, {"sessionKey", key}});
        return r.value("ok", false) ? Ok(r["payload"]) : Fail("terminal open failed: " + r.value("error", json()).dump());
    }
    if (key.empty()) return Fail("sessionKey is required");
    const auto ws = WorkspaceOf(d, agent_of(key));
    if (m == "sessions.files.reveal") {
        const auto rel = str("path");
        if (rel.empty()) return Fail("path is required");
        auto f = (fs::path(rel).is_absolute() ? fs::path(rel) : ws / rel).lexically_normal();
        auto wsn = ws.lexically_normal().string();
        if (f.string().rfind(wsn, 0) != 0) return Fail("path is outside the session workspace");
        if (!fs::exists(f, ec)) return Fail("file not found");
        bool opened = false;
        if (const char* cmd = std::getenv("OPENCLAW_REVEAL_CMD")) opened = Run({cmd, f.string()}, {}).code == 0;  // desktop hook (e.g. xdg-open)
        return Ok({{"path", f.string()}, {"directory", fs::is_directory(f, ec) ? f.string() : f.parent_path().string()}, {"opened", opened}});
    }
    if (m == "sessions.github.options" || m == "sessions.github.status") {
        auto git = [&](std::vector<std::string> a) { a.insert(a.begin(), "git"); auto r = Run(a, ws); return r.code == 0 ? Trim(r.out) : std::string(); };
        if (git({"rev-parse", "--is-inside-work-tree"}) != "true") return Ok({{"available", false}, {"reason", "workspace is not a git repository"}, {"workspace", ws.string()}});
        const auto branch = git({"symbolic-ref", "--short", "HEAD"});
        if (m == "sessions.github.options") {
            json remotes = json::array(), branches = json::array();
            std::istringstream rs(git({"remote", "-v"})); std::string l; while (std::getline(rs, l)) if (l.find("(push)") != std::string::npos) { std::istringstream ls(l); std::string n, u; ls >> n >> u; remotes.push_back({{"name", n}, {"url", u}}); }
            std::istringstream bs(git({"branch", "--format=%(refname:short)"})); while (std::getline(bs, l)) branches.push_back(l);
            auto accts = ReadJsonFile(MetaPath(root, "github-accounts"), json::object());
            return Ok({{"available", true}, {"branch", branch}, {"remotes", remotes}, {"branches", branches}, {"accounts", accts.is_object() ? accts.size() : 0}});
        }
        std::istringstream ss(git({"status", "--porcelain"})); std::string l; json changed = json::array(); while (std::getline(ss, l)) changed.push_back({{"status", l.substr(0, 2)}, {"path", l.size() > 3 ? l.substr(3) : ""}});
        json st{{"available", true}, {"branch", branch}, {"head", git({"rev-parse", "HEAD"})}, {"changed", changed}, {"dirty", !changed.empty()}};
        auto up = git({"rev-list", "--left-right", "--count", "@{upstream}...HEAD"});
        if (!up.empty()) { std::istringstream us(up); int b = 0, a = 0; us >> b >> a; st["behind"] = b; st["ahead"] = a; } else st["upstream"] = nullptr;
        return Ok(st);
    }
    auto meta = ReadJsonFile(MetaPath(root, "session-meta"), json{{"sessions", json::object()}});
    auto& sm = meta["sessions"][key]; if (!sm.is_object()) sm = json::object();
    auto save_meta = [&] { WriteJsonFile(MetaPath(root, "session-meta"), meta); };
    if (m == "sessions.pluginPatch") {
        const auto pid = str("pluginId").empty() ? str("plugin") : str("pluginId");
        bool known = false; for (auto& x : d.plugins.List()) known = known || x.id == pid;
        if (!known) return Fail("unknown plugin: " + pid);
        if (!p.contains("patch") || !p["patch"].is_object()) return Fail("patch object is required");
        auto& st = sm["pluginState"][pid]; if (!st.is_object()) st = json::object();
        st.merge_patch(p["patch"]); save_meta();
        GatewayEmit("sessions.changed", {{"sessionKey", key}, {"reason", "pluginPatch"}, {"pluginId", pid}});
        return Ok({{"sessionKey", key}, {"pluginId", pid}, {"state", st}});
    }
    if (m == "sessions.providerReview.continue") {
        auto& rv = sm["providerReview"];
        if (!rv.is_object() || rv.value("status", "") != "pending") return Fail("no provider review pending for this session; review the findings in chat");
        if (!str("sessionId").empty() && rv.value("sessionId", str("sessionId")) != str("sessionId")) return Fail("The session changed; refresh its findings.");
        rv["status"] = "acknowledged"; rv["acknowledgedAt"] = NowMs(); save_meta();
        auto r = GatewayRpc("chat.send", {{"sessionKey", key}, {"message", str("message").empty() ? std::string("Continue after provider review.") : str("message")}});
        return r.value("ok", false) ? Ok({{"continued", true}, {"reply", r["payload"].value("text", "")}}) : Fail("continue failed");
    }
    if (!d.sessions.Exists(key)) return Fail("session not found");
    if (m == "sessions.recover") {
        auto msgs = d.sessions.Load(key);
        std::vector<agent::Message> keep; int dropped = 0;
        std::set<std::string> answered; for (auto& x : msgs) if (x.role == "tool" && x.tool_call_id) answered.insert(*x.tool_call_id);
        for (auto& x : msgs) {
            if (x.role != "system" && x.role != "user" && x.role != "assistant" && x.role != "tool") { ++dropped; continue; }
            if (x.role == "tool" && !x.tool_call_id) { ++dropped; continue; }
            keep.push_back(x);
        }
        // close dangling tool calls so providers accept the transcript
        int closed = 0;
        for (auto& x : msgs) if (x.role == "assistant" && x.tool_calls.is_array()) for (auto& c : x.tool_calls) { auto id = c.value("id", std::string()); if (!id.empty() && !answered.count(id)) {
            agent::Message t; t.role = "tool"; t.tool_call_id = id; t.content = "[recovered: tool call interrupted]"; keep.push_back(t); answered.insert(id); ++closed; } }
        d.sessions.Save(key, keep);
        sm["recoveredAt"] = NowMs(); save_meta();
        return Ok({{"sessionKey", key}, {"messages", keep.size()}, {"dropped", dropped}, {"closedToolCalls", closed}});
    }
    if (m == "sessions.activitySummary.ensure") {
        auto msgs = d.sessions.Load(key);
        auto& cur = sm["activitySummary"];
        if (cur.is_object() && cur.value("messages", -1) == static_cast<int>(msgs.size()) && !p.value("force", false)) return Ok({{"summary", cur}, {"cached", true}});
        std::map<std::string, int> roles; json tools = json::array(); std::string last_user, last_assistant;
        for (auto& x : msgs) { roles[x.role]++; if (x.role == "user") last_user = x.content; if (x.role == "assistant") last_assistant = x.content;
            if (x.role == "tool" && x.name) tools.push_back(*x.name); }
        json rc = json::object(); for (auto& [k, v] : roles) rc[k] = v;
        cur = {{"messages", msgs.size()}, {"roles", rc}, {"tools", tools}, {"lastUser", Utf8Cut(last_user, 200)}, {"lastAssistant", Utf8Cut(last_assistant, 200)}, {"updatedAt", NowMs()},
               {"text", std::to_string(roles["user"]) + " user turns, " + std::to_string(roles["assistant"]) + " replies, " + std::to_string(tools.size()) + " tool results"}};
        save_meta();
        return Ok({{"summary", cur}, {"cached", false}});
    }
    return {};
}


// ---------------------------------------------------------------- system agent (openclaw.setup/*, openclaw.chat) and the wizard step engine
struct ProviderHint { const char* provider; const char* env; const char* model; };
const ProviderHint kProviderHints[] = {
    {"openai", "OPENAI_API_KEY", "gpt-4o-mini"}, {"anthropic", "ANTHROPIC_API_KEY", "claude-sonnet-4"}, {"deepseek", "DEEPSEEK_API_KEY", "deepseek-chat"},
    {"google", "GEMINI_API_KEY", "gemini-2.5-flash"}, {"openrouter", "OPENROUTER_API_KEY", "openrouter/auto"}, {"xai", "XAI_API_KEY", "grok-4"},
    {"mistral", "MISTRAL_API_KEY", "mistral-large-latest"}, {"groq", "GROQ_API_KEY", "llama-3.3-70b-versatile"}};
json SetupDetect(ServerDeps& d) {
    json cands = json::array();
    for (auto& h : kProviderHints) { const char* v = std::getenv(h.env); if (v && *v) cands.push_back({{"provider", h.provider}, {"model", h.model}, {"source", "env"}, {"env", h.env}, {"authChoice", std::string(h.provider) + "-api-key"}}); }
    const json provs = d.config.raw.value("models", json::object()).value("providers", json::object());
    for (auto& [k, v] : provs.items()) {
        bool dup = false; for (auto& c : cands) dup = dup || c["provider"] == k;
        if (dup) continue;
        std::string model; if (v.contains("models") && v["models"].is_array() && !v["models"].empty()) model = v["models"][0].is_string() ? v["models"][0].get<std::string>() : v["models"][0].value("id", "");
        cands.push_back({{"provider", k}, {"model", model}, {"source", "config"}, {"hasKey", v.contains("apiKey")}});
    }
    if (d.router) for (auto& n : d.router->Names()) { bool dup = false; for (auto& c : cands) dup = dup || c["provider"] == n; if (!dup) cands.push_back({{"provider", n}, {"model", ""}, {"source", "runtime"}}); }
    const auto cur = d.config.raw.value("agents", json::object()).value("defaults", json::object()).value("model", json());
    return {{"candidates", cands}, {"currentModel", cur}, {"configured", !cur.is_null() || (d.router && !d.router->Names().empty())}};
}
json SetupVerify(ServerDeps& d, const std::string& provider, const std::string& model) {
    if (!d.router) return {{"ok", false}, {"error", "no provider router"}};
    if (!d.router->Has(provider)) return {{"ok", false}, {"error", "provider not available in this gateway: " + provider}};
    agent::Message u; u.role = "user"; u.content = "Reply with the single word: ok";
    const auto t0 = NowMs();
    auto r = d.router->CompleteRoute(model.empty() ? provider : provider + "/" + model, {u}, json::array());
    return {{"ok", r.ok}, {"provider", provider}, {"model", model}, {"latencyMs", NowMs() - t0}, {"error", r.ok ? json(nullptr) : json(r.error)}, {"sample", Utf8Cut(r.message.content, 80)}};
}

// ---------------------------------------------------------------- OAuth 2.1 authorization-code + PKCE (S256) for MCP servers and model providers
namespace oauthflow {
std::string B64Url(const unsigned char* d, size_t n) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"; std::string o; size_t i = 0;
    for (; i + 2 < n; i += 3) { unsigned v = d[i] << 16 | d[i + 1] << 8 | d[i + 2]; o += a[v >> 18 & 63]; o += a[v >> 12 & 63]; o += a[v >> 6 & 63]; o += a[v & 63]; }
    if (n - i == 1) { unsigned v = d[i] << 16; o += a[v >> 18 & 63]; o += a[v >> 12 & 63]; }
    else if (n - i == 2) { unsigned v = d[i] << 16 | d[i + 1] << 8; o += a[v >> 18 & 63]; o += a[v >> 12 & 63]; o += a[v >> 6 & 63]; }
    return o;
}
std::string RandUrl(size_t n) { std::vector<unsigned char> b(n); RAND_bytes(b.data(), static_cast<int>(n)); return B64Url(b.data(), n); }
std::string Challenge(const std::string& verifier) { unsigned char h[32]; SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), h); return B64Url(h, 32); }
std::string Origin(const std::string& url) { std::smatch m; return std::regex_search(url, m, std::regex(R"(^(https?://[^/]+))")) ? m[1].str() : std::string(); }
json GetJson(const std::string& url) { auto r = GitHubRequest(url, "GET", ""); if (r.status != 200) return nullptr; try { return json::parse(r.body); } catch (...) { return nullptr; } }
std::string QueryParam(const std::string& s, const std::string& k) {
    std::smatch m; if (!std::regex_search(s, m, std::regex("(?:^|[?&#])" + k + "=([^&#]*)"))) return {};
    std::string v = m[1], o; for (size_t i = 0; i < v.size(); ++i) { if (v[i] == '%' && i + 2 < v.size()) { o += static_cast<char>(std::stoi(v.substr(i + 1, 2), nullptr, 16)); i += 2; } else o += v[i] == '+' ? ' ' : v[i]; }
    return o;
}
// Resolve endpoints: explicit oauth config wins; else RFC 9728 protected-resource → RFC 8414 metadata; dynamic client registration when no clientId.
json Resolve(const json& oauth, const std::string& resource_url, const std::string& redirect) {
    json o = oauth.is_object() ? oauth : json::object();
    std::string authz = o.value("authorizationUrl", std::string()), token = o.value("tokenUrl", std::string()), reg, client = o.value("clientId", std::string());
    if (authz.empty() || token.empty()) {
        std::string issuer = o.value("issuer", std::string());
        if (issuer.empty() && !resource_url.empty()) { auto pr = GetJson(Origin(resource_url) + "/.well-known/oauth-protected-resource");
            if (pr.is_object() && pr.contains("authorization_servers") && !pr["authorization_servers"].empty()) issuer = pr["authorization_servers"][0]; else issuer = Origin(resource_url); }
        if (issuer.empty()) throw std::runtime_error("no OAuth issuer configured");
        auto md = GetJson(Origin(issuer) + "/.well-known/oauth-authorization-server");
        if (!md.is_object()) md = GetJson(Origin(issuer) + "/.well-known/openid-configuration");
        if (!md.is_object()) throw std::runtime_error("OAuth metadata discovery failed at " + issuer);
        authz = md.value("authorization_endpoint", authz); token = md.value("token_endpoint", token); reg = md.value("registration_endpoint", std::string());
    }
    if (authz.empty() || token.empty()) throw std::runtime_error("OAuth endpoints are not configured");
    std::string secret = o.value("clientSecret", std::string());
    if (client.empty()) {
        if (reg.empty()) throw std::runtime_error("no clientId configured and the server offers no dynamic client registration");
        auto r = GitHubRequest(reg, "POST", "", json{{"client_name", "OpenClaw"}, {"redirect_uris", {redirect}}, {"grant_types", {"authorization_code", "refresh_token"}},
                                                     {"response_types", {"code"}}, {"token_endpoint_auth_method", "none"}}.dump());
        json j; try { j = json::parse(r.body); } catch (...) {}
        if ((r.status != 200 && r.status != 201) || !j.contains("client_id")) throw std::runtime_error("dynamic client registration failed (HTTP " + std::to_string(r.status) + ")");
        client = j["client_id"]; secret = j.value("client_secret", secret);
    }
    std::string scope; if (o.contains("scopes") && o["scopes"].is_array()) for (auto& s : o["scopes"]) scope += (scope.empty() ? "" : " ") + s.get<std::string>(); else scope = o.value("scope", std::string());
    return {{"authorizationUrl", authz}, {"tokenUrl", token}, {"clientId", client}, {"clientSecret", secret}, {"scope", scope}};
}
json Begin(const json& ep, const std::string& redirect, const std::string& resource) {
    const auto verifier = RandUrl(48), state = RandUrl(24);
    std::string url = ep["authorizationUrl"].get<std::string>(); url += (url.find('?') == std::string::npos ? "?" : "&");
    url += "response_type=code&client_id=" + UrlEncode(ep["clientId"]) + "&redirect_uri=" + UrlEncode(redirect) + "&state=" + state +
           "&code_challenge=" + Challenge(verifier) + "&code_challenge_method=S256";
    if (!ep["scope"].get<std::string>().empty()) url += "&scope=" + UrlEncode(ep["scope"]);
    if (!resource.empty()) url += "&resource=" + UrlEncode(resource);
    return {{"url", url}, {"state", state}, {"verifier", verifier}};
}
json Exchange(const json& ctx, const std::string& answer) {
    std::string code = answer.find("code=") != std::string::npos ? QueryParam(answer, "code") : Trim(answer);
    if (answer.find("error=") != std::string::npos) throw std::runtime_error("authorization denied: " + QueryParam(answer, "error"));
    if (answer.find("state=") != std::string::npos && QueryParam(answer, "state") != ctx["state"].get<std::string>()) throw std::runtime_error("OAuth state mismatch");
    if (code.empty()) throw std::runtime_error("no authorization code in the answer");
    const auto& ep = ctx["endpoints"];
    std::string form = "grant_type=authorization_code&code=" + UrlEncode(code) + "&redirect_uri=" + UrlEncode(ctx["redirectUri"]) + "&client_id=" + UrlEncode(ep["clientId"]) +
                       "&code_verifier=" + UrlEncode(ctx["verifier"]);
    if (!ep.value("clientSecret", std::string()).empty()) form += "&client_secret=" + UrlEncode(ep["clientSecret"]);
    if (!ctx.value("resource", std::string()).empty()) form += "&resource=" + UrlEncode(ctx["resource"]);
    auto r = FormPost(ep["tokenUrl"], form);
    json j; try { j = json::parse(r.body); } catch (...) {}
    if (r.status != 200 || !j.contains("access_token")) throw std::runtime_error("token exchange failed (HTTP " + std::to_string(r.status) + "): " + (j.contains("error") ? j["error"].dump() : r.error));
    json tok{{"access_token", j["access_token"]}, {"token_type", j.value("token_type", "Bearer")}, {"obtainedAt", NowMs()}, {"tokenUrl", ep["tokenUrl"]}, {"clientId", ep["clientId"]}};
    if (j.contains("refresh_token")) tok["refresh_token"] = j["refresh_token"];
    if (j.contains("expires_in")) tok["expiresAt"] = NowMs() + j["expires_in"].get<long long>() * 1000;
    if (j.contains("scope")) tok["scope"] = j["scope"];
    return tok;
}
}  // namespace oauthflow
std::mutex g_setup_mu;
ExtraResult SystemAgent(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    static const std::string kSys = "agent:main:openclaw-system";
    auto led = ReadJsonFile(MetaPath(root, "system-agent"), json{{"changes", json::array()}, {"approvals", json::array()}});
    auto wiz = ReadJsonFile(MetaPath(root, "wizard"), json::object());
    auto save_wiz = [&] { WriteJsonFile(MetaPath(root, "wizard"), wiz); };
    auto record_change = [&](const std::string& kind, const json& detail) {
        led["changes"].push_back({{"id", Rid("chg")}, {"kind", kind}, {"detail", detail}, {"at", NowMs()}, {"by", t_principal}}); WriteJsonFile(MetaPath(root, "system-agent"), led); };
    auto activate = [&](const std::string& provider, const std::string& model) -> ExtraResult {
        std::unique_lock<std::mutex> l(g_setup_mu, std::try_to_lock);
        if (!l.owns_lock()) return Fail("setup activation already in progress");
        auto v = SetupVerify(d, provider, model);
        if (!v.value("ok", false)) return Fail("verification failed; nothing saved: " + v["error"].dump());
        const auto ref = model.empty() ? provider : provider + "/" + model;
        auto r = GatewayRpc("config.patch", {{"agents", {{"defaults", {{"model", ref}}}}}});
        if (!r.value("ok", false)) return Fail("verified but saving config failed");
        if (d.router && d.router->Has(provider)) d.router->SetDefault(provider);
        record_change("model.activate", {{"model", ref}});
        return Ok({{"activated", true}, {"model", ref}, {"verify", v}});
    };
    // ---- wizard engine: a session is an ordered list of prompts; answers accumulate until done.
    auto step_view = [&](const json& w) -> json {
        json out{{"sessionId", w["sessionId"]}, {"status", w["status"]}, {"done", w["status"] != "running"}};
        if (w["status"] == "running") { auto s = w["steps"][w["index"].get<size_t>()]; if (s.value("sensitive", false)) s["sensitive"] = true; out["step"] = s; }
        if (w.contains("result")) out["result"] = w["result"];
        if (w.contains("error")) out["error"] = w["error"];
        return out;
    };
    auto new_wizard = [&](const std::string& kind, json steps, const json& ctx) {
        auto id = str("sessionId").empty() ? Rid("wiz") : str("sessionId");
        wiz[id] = {{"sessionId", id}, {"kind", kind}, {"steps", steps}, {"index", 0}, {"answers", json::object()}, {"status", "running"}, {"context", ctx}, {"createdAt", NowMs()}, {"owner", t_principal}};
        save_wiz(); return id;
    };
    auto finish = [&](json& w) {  // apply a completed wizard
        const auto kind = w["kind"].get<std::string>(); auto a = w["answers"]; auto ctx = w["context"];
        try {
            if (kind == "provider-auth" || kind == "prepare") {
                const auto prov = ctx.value("provider", a.value("provider", std::string()));
                json patch{{"models", {{"providers", {{prov, json::object()}}}}}};
                if (a.contains("apiKey") && !a["apiKey"].get<std::string>().empty()) patch["models"]["providers"][prov]["apiKey"] = a["apiKey"];
                if (a.contains("baseUrl") && !a["baseUrl"].get<std::string>().empty()) patch["models"]["providers"][prov]["baseUrl"] = a["baseUrl"];
                auto r = GatewayRpc("config.patch", patch);
                if (!r.value("ok", false)) throw std::runtime_error("config save failed");
                record_change("provider.auth", {{"provider", prov}, {"credentialsSaved", patch["models"]["providers"][prov].contains("apiKey")}});
                w["result"] = {{"provider", prov}, {"saved", true}, {"restartRequired", !(d.router && d.router->Has(prov))}};
                if (kind == "prepare") w["result"]["preparedModel"] = prov + (ctx.value("model", std::string()).empty() ? "" : "/" + ctx.value("model", std::string()));
            } else if (kind == "mcp-oauth" || kind == "model-oauth") {
                auto tok = oauthflow::Exchange(ctx, a.value("authorize", std::string()));
                const auto name = ctx["target"].get<std::string>();
                if (kind == "mcp-oauth") {
                    std::lock_guard<std::mutex> l(g_mu); auto db = ReadJsonFile(MetaPath(root, "mcp-oauth-tokens"), json::object()); db[name] = tok; WriteJsonFile(MetaPath(root, "mcp-oauth-tokens"), db);
                    // prove the credential works: MCP initialize against the server with the bearer token
                    auto r = GitHubRequest(ctx["resource"], "POST", tok["access_token"], json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                           {"params", {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()}, {"clientInfo", {{"name", "openclaw"}, {"version", "cpp"}}}}}}.dump());
                    record_change("mcp.oauth", {{"server", name}});
                    w["result"] = {{"serverName", name}, {"authorized", true}, {"verified", r.status == 200}, {"httpStatus", r.status}};
                } else {
                    { std::lock_guard<std::mutex> l(g_mu); auto db = ReadJsonFile(MetaPath(root, "model-oauth-profiles"), json::object());
                      db[name + ":oauth"] = tok; db[name + ":oauth"]["provider"] = name; db[name + ":oauth"]["agentId"] = ctx.value("agentId", "main"); WriteJsonFile(MetaPath(root, "model-oauth-profiles"), db); }
                    auto r = GatewayRpc("config.patch", {{"models", {{"providers", {{name, {{"apiKey", tok["access_token"]}}}}}}}});
                    if (!r.value("ok", false)) throw std::runtime_error("config save failed");
                    record_change("provider.oauth", {{"provider", name}});
                    w["result"] = {{"provider", name}, {"profileId", name + ":oauth"}, {"saved", true}, {"hasRefreshToken", tok.contains("refresh_token")}};
                }
            } else if (kind == "activate") {
                auto r = activate(ctx.value("provider", a.value("provider", std::string())), ctx.value("model", a.value("model", std::string())));
                if (!r.ok) throw std::runtime_error(r.error);
                w["result"] = r.payload;
            } else {  // onboard: workspace + agent name + model choice
                json patch = json::object();
                if (a.contains("workspace") && !a["workspace"].get<std::string>().empty()) patch["agents"]["defaults"]["workspace"] = a["workspace"];
                if (a.contains("model") && !a["model"].get<std::string>().empty()) patch["agents"]["defaults"]["model"] = a["model"];
                if (!patch.empty()) { auto r = GatewayRpc("config.patch", patch); if (!r.value("ok", false)) throw std::runtime_error("config save failed"); record_change("onboard", patch); }
                w["result"] = {{"applied", patch}};
            }
            w["status"] = "done";
        } catch (const std::exception& e) { w["status"] = "error"; w["error"] = e.what(); }
    };
    auto provider_steps = [&](const std::string& prov) {
        std::string env; for (auto& h : kProviderHints) if (prov == h.provider) env = h.env;
        return json::array({{{"id", "apiKey"}, {"type", "text"}, {"sensitive", true}, {"message", "API key for " + prov + (env.empty() ? "" : " (or set " + env + ")")}},
                            {{"id", "baseUrl"}, {"type", "text"}, {"optional", true}, {"message", "Custom base URL (blank for default)"}}});
    };
    if (m == "mcp.authLogin" || m == "models.authLogin") {
        if (!t_full_write) return Fail(m == "mcp.authLogin" ? "Connector sign-in requires an administrator connection." : "Provider login requires an administrator connection.");
        if (str("sessionId").empty()) return Fail("sessionId is required");
        if (wiz.contains(str("sessionId")) && wiz[str("sessionId")]["status"] == "running") return Fail("setup wizard session already running: " + str("sessionId"));
        const auto host = d.config.gateway_host.empty() || d.config.gateway_host == "0.0.0.0" ? std::string("127.0.0.1") : d.config.gateway_host;
        const auto redirect = "http://" + host + ":" + std::to_string(d.config.gateway_port) + "/oauth/callback";
        std::string target, resource; json oauth;
        if (m == "mcp.authLogin") {
            target = str("serverName"); if (target.empty()) return Fail("serverName is required");
            auto sv = config::GetPath(d.config.raw, "mcp.servers." + target);
            if (!sv || !sv->is_object() || !sv->contains("url")) return Fail("This connector cannot use operator browser sign-in. Check its existing account settings.");
            resource = (*sv)["url"]; oauth = sv->value("oauth", json::object());
        } else {
            const auto choice = str("authChoice"); if (choice.empty()) return Fail("authChoice is required");
            target = choice.substr(0, choice.find_first_of(":/"));
            auto pv = config::GetPath(d.config.raw, "models.providers." + target);
            if (!pv || !pv->is_object() || !pv->contains("oauth")) return Fail("That provider login is no longer available. Refresh Models and choose an available sign-in option.");
            oauth = (*pv)["oauth"];
        }
        json ep, b;
        try { ep = oauthflow::Resolve(oauth, resource, redirect); b = oauthflow::Begin(ep, redirect, resource); } catch (const std::exception& e) { return Fail(std::string("sign-in could not start: ") + e.what()); }
        json steps = json::array({{{"id", "authorize"}, {"type", "openUrl"}, {"url", b["url"]}, {"message", "Open the sign-in page. The gateway completes automatically at " + redirect + ", or paste the final redirected URL here."}}});
        auto id = new_wizard(m == "mcp.authLogin" ? "mcp-oauth" : "model-oauth", steps,
                             {{"target", target}, {"resource", resource}, {"redirectUri", redirect}, {"state", b["state"]}, {"verifier", b["verifier"]}, {"endpoints", ep}, {"agentId", str("agentId")}});
        auto v = step_view(wiz[id]); v["step"].erase("sensitive"); return Ok(v);
    }
    if (m == "wizard.start") {
        const auto mode = str("mode").empty() ? (str("flow").empty() ? std::string("onboard") : str("flow")) : str("mode");
        json steps = json::array({{{"id", "workspace"}, {"type", "text"}, {"optional", true}, {"message", "Workspace directory"}},
                                  {{"id", "model"}, {"type", "select"}, {"optional", true}, {"message", "Default model"}, {"options", json::array()}}});
        const json det = SetupDetect(d);
        for (auto& c : det["candidates"]) steps[1]["options"].push_back(c.value("provider", "") + (c.value("model", "").empty() ? "" : "/" + c.value("model", "")));
        if (mode == "provider-auth") steps = provider_steps(str("provider"));
        auto id = new_wizard(mode, steps, {{"provider", str("provider")}});
        return Ok(step_view(wiz[id]));
    }
    if (m == "openclaw.setup.auth.start" || m == "openclaw.setup.prepare.start" || m == "openclaw.setup.activate.start") {
        if (!str("sessionId").empty() && wiz.contains(str("sessionId")) && wiz[str("sessionId")]["status"] == "running") return Fail("setup wizard session already running: " + str("sessionId"));
        auto prov = str("provider"); const auto choice = str("authChoice");
        if (prov.empty() && !choice.empty()) prov = choice.substr(0, choice.find('-'));
        if (prov.empty()) return Fail("provider or authChoice is required");
        const auto kind = m == "openclaw.setup.auth.start" ? "provider-auth" : m == "openclaw.setup.prepare.start" ? "prepare" : "activate";
        json steps = std::string(kind) == "activate" ? json::array({{{"id", "confirm"}, {"type", "confirm"}, {"message", "Test and activate " + prov + (str("model").empty() ? "" : "/" + str("model")) + "?"}}}) : provider_steps(prov);
        auto id = new_wizard(kind, steps, {{"provider", prov}, {"model", str("model")}, {"workspace", str("workspace")}, {"agentId", str("agentId")}});
        return Ok(step_view(wiz[id]));
    }
    if (m.rfind("wizard.", 0) == 0) {
        std::string id = str("sessionId");
        if (id.empty() && !str("oauthState").empty()) {  // GET /oauth/callback: the state value names the running sign-in
            for (auto& [k, w] : wiz.items()) if (w["status"] == "running" && w["context"].value("state", "") == str("oauthState")) id = k;
            if (id.empty()) return Fail("unknown or finished sign-in");
        }
        if (!wiz.contains(id)) return Fail("unknown wizard session: " + id);
        auto& w = wiz[id];
        if (m == "wizard.status") return Ok(step_view(w));
        if (m == "wizard.cancel") { if (w["status"] == "running") { w["status"] = "cancelled"; save_wiz(); } return Ok(step_view(w)); }
        if (w["status"] != "running") return Fail("wizard session is " + w["status"].get<std::string>());
        auto& step = w["steps"][w["index"].get<size_t>()];
        json ans = p.contains("answer") ? p["answer"] : p.value("value", json());
        if (ans.is_object() && ans.contains("value")) { if (ans.contains("stepId") && ans["stepId"] != step["id"]) return Fail("answer is for a different step"); ans = ans["value"]; }
        if (step["type"] == "confirm") { if (!ans.is_boolean()) return Fail("confirm step needs a boolean"); if (!ans.get<bool>()) { w["status"] = "cancelled"; save_wiz(); return Ok(step_view(w)); } }
        else { if (ans.is_null()) ans = ""; if (!ans.is_string()) return Fail("text answer expected");
            if (ans.get<std::string>().empty() && !step.value("optional", false)) return Fail("an answer is required"); }
        w["answers"][step["id"].get<std::string>()] = ans;
        w["index"] = w["index"].get<int>() + 1;
        if (w["index"].get<size_t>() >= w["steps"].size()) { save_wiz(); finish(w); }
        save_wiz();
        return Ok(step_view(w));
    }
    if (m == "openclaw.setup.detect") return Ok(SetupDetect(d));
    if (m == "openclaw.setup.verify") { if (str("provider").empty()) return Fail("provider is required"); return Ok(SetupVerify(d, str("provider"), str("model"))); }
    if (m == "openclaw.setup.activate") { if (str("provider").empty()) return Fail("provider is required"); return activate(str("provider"), str("model")); }
    if (m == "openclaw.changes.list") return Ok({{"changes", led["changes"]}});
    if (m == "openclaw.approval.list") {
        json out = led["approvals"]; auto ex = GatewayRpc("exec.approvals.list", json::object());
        if (ex.value("ok", false)) for (auto& a : ex["payload"].value("pending", json::array())) out.push_back({{"kind", "exec"}, {"approval", a}});
        return Ok({{"approvals", out}});
    }
    if (m == "openclaw.chat.history") {
        const size_t lim = std::clamp(p.value("limit", 50), 1, 500);
        auto ms = d.sessions.Exists(kSys) ? d.sessions.Load(kSys) : std::vector<agent::Message>{};
        json turns = json::array(); for (size_t i = ms.size() > lim ? ms.size() - lim : 0; i < ms.size(); ++i) if (ms[i].role == "user" || ms[i].role == "assistant") turns.push_back({{"role", ms[i].role}, {"text", ms[i].content}});
        return Ok({{"turns", turns}});
    }
    if (m == "openclaw.chat") {
        if (p.value("reset", false)) d.sessions.Remove(kSys);
        if (p.contains("wizardAnswer") || p.value("wizardCancel", false)) {
            auto r = GatewayRpc(p.value("wizardCancel", false) ? "wizard.cancel" : "wizard.next", {{"sessionId", str("sessionId")}, {"answer", p.value("wizardAnswer", json())}});
            return r.value("ok", false) ? Ok({{"wizard", r["payload"]}}) : Fail(r.value("error", json("wizard failed")).dump());
        }
        if (str("message").empty()) return Ok({{"turns", json::array()}, {"welcome", "I'm OpenClaw. I can detect providers, verify a model and change settings."}});
        auto r = GatewayRpc("chat.send", {{"sessionKey", kSys}, {"message", str("message")}});
        return r.value("ok", false) ? Ok({{"reply", r["payload"].value("text", "")}, {"sessionKey", kSys}}) : Fail("system agent turn failed");
    }
    return {};
}


// ---------------------------------------------------------------- portals: token-gated TCP reverse proxy from a listen port to a local service port
struct PortalRt { json info; int lfd = -1; std::atomic<bool> stop{false}; std::string token; std::string owner; };
std::mutex g_portal_mu;
std::map<std::string, std::shared_ptr<PortalRt>> g_portals;
void PortalSplice(int a, int b) {
    pollfd fds[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}}; char buf[16384];
    for (;;) {
        if (poll(fds, 2, 30000) <= 0) break;
        bool done = false;
        for (int i = 0; i < 2 && !done; ++i) if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            auto n = ::recv(fds[i].fd, buf, sizeof buf, 0); if (n <= 0) { done = true; break; }
            for (ssize_t off = 0; off < n;) { auto w = ::send(fds[1 - i].fd, buf + off, n - off, MSG_NOSIGNAL); if (w <= 0) { done = true; break; } off += w; }
        }
        if (done) break;
    }
}
void PortalConn(std::shared_ptr<PortalRt> rt, int c) {
    std::string head; char ch[4096];
    while (head.find("\r\n\r\n") == std::string::npos && head.size() < 65536) { auto n = ::recv(c, ch, sizeof ch, 0); if (n <= 0) { ::close(c); return; } head.append(ch, n); }
    const auto line = head.substr(0, head.find("\r\n"));
    const bool authed = line.find("oc_portal=" + rt->token) != std::string::npos || head.find("oc_portal=" + rt->token) != std::string::npos ||
                        head.find("X-OpenClaw-Portal-Token: " + rt->token) != std::string::npos;
    if (!authed) { const char* r = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 13\r\nConnection: close\r\n\r\nportal token\n"; ::send(c, r, strlen(r), MSG_NOSIGNAL); ::close(c); return; }
    int u = ::socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(rt->info["port"].get<int>()); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(u, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { const char* r = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; ::send(c, r, strlen(r), MSG_NOSIGNAL); ::close(u); ::close(c); return; }
    // Strip the portal token from the forwarded request line so the upstream never sees it.
    auto q = head.find("oc_portal=" + rt->token);
    if (q != std::string::npos && q < line.size()) { auto e = q + 10 + rt->token.size(); if (q > 0 && (head[q - 1] == '&')) --q; else if (e < head.size() && head[e] == '&') ++e; head.erase(q, e - q); auto l2 = head.find("\r\n"); auto qs = head.rfind('?', l2); if (qs != std::string::npos && (head[qs + 1] == ' ')) head.erase(qs, 1); }
    ::send(u, head.data(), head.size(), MSG_NOSIGNAL);
    PortalSplice(c, u); ::close(u); ::close(c);
}
json PortalList(const std::string& owner) {
    std::lock_guard<std::mutex> l(g_portal_mu); json out = json::array();
    for (auto& [id, rt] : g_portals) if (owner.empty() || rt->owner == owner) out.push_back(rt->info);
    return out;
}
ExtraResult Portals(ServerDeps& d, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    const bool sess = m.rfind("portal.session.", 0) == 0;
    if (sess && (str("sessionKey").empty() || str("environmentId").empty())) return Fail("sessionKey and environmentId are required");
    const auto owner = sess ? str("sessionKey") : std::string();
    if (!str("environmentId").empty() && str("environmentId") != "local") return Fail("unknown environment: " + str("environmentId"));
    auto changed = [&] { GatewayEmit("portal.changed", {{"portals", PortalList("")}}); };
    if (m == "portal.list" || m == "portal.session.list") return Ok({{"portals", PortalList(owner)}});
    if (m == "portal.open" || m == "portal.session.open") {
        if (!p.contains("port") || !p["port"].is_number_integer() || p["port"].get<int>() < 1 || p["port"].get<int>() > 65535) return Fail("port must be an integer 1-65535");
        if (!str("path").empty() && str("path")[0] != '/') return Fail("path must start with /");
        int fd = ::socket(AF_INET, SOCK_STREAM, 0); int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = 0;
        const auto host = d.config.gateway_host.empty() ? std::string("127.0.0.1") : d.config.gateway_host;
        inet_pton(AF_INET, host == "0.0.0.0" || host == "localhost" ? (host == "localhost" ? "127.0.0.1" : "0.0.0.0") : host.c_str(), &a.sin_addr);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 64) != 0) { ::close(fd); return Fail("cannot open portal listener"); }
        socklen_t sl = sizeof a; ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &sl);
        auto rt = std::make_shared<PortalRt>(); rt->lfd = fd; rt->owner = owner;
        unsigned char rb[16]; std::ifstream("/dev/urandom", std::ios::binary).read(reinterpret_cast<char*>(rb), 16);
        static const char* hx = "0123456789abcdef"; for (auto b : rb) { rt->token.push_back(hx[b >> 4]); rt->token.push_back(hx[b & 15]); }
        const int lp = ntohs(a.sin_port); const auto id = Rid("portal");
        const auto shown = host == "0.0.0.0" ? std::string("127.0.0.1") : host;
        const auto path = str("path").empty() ? std::string("/") : str("path");
        rt->info = {{"id", id}, {"title", str("title").empty() ? "Port " + std::to_string(p["port"].get<int>()) : str("title")}, {"port", p["port"]}, {"listenPort", lp},
                    {"publicUrl", "http://" + shown + ":" + std::to_string(lp) + path}, {"path", path}, {"createdAtMs", NowMs()}, {"origin", sess ? "session" : "operator"}};
        if (!str("description").empty()) rt->info["description"] = str("description");
        json out = rt->info; out["tokenQuery"] = "oc_portal=" + rt->token; out["url"] = rt->info["publicUrl"].get<std::string>() + (path.find('?') == std::string::npos ? "?" : "&") + "oc_portal=" + rt->token;
        { std::lock_guard<std::mutex> l(g_portal_mu); g_portals[id] = rt; }
        std::thread([rt] { while (!rt->stop) { int c = ::accept(rt->lfd, nullptr, nullptr); if (c < 0) { if (rt->stop) break; continue; } std::thread(PortalConn, rt, c).detach(); } }).detach();
        changed();
        return Ok(out);
    }
    if (m == "portal.close" || m == "portal.session.close") {
        std::shared_ptr<PortalRt> rt;
        { std::lock_guard<std::mutex> l(g_portal_mu); auto it = g_portals.find(str("id")); if (it == g_portals.end() || (sess && it->second->owner != owner)) return Ok({{"closed", false}}); rt = it->second; g_portals.erase(it); }
        rt->stop = true; ::shutdown(rt->lfd, SHUT_RDWR); ::close(rt->lfd);
        changed();
        return Ok({{"closed", true}});
    }
    return {};
}

// ---------------------------------------------------------------- doctor.memory dream diary (DREAMS.md) maintenance, ported from memory-core
namespace dream {
const std::string kStart = "<!-- openclaw:dreaming:diary:start -->", kEnd = "<!-- openclaw:dreaming:diary:end -->", kBackfill = "openclaw:dreaming:backfill-entry";
std::string Ensure(const std::string& s) {
    if (s.find(kStart) != std::string::npos && s.find(kEnd) != std::string::npos) return s;
    std::string sec = "# Dream Diary\n\n" + kStart + "\n" + kEnd + "\n";
    return Trim(s).empty() ? sec : sec + "\n" + s;
}
std::optional<std::vector<std::string>> Blocks(const std::string& s) {
    auto a = s.find(kStart), b = s.find(kEnd); if (a == std::string::npos || b == std::string::npos || b < a) return std::nullopt;
    std::string body = s.substr(a + kStart.size(), b - a - kStart.size()); std::vector<std::string> out;
    size_t pos = 0; for (;;) { auto n = body.find("\n---\n", pos); auto blk = Trim(body.substr(pos, n == std::string::npos ? std::string::npos : n - pos)); if (!blk.empty()) out.push_back(blk); if (n == std::string::npos) break; pos = n + 5; }
    // a leading "---" on the first block (joinDiaryBlocks format) is a separator, not content
    for (auto& x : out) if (x.rfind("---", 0) == 0) x = Trim(x.substr(3));
    out.erase(std::remove_if(out.begin(), out.end(), [](auto& x) { return x.empty(); }), out.end());
    return out;
}
std::string Join(const std::vector<std::string>& bl) { std::string o; for (size_t i = 0; i < bl.size(); ++i) { if (i) o += "\n"; o += "---\n\n" + Trim(bl[i]) + "\n"; } return o; }
std::string Replace(const std::string& s, const std::string& content) {
    auto e = Ensure(s); auto a = e.find(kStart), b = e.find(kEnd);
    return e.substr(0, a + kStart.size()) + (Trim(content).empty() ? "\n" : "\n" + Trim(content) + "\n") + e.substr(b);
}
std::string Fingerprint(const std::string& blk) {
    std::istringstream ss(blk); std::string l, date, body;
    while (std::getline(ss, l)) { l = Trim(l); if (l.empty() || l.rfind("<!--", 0) == 0 || l[0] == '#') continue;
        if (date.empty() && l.size() > 2 && l.front() == '*' && l.back() == '*') { date = Trim(l.substr(1, l.size() - 2)); continue; } body += l + "\n"; }
    return date + "\n" + body;
}
std::string DayLabel(const std::string& iso) {
    static const char* mon[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    if (iso.size() != 10) return iso; int mo = std::atoi(iso.substr(5, 2).c_str()); if (mo < 1 || mo > 12) return iso;
    return std::string(mon[mo - 1]) + " " + std::to_string(std::atoi(iso.substr(8, 2).c_str())) + ", " + iso.substr(0, 4);
}
}  // namespace dream
ExtraResult DoctorMemory(ServerDeps& d, const std::string& m, const json& p) {
    const auto agent = p.value("agentId", std::string("main"));
    const auto ws = WorkspaceOf(d, agent); std::error_code ec;
    fs::path dp = ws / "DREAMS.md"; std::string name = "DREAMS.md";
    if (!fs::is_regular_file(dp, ec) && fs::is_regular_file(ws / "dreams.md", ec)) { dp = ws / "dreams.md"; name = "dreams.md"; }
    const bool symlink = fs::is_symlink(dp, ec);
    auto read = [&]() -> std::string { if (symlink) return {}; std::ifstream i(dp); return std::string(std::istreambuf_iterator<char>(i), {}); };
    auto info = [&]() { json o{{"agentId", agent}, {"path", name}, {"found", !symlink && fs::is_regular_file(dp, ec)}};
        if (o["found"]) { o["content"] = read(); o["updatedAtMs"] = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(fs::last_write_time(dp, ec).time_since_epoch()).count()); } return o; };
    auto write = [&](const std::string& s) { if (symlink) throw std::runtime_error("refusing to write a redirected diary"); std::ofstream(dp) << s; };
    try {
        if (m == "doctor.memory.dreamDiary") return Ok(info());
        if (m == "doctor.memory.backfillDreamDiary") {
            std::vector<fs::path> files; static const std::regex re(R"(^(\d{4}-\d{2}-\d{2})(?:-[^/]+)?\.md$)", std::regex::icase);
            for (auto it = fs::directory_iterator(ws / "memory", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) if (std::regex_match(it->path().filename().string(), re)) files.push_back(it->path());
            std::sort(files.begin(), files.end());
            auto base = info(); if (files.empty()) return Ok({{"path", name}, {"found", base["found"]}, {"scannedFiles", 0}, {"written", 0}, {"replaced", 0}});
            auto cur = dream::Ensure(read()); auto blocks = dream::Blocks(cur).value_or(std::vector<std::string>{});
            int replaced = 0; std::vector<std::string> keep; for (auto& b : blocks) { if (b.find(dream::kBackfill) != std::string::npos) ++replaced; else keep.push_back(b); }
            int written = 0;
            for (auto& f : files) {
                std::smatch mm; auto fn = f.filename().string(); std::regex_match(fn, mm, re); const auto day = mm[1].str();
                std::ifstream in(f); std::string l, body; int n = 0;
                while (std::getline(in, l) && n < 12) { auto tl = Trim(l); if (tl.empty() || tl[0] == '#' || tl.rfind("<!--", 0) == 0) continue; body += (tl.rfind("- ", 0) == 0 ? tl : "- " + tl) + "\n"; ++n; }
                if (body.empty()) continue;
                keep.push_back("*" + dream::DayLabel(day) + "*\n\n<!-- " + dream::kBackfill + " day=" + day + " source=memory/" + fn + " -->\n\n" + Trim(body)); ++written;
            }
            write(dream::Replace(cur, dream::Join(keep)));
            return Ok({{"agentId", agent}, {"path", name}, {"found", true}, {"scannedFiles", files.size()}, {"written", written}, {"replaced", replaced}});
        }
        if (m == "doctor.memory.resetDreamDiary" || m == "doctor.memory.dedupeDreamDiary") {
            auto cur = read(); auto e = dream::Ensure(cur); auto blocks = dream::Blocks(e).value_or(std::vector<std::string>{});
            std::vector<std::string> keep; std::set<std::string> seen;
            for (auto& b : blocks) { if (m == "doctor.memory.resetDreamDiary") { if (b.find(dream::kBackfill) == std::string::npos) keep.push_back(b); } else if (seen.insert(dream::Fingerprint(b)).second) keep.push_back(b); }
            const int removed = static_cast<int>(blocks.size() - keep.size());
            if (removed > 0 || (m == "doctor.memory.resetDreamDiary" && !cur.empty())) write(dream::Replace(e, dream::Join(keep)));
            json o{{"agentId", agent}, {"path", name}, {"found", info()["found"]}, {"removedEntries", removed}};
            if (m == "doctor.memory.dedupeDreamDiary") { o["dedupedEntries"] = removed; o["keptEntries"] = keep.size(); }
            return Ok(o);
        }
        const auto dreams_dir = ws / "memory" / ".dreams";
        if (m == "doctor.memory.resetGroundedShortTerm") {
            auto f = dreams_dir / "short-term-recall.json"; auto j = ReadJsonFile(f, json());
            int removed = 0;
            if (j.is_object() && j.contains("entries")) {
                json& en = j["entries"]; json keep = en.is_array() ? json::array() : json::object();
                if (en.is_array()) { for (auto& x : en) { if (x.value("source", "") == "grounded" || x.value("grounded", false)) ++removed; else keep.push_back(x); } }
                else for (auto& [k, x] : en.items()) { if (x.value("source", "") == "grounded" || x.value("grounded", false)) ++removed; else keep[k] = x; }
                if (removed) { en = keep; WriteJsonFile(f, j); }
            }
            return Ok({{"agentId", agent}, {"removedShortTermEntries", removed}});
        }
        if (m == "doctor.memory.repairDreamingArtifacts") {
            const auto arch = dreams_dir / ("repair-" + std::to_string(NowMs())); json warnings = json::array(); bool changed = false;
            bool ad = false, ac = false, ai = false;
            auto archive = [&](const fs::path& src, bool& flag) { fs::create_directories(arch, ec); fs::rename(src, arch / src.filename(), ec); if (ec) warnings.push_back("could not archive " + src.string() + ": " + ec.message()); else { flag = true; changed = true; } };
            if (fs::is_regular_file(dp, ec) && !symlink) { auto s = read(); auto a = s.find(dream::kStart), b = s.find(dream::kEnd);
                if ((a == std::string::npos) != (b == std::string::npos) || (a != std::string::npos && b < a)) archive(dp, ad); }
            for (auto [fname, flag] : std::vector<std::pair<std::string, bool*>>{{"session-corpus", &ac}, {"session-ingestion.json", &ai}}) {
                auto f = dreams_dir / fname; if (!fs::exists(f, ec)) continue;
                if (fs::is_regular_file(f, ec)) { try { std::ifstream i(f); (void)json::parse(i); continue; } catch (...) {} archive(f, *flag); }
                else if (fs::is_directory(f, ec)) { bool bad = false; for (auto& e : fs::directory_iterator(f, ec)) if (e.path().extension() == ".jsonl") { std::ifstream i(e.path()); std::string l; while (std::getline(i, l)) if (!Trim(l).empty()) { try { (void)json::parse(l); } catch (...) { bad = true; } } } if (bad) archive(f, *flag); }
            }
            json o{{"agentId", agent}, {"changed", changed}, {"archivedDreamsDiary", ad}, {"archivedSessionCorpus", ac}, {"archivedSessionIngestion", ai}, {"warnings", warnings}};
            if (changed) o["archiveDir"] = arch.string();
            return Ok(o);
        }
    } catch (const std::exception& e) { return Fail(std::string("memory action failed: ") + e.what()); }
    return {};
}

// ---------------------------------------------------------------- gateway suspension (drain / ready / handoff), stop.request, diagnostics
struct SuspendState { std::string id, owner, phase = "running", terminal; long long expires = 0; bool committed = false; };
std::mutex g_susp_mu; SuspendState g_susp;
bool GatewayAdmissionClosed(std::string* why) {
    std::lock_guard<std::mutex> l(g_susp_mu);
    if (g_susp.phase == "running") return false;
    if (g_susp.expires && NowMs() > g_susp.expires && !g_susp.committed) { g_susp = SuspendState{}; return false; }
    if (why) *why = "gateway is suspending (" + g_susp.phase + "); retry after resume";
    return true;
}
json SuspendBlockers() {
    json b = json::array();
    auto tl = GatewayRpc("terminal.list", json::object());
    if (tl.value("ok", false)) for (auto& s : tl["payload"].value("sessions", json::array())) if (s.value("running", false)) b.push_back({{"kind", "background-exec"}, {"id", s.value("sessionId", "")}, {"detail", "terminal"}});
    auto pr = GatewayRpc("sessions.processes", json::object());
    if (pr.value("ok", false)) for (auto& s : pr["payload"].value("processes", json::array())) if (s.value("running", s.value("status", "") == "running")) b.push_back({{"kind", "background-exec"}, {"id", s.value("id", s.value("sessionId", ""))}});
    return b;
}
json ProcThreads() {
    json out = json::array(); std::error_code ec;
    for (auto& e : fs::directory_iterator("/proc/self/task", ec)) {
        std::ifstream st(e.path() / "stat"); std::string s((std::istreambuf_iterator<char>(st)), {});
        auto rp = s.rfind(')'); if (rp == std::string::npos) continue;
        std::istringstream rest(s.substr(rp + 2)); std::vector<std::string> f; std::string x; while (rest >> x) f.push_back(x);
        std::ifstream cn(e.path() / "comm"); std::string comm; std::getline(cn, comm);
        if (f.size() > 12) out.push_back({{"tid", std::atoi(e.path().filename().c_str())}, {"name", comm}, {"state", f[0]}, {"utimeTicks", std::atoll(f[11].c_str())}, {"stimeTicks", std::atoll(f[12].c_str())}});
    }
    return out;
}
json ProcStatusKb() {
    json o = json::object(); std::ifstream i("/proc/self/status"); std::string l;
    while (std::getline(i, l)) for (auto k : {"VmRSS", "VmHWM", "VmSize", "RssAnon", "Threads"}) if (l.rfind(std::string(k) + ":", 0) == 0) o[k] = std::atoll(l.substr(l.find(':') + 1).c_str());
    return o;
}
ExtraResult Lifecycle(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    const long long ttl = 5 * 60 * 1000;
    if (m == "gateway.suspend.prepare") {
        if (str("requestId").empty()) return Fail("requestId is required");
        auto blockers = SuspendBlockers();
        std::lock_guard<std::mutex> l(g_susp_mu);
        if (g_susp.phase != "running" && g_susp.owner != str("requestId")) return Ok({{"status", "busy"}, {"reason", "gateway-draining"}, {"retryAfterMs", 1000}, {"activeCount", blockers.size()}, {"blockers", blockers}});
        if (!blockers.empty() && !p.value("drain", false) && str("terminalPolicy") != "terminate")
            return Ok({{"status", "busy"}, {"reason", "active-work"}, {"retryAfterMs", 1000}, {"activeCount", blockers.size()}, {"blockers", blockers}});
        if (g_susp.id.empty()) g_susp.id = Rid("susp");
        g_susp.owner = str("requestId"); g_susp.terminal = str("terminalPolicy"); g_susp.expires = NowMs() + ttl;
        if (str("terminalPolicy") == "terminate") for (auto& b : blockers) if (b.value("detail", "") == "terminal") GatewayRpc("terminal.close", {{"sessionId", b["id"]}});
        const bool ready = blockers.empty() || str("terminalPolicy") == "terminate";
        g_susp.phase = ready ? "ready" : "draining";
        GatewayEmit("gateway.suspend", {{"status", g_susp.phase}, {"suspensionId", g_susp.id}});
        json o{{"status", g_susp.phase}, {"suspensionId", g_susp.id}, {"expiresAtMs", g_susp.expires}, {"activeCount", ready ? 0 : blockers.size()}, {"blockers", ready ? json::array() : blockers}};
        if (!ready) o["retryAfterMs"] = 1000;
        return Ok(o);
    }
    if (m == "gateway.suspend.status" || m == "gateway.suspend.resume" || m == "gateway.suspend.handoff") {
        std::unique_lock<std::mutex> l(g_susp_mu);
        if (g_susp.phase == "running") { if (m == "gateway.suspend.resume") return Ok({{"ok", true}, {"status", "running"}, {"resumed", false}}); if (m == "gateway.suspend.status") return Ok({{"status", "running"}}); return Fail("gateway is not suspended"); }
        if (str("suspensionId") != g_susp.id) return Fail("unknown or stale suspensionId");
        if (m == "gateway.suspend.resume") { if (g_susp.committed) return Fail("handoff committed; the gateway is exiting"); g_susp = SuspendState{}; l.unlock(); GatewayEmit("gateway.suspend", {{"status", "running"}}); return Ok({{"ok", true}, {"status", "running"}, {"resumed", true}}); }
        if (m == "gateway.suspend.status") {
            if (g_susp.phase == "draining") { l.unlock(); auto b = SuspendBlockers(); l.lock(); if (b.empty()) g_susp.phase = "ready";
                if (g_susp.phase == "draining") return Ok({{"status", "draining"}, {"ownerId", g_susp.owner}, {"phase", "draining"}, {"expiresAtMs", g_susp.expires}, {"retryAfterMs", 1000}, {"activeCount", b.size()}, {"blockers", b}}); }
            return Ok({{"status", "ready"}, {"ownerId", g_susp.owner}, {"expiresAtMs", g_susp.expires}});
        }
        if (g_susp.phase != "ready") return Fail("gateway is still draining");
        if (!p.contains("target") || p["target"].value("pid", 0) != static_cast<int>(getpid())) return Fail("handoff target does not match this gateway process");
        if (p.value("commit", false)) { g_susp.committed = true; g_susp.phase = "exiting"; }
        return Ok({{"status", g_susp.committed ? "committed" : "armed"}, {"suspensionId", g_susp.id}, {"expiresAtMs", g_susp.expires}});
    }
    if (m == "gateway.stop.request") {
        const int delay = std::clamp(p.value("delayMs", 250), 0, 60000);
        ReadJsonFile(MetaPath(root, "stop-requests"), json::array());
        auto led = ReadJsonFile(MetaPath(root, "stop-requests"), json::array()); led.push_back({{"at", NowMs()}, {"by", t_principal}, {"reason", str("reason")}}); WriteJsonFile(MetaPath(root, "stop-requests"), led);
        GatewayEmit("shutdown", {{"reason", str("reason").empty() ? std::string("stop.request") : str("reason")}, {"restartExpectedMs", nullptr}});
        std::thread([delay] { std::this_thread::sleep_for(std::chrono::milliseconds(delay)); ::kill(::getpid(), SIGTERM); }).detach();
        return Ok({{"accepted", true}, {"pid", getpid()}, {"stopInMs", delay}});
    }
    if (m == "diagnostics.lanes") return Ok({{"lanes", ProcThreads()}, {"process", ProcStatusKb()}});
    if (m == "diagnostics.cpuProfile" || m == "diagnostics.heapProfile") {
        const int dur = std::clamp(p.value("durationMs", 1000), 50, 60000);
        const auto t0 = NowMs(); auto a = ProcThreads(); json samples = json::array();
        for (int waited = 0; waited < dur; waited += std::min(100, dur)) { std::this_thread::sleep_for(std::chrono::milliseconds(std::min(100, dur))); if (m == "diagnostics.heapProfile") samples.push_back({{"t", NowMs() - t0}, {"mem", ProcStatusKb()}}); }
        if (m == "diagnostics.heapProfile") return Ok({{"durationMs", NowMs() - t0}, {"samples", samples}, {"unit", "kB"}, {"format", "openclaw-cpp.rss-samples.v1"}});
        auto b = ProcThreads(); json th = json::array(); const long hz = sysconf(_SC_CLK_TCK);
        for (auto& x : b) { long long before = 0; for (auto& y : a) if (y["tid"] == x["tid"]) before = y["utimeTicks"].get<long long>() + y["stimeTicks"].get<long long>();
            long long now = x["utimeTicks"].get<long long>() + x["stimeTicks"].get<long long>(); th.push_back({{"tid", x["tid"]}, {"name", x["name"]}, {"cpuMs", (now - before) * 1000 / hz}}); }
        return Ok({{"durationMs", NowMs() - t0}, {"threads", th}, {"format", "openclaw-cpp.thread-cpu.v1"}});
    }
    if (m == "diagnostics.heapSnapshot") {
        if (str("reason").size() > 256) return Fail("reason must be at most 256 characters");
        const auto dir = root / "diagnostics"; std::error_code ec; fs::create_directories(dir, ec);
        const auto f = dir / ("heap-" + std::to_string(NowMs()) + ".xml");
        FILE* fp = std::fopen(f.c_str(), "w"); if (!fp) return Fail("cannot write snapshot");
        malloc_info(0, fp); std::fclose(fp);
        return Ok({{"path", f.string()}, {"bytes", fs::file_size(f, ec)}, {"format", "glibc-malloc-info-xml"}, {"process", ProcStatusKb()}, {"reason", str("reason")}});
    }
    return {};
}


// ---------------------------------------------------------------- Web Push (RFC 8030/8291/8292): VAPID keys, subscriptions, aes128gcm delivery
namespace webpush {
std::string B64u(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o; int v = 0, b = -6;
    for (unsigned char c : in) { v = (v << 8) + c; b += 8; while (b >= 0) { o.push_back(t[(v >> b) & 0x3F]); b -= 6; } }
    if (b > -6) o.push_back(t[((v << 8) >> (b + 8)) & 0x3F]);
    return o;
}
std::string Hmac(const std::string& key, const std::string& data) {
    unsigned char out[32]; unsigned int n = 32;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, &n);
    return std::string(reinterpret_cast<char*>(out), n);
}
std::string RawPub(EVP_PKEY* k) {
    unsigned char buf[80]; size_t n = 0;
    if (EVP_PKEY_get_octet_string_param(k, OSSL_PKEY_PARAM_PUB_KEY, buf, sizeof buf, &n) != 1) return {};
    return std::string(reinterpret_cast<char*>(buf), n);
}
EVP_PKEY* Gen() { return EVP_EC_gen("P-256"); }
EVP_PKEY* FromPem(const std::string& pem) { BIO* b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())); EVP_PKEY* k = PEM_read_bio_PrivateKey(b, nullptr, nullptr, nullptr); BIO_free(b); return k; }
std::string ToPem(EVP_PKEY* k) { BIO* b = BIO_new(BIO_s_mem()); PEM_write_bio_PrivateKey(b, k, nullptr, nullptr, 0, nullptr, nullptr); char* d = nullptr; long n = BIO_get_mem_data(b, &d); std::string s(d, n); BIO_free(b); return s; }
EVP_PKEY* FromRawPub(const std::string& raw) {
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr); EVP_PKEY* k = nullptr;
    OSSL_PARAM ps[] = {OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, const_cast<char*>("prime256v1"), 0),
                       OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, const_cast<char*>(raw.data()), raw.size()), OSSL_PARAM_construct_end()};
    if (EVP_PKEY_fromdata_init(c) != 1 || EVP_PKEY_fromdata(c, &k, EVP_PKEY_PUBLIC_KEY, ps) != 1) k = nullptr;
    EVP_PKEY_CTX_free(c); return k;
}
std::string Ecdh(EVP_PKEY* priv, EVP_PKEY* peer) {
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(priv, nullptr); size_t n = 0; std::string out;
    if (EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, peer) == 1 && EVP_PKEY_derive(c, nullptr, &n) == 1) { out.resize(n); EVP_PKEY_derive(c, reinterpret_cast<unsigned char*>(out.data()), &n); out.resize(n); }
    EVP_PKEY_CTX_free(c); return out;
}
std::string Es256Jwt(EVP_PKEY* k, const json& claims) {
    const auto si = B64u(R"({"typ":"JWT","alg":"ES256"})") + "." + B64u(claims.dump());
    EVP_MD_CTX* m = EVP_MD_CTX_new(); size_t n = 0; std::string der;
    EVP_DigestSignInit(m, nullptr, EVP_sha256(), nullptr, k); EVP_DigestSign(m, nullptr, &n, reinterpret_cast<const unsigned char*>(si.data()), si.size());
    der.resize(n); EVP_DigestSign(m, reinterpret_cast<unsigned char*>(der.data()), &n, reinterpret_cast<const unsigned char*>(si.data()), si.size()); der.resize(n); EVP_MD_CTX_free(m);
    const unsigned char* pp = reinterpret_cast<const unsigned char*>(der.data()); ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &pp, static_cast<long>(der.size()));
    unsigned char rs[64]; BN_bn2binpad(ECDSA_SIG_get0_r(sig), rs, 32); BN_bn2binpad(ECDSA_SIG_get0_s(sig), rs + 32, 32); ECDSA_SIG_free(sig);
    return si + "." + B64u(std::string(reinterpret_cast<char*>(rs), 64));
}
// RFC 8291 aes128gcm body for one record.
std::optional<std::string> Encrypt(const std::string& ua_pub, const std::string& auth, const std::string& plaintext) {
    EVP_PKEY* peer = FromRawPub(ua_pub); if (!peer) return std::nullopt;
    EVP_PKEY* as = Gen(); const auto as_pub = RawPub(as); const auto shared = Ecdh(as, peer);
    EVP_PKEY_free(peer); EVP_PKEY_free(as); if (shared.empty()) return std::nullopt;
    std::string salt(16, '\0'); RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), 16);
    const auto prk_key = Hmac(auth, shared);
    const auto ikm = Hmac(prk_key, std::string("WebPush: info") + '\0' + ua_pub + as_pub + '\x01');
    const auto prk = Hmac(salt, ikm);
    const auto cek = Hmac(prk, std::string("Content-Encoding: aes128gcm") + '\0' + '\x01').substr(0, 16);
    const auto nonce = Hmac(prk, std::string("Content-Encoding: nonce") + '\0' + '\x01').substr(0, 12);
    const std::string pt = plaintext + '\x02';
    std::string ct(pt.size() + 16, '\0'); int l = 0, l2 = 0;
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(c, EVP_aes_128_gcm(), nullptr, reinterpret_cast<const unsigned char*>(cek.data()), reinterpret_cast<const unsigned char*>(nonce.data()));
    EVP_EncryptUpdate(c, reinterpret_cast<unsigned char*>(ct.data()), &l, reinterpret_cast<const unsigned char*>(pt.data()), static_cast<int>(pt.size()));
    EVP_EncryptFinal_ex(c, reinterpret_cast<unsigned char*>(ct.data()) + l, &l2);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, ct.data() + l + l2); EVP_CIPHER_CTX_free(c);
    ct.resize(l + l2 + 16);
    std::string hdr = salt; hdr += std::string("\x00\x00\x10\x00", 4); hdr.push_back(static_cast<char>(as_pub.size())); hdr += as_pub;
    return hdr + ct;
}
}  // namespace webpush
ExtraResult Push(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    std::lock_guard<std::mutex> l(g_mu);
    auto db = ReadJsonFile(MetaPath(root, "web-push"), json{{"subscriptions", json::array()}});
    auto save = [&] { WriteJsonFile(MetaPath(root, "web-push"), db); std::error_code ec; fs::permissions(MetaPath(root, "web-push"), fs::perms::owner_read | fs::perms::owner_write, ec); };
    auto vapid = [&]() -> EVP_PKEY* {
        if (db.contains("vapidPrivatePem")) return webpush::FromPem(db["vapidPrivatePem"].get<std::string>());
        EVP_PKEY* k = webpush::Gen(); db["vapidPrivatePem"] = webpush::ToPem(k); db["vapidPublicKey"] = webpush::B64u(webpush::RawPub(k)); save(); return k;
    };
    const auto endpoint = str("endpoint").empty() ? (p.contains("subscription") ? p["subscription"].value("endpoint", std::string()) : std::string()) : str("endpoint");
    auto find = [&]() -> json* { for (auto& s : db["subscriptions"]) if (s.value("endpoint", "") == endpoint && (s.value("owner", "") == t_principal || t_principal == "operator")) return &s; return nullptr; };
    if (m == "push.web.vapidPublicKey") { EVP_PKEY* k = vapid(); EVP_PKEY_free(k); return Ok({{"publicKey", db["vapidPublicKey"]}}); }
    if (m == "push.web.subscribe") {
        auto sub = p.value("subscription", json::object());
        const auto keys = sub.value("keys", json::object());
        if (endpoint.rfind("https://", 0) != 0 && endpoint.rfind("http://127.0.0.1", 0) != 0 && endpoint.rfind("http://localhost", 0) != 0) return Fail("subscription.endpoint must be an https URL");
        if (keys.value("p256dh", "").empty() || keys.value("auth", "").empty()) return Fail("subscription.keys.p256dh and keys.auth are required");
        json rec{{"endpoint", endpoint}, {"keys", keys}, {"owner", t_principal}, {"createdAtMs", NowMs()}, {"preferences", json::object()}};
        if (auto* s = find()) { (*s)["keys"] = keys; } else db["subscriptions"].push_back(rec);
        save(); return Ok({{"subscribed", true}, {"endpoint", endpoint}});
    }
    if (m == "push.web.unsubscribe") {
        json next = json::array(); bool rm = false; for (auto& s : db["subscriptions"]) { if (s.value("endpoint", "") == endpoint && !rm) { rm = true; continue; } next.push_back(s); }
        db["subscriptions"] = next; save(); return Ok({{"unsubscribed", rm}});
    }
    if (m == "push.web.preferences.get" || m == "push.web.preferences.set") {
        auto* s = find(); if (!s) return Fail("unknown web push subscription");
        if (m == "push.web.preferences.set") { if (!p.contains("preferences") || !p["preferences"].is_object()) return Fail("preferences object is required"); (*s)["preferences"].merge_patch(p["preferences"]); save(); }
        return Ok({{"durableIdentity", t_principal != "operator"}, {"user", (*s)["preferences"]}, {"endpoint", endpoint}});
    }
    if (m == "push.web.test") {
        if (db["subscriptions"].empty()) return Fail("no web push subscriptions registered");
        EVP_PKEY* k = vapid();
        const json payload{{"title", str("title").empty() ? std::string("OpenClaw") : str("title")}, {"body", str("body").empty() ? std::string("Web push test notification") : str("body")}};
        json results = json::array(); bool any = false;
        for (auto& s : db["subscriptions"]) {
            const auto ep = s["endpoint"].get<std::string>();
            const auto ua = B64Decode(s["keys"]["p256dh"].get<std::string>()), au = B64Decode(s["keys"]["auth"].get<std::string>());
            auto body = webpush::Encrypt(ua, au, payload.dump());
            if (!body) { results.push_back({{"endpoint", ep}, {"ok", false}, {"error", "invalid subscription keys"}}); continue; }
            static const std::regex ore("^(https?://[^/]+)"); std::smatch om; std::regex_search(ep, om, ore);
            const auto jwt = webpush::Es256Jwt(k, {{"aud", om[1].str()}, {"exp", NowMs() / 1000 + 12 * 3600}, {"sub", "mailto:openclaw@localhost"}});
            CURL* c = curl_easy_init(); curl_slist* h = nullptr; std::string resp; long code = 0;
            h = curl_slist_append(h, "Content-Encoding: aes128gcm"); h = curl_slist_append(h, "Content-Type: application/octet-stream"); h = curl_slist_append(h, "TTL: 60");
            h = curl_slist_append(h, ("Authorization: vapid t=" + jwt + ", k=" + db["vapidPublicKey"].get<std::string>()).c_str());
            curl_easy_setopt(c, CURLOPT_URL, ep.c_str()); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h); curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->data());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size())); curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Sink); curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
            auto rc = curl_easy_perform(c); curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code); curl_slist_free_all(h); curl_easy_cleanup(c);
            const bool ok = rc == CURLE_OK && code >= 200 && code < 300; any = any || ok;
            json r{{"endpoint", ep}, {"ok", ok}, {"status", code}}; if (rc != CURLE_OK) r["error"] = curl_easy_strerror(rc);
            if (code == 404 || code == 410) { s["expired"] = true; r["expired"] = true; }
            results.push_back(r);
        }
        EVP_PKEY_free(k);
        json keep = json::array(); for (auto& s : db["subscriptions"]) if (!s.value("expired", false)) keep.push_back(s); db["subscriptions"] = keep; save();
        if (!any) { ExtraResult f = Fail("all web push deliveries failed"); f.payload = {{"results", results}}; return f; }
        return Ok({{"results", results}});
    }
    if (m == "push.test") {
        if (str("nodeId").empty()) return Fail("nodeId required");
        auto reg = ReadJsonFile(MetaPath(root, "apns-registrations"), json::object());
        if (!reg.contains(str("nodeId"))) return Fail("node " + str("nodeId") + " has no APNs registration (connect iOS node first)");
        const auto relay = config::GetPath(d.config.raw, "push.apns.relayUrl");
        if (!relay || !relay->is_string()) return Fail("APNs delivery needs push.apns.relayUrl (direct APNs keys are not supported in the C++ runtime)");
        auto r = GitHubRequest(relay->get<std::string>(), "POST", "", json{{"token", reg[str("nodeId")].value("token", "")}, {"title", str("title").empty() ? std::string("OpenClaw") : str("title")},
                                                                          {"body", str("body").empty() ? "Push test for node " + str("nodeId") : str("body")}}.dump());
        return Ok({{"ok", r.status >= 200 && r.status < 300}, {"status", r.status}, {"transport", "relay"}});
    }
    return {};
}

// ---------------------------------------------------------------- users.authConnect (per-user model account connect) and tools.github (agent GitHub identity)
ExtraResult AuthConnect(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    std::unique_lock<std::mutex> l(g_mu);
    auto db = ReadJsonFile(MetaPath(root, "auth-connect"), json::object());
    auto save = [&] { WriteJsonFile(MetaPath(root, "auth-connect"), db); };
    if (m == "users.authConnect.catalog") {
        json out = json::array();
        for (auto& h : kProviderHints) out.push_back({{"provider", h.provider}, {"methods", json::array({{{"id", "api-key"}, {"label", "API key"}, {"available", true}}})}});
        if (auto pv = config::GetPath(d.config.raw, "models.providers"); pv && pv->is_object())
            for (auto& [k, v] : pv->items()) if (v.is_object() && v.contains("oauth")) {
                bool found = false; for (auto& o : out) if (o["provider"] == k) { o["methods"].push_back({{"id", "oauth"}, {"label", "Sign in"}, {"available", true}}); found = true; }
                if (!found) out.push_back({{"provider", k}, {"methods", json::array({{{"id", "oauth"}, {"label", "Sign in"}, {"available", true}}})}}); }
        return Ok({{"providers", out}});
    }
    if (m == "users.authConnect.start") {
        if (str("provider").empty()) return Fail("provider is required");
        const auto meth = str("method").empty() ? std::string("api-key") : str("method");
        if (meth == "oauth") {
            auto pv = config::GetPath(d.config.raw, "models.providers." + str("provider"));
            if (!pv || !pv->is_object() || !pv->contains("oauth")) return Fail("connect method oauth is not available for " + str("provider") + "; use api-key");
            const auto host = d.config.gateway_host.empty() || d.config.gateway_host == "0.0.0.0" ? std::string("127.0.0.1") : d.config.gateway_host;
            const auto redirect = str("redirectUri").empty() ? "http://" + host + ":" + std::to_string(d.config.gateway_port) + "/oauth/callback" : str("redirectUri");
            json ep, b; try { ep = oauthflow::Resolve((*pv)["oauth"], "", redirect); b = oauthflow::Begin(ep, redirect, ""); } catch (const std::exception& e) { return Fail(std::string("sign-in could not start: ") + e.what()); }
            const auto id = Rid("connect");
            db[id] = {{"connectId", id}, {"provider", str("provider")}, {"method", meth}, {"owner", t_principal}, {"profileId", str("profileId").empty() ? t_principal : str("profileId")},
                      {"status", "awaiting_input"}, {"step", {{"stepId", "authorize"}, {"kind", "redirect"}, {"url", b["url"]}, {"label", "Sign in to " + str("provider") + ", then paste the final redirected URL"}}},
                      {"oauth", {{"state", b["state"]}, {"verifier", b["verifier"]}, {"redirectUri", redirect}, {"endpoints", ep}}}, {"expiresAtMs", NowMs() + 15 * 60 * 1000}};
            save(); json o = db[id]; o.erase("owner"); o.erase("oauth"); return Ok(o);
        }
        if (meth != "api-key") return Fail("connect method " + meth + " is not available for " + str("provider") + "; use api-key");
        const auto id = Rid("connect");
        db[id] = {{"connectId", id}, {"provider", str("provider")}, {"method", meth}, {"owner", t_principal}, {"profileId", str("profileId").empty() ? t_principal : str("profileId")},
                  {"status", "awaiting_input"}, {"step", {{"stepId", "apiKey"}, {"kind", "secret"}, {"label", str("provider") + " API key"}}}, {"expiresAtMs", NowMs() + 15 * 60 * 1000}};
        save(); json o = db[id]; o.erase("owner"); return Ok(o);
    }
    const auto id = str("connectId");
    if (!db.contains(id) || db[id].value("owner", "") != t_principal) return Fail("unknown connect session");
    auto& c = db[id];
    if (c.value("expiresAtMs", 0LL) < NowMs() && c["status"] == "awaiting_input") c["status"] = "expired";
    auto view = [&] { json o = c; o.erase("owner"); o.erase("oauth"); return o; };
    if (m == "users.authConnect.status") { save(); return Ok(view()); }
    if (m == "users.authConnect.cancel") { if (c["status"] == "awaiting_input") c["status"] = "cancelled"; save(); return Ok(view()); }
    if (m == "users.authConnect.answer") {
        if (c["status"] != "awaiting_input") return Fail("connect session is " + c["status"].get<std::string>());
        if (str("stepId") != c["step"].value("stepId", "")) return Fail("answer is for a different step");
        const auto key = str("value");
        const auto prov = c["provider"].get<std::string>(); const auto ap = prov + ":user-" + SafeName(c["profileId"].get<std::string>());
        auth::ProfileStore store(auth::DefaultProfilesPath("main"));
        if (c.value("method", "") == "oauth") {
            json tok; try { tok = oauthflow::Exchange(c["oauth"], key); } catch (const std::exception& e) { c["status"] = "failed"; c["error"] = e.what(); c.erase("step"); save(); return Ok(view()); }
            json prof{{"type", "oauth"}, {"provider", prov}, {"access", tok["access_token"]}, {"owner", c["profileId"]}};
            if (tok.contains("refresh_token")) prof["refresh"] = tok["refresh_token"]; if (tok.contains("expiresAt")) prof["expires"] = tok["expiresAt"];
            if (!store.Upsert(ap, prof)) return Fail("could not save credentials");
            c.erase("oauth");
        } else {
        if (Trim(key).size() < 8) return Fail("that does not look like an API key");
        if (!store.Upsert(ap, {{"type", "api_key"}, {"provider", prov}, {"key", Trim(key)}, {"owner", c["profileId"]}})) return Fail("could not save credentials");
        }
        c["status"] = "connected"; c["authProfileId"] = ap; c.erase("step"); save();
        const auto prof = c["profileId"]; l.unlock();
        auto r = GatewayRpc("users.linkAuthProfile", {{"profileId", prof}, {"authProfileId", ap}});
        l.lock(); db = ReadJsonFile(MetaPath(root, "auth-connect"), db); db[id]["linked"] = r.value("ok", false); save();
        json o = db[id]; o.erase("owner"); return Ok(o);
    }
    return {};
}
ExtraResult ToolsGitHub(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    const auto agent = str("agentId");
    const auto scope = str("scope").empty() ? str("selectedScope") : str("scope");
    if (m != "tools.github.authorize.poll" && m != "tools.github.authorize.cancel") {
        if (agent.empty()) return Fail("agentId is required");
        if (scope != "system" && scope != "agent") return Fail("scope must be system or agent");
    }
    auto cfgdb = ReadJsonFile(MetaPath(root, "tools-github"), json::object());
    auto principal = [&](const std::string& sc) { return sc == "system" ? std::string("tools-github:system") : "tools-github:agent:" + agent; };
    auto facts = [&](const std::string& sc) -> json {
        const auto key = sc == "system" ? std::string("system") : "agent:" + agent;
        auto c = cfgdb.value(key, json::object());
        json author{{"name", c.value("gitAuthor", json::object()).value("name", json(nullptr))}, {"email", c.value("gitAuthor", json::object()).value("email", json(nullptr))}};
        std::lock_guard<std::mutex> l(g_mu);
        auto db = PersonalRead(root);
        const auto pr = principal(sc);
        if (db["accounts"].contains(pr)) { auto& a = db["accounts"][pr];
            return {{"source", sc == "system" ? "system-configured" : "agent-override"}, {"credentialKind", a.value("kind", std::string("managed-oauth"))}, {"credentialState", "available"},
                    {"account", {{"login", a["account"]["login"]}}}, {"gitAuthor", author}, {"evidence", "github-api"}, {"accessExpiresAtMs", a.value("accessExpiresAtMs", json(nullptr))},
                    {"refreshState", a.contains("accessExpiresAtMs") ? "available" : "not_applicable"}, {"oauthScopes", a.value("scopes", json::array())}, {"repositoryGrants", "unknown"}}; }
        if (sc == "system" && !GitHubToken().empty())
            return {{"source", "system-detected"}, {"credentialKind", "native"}, {"credentialState", "unverified"}, {"account", nullptr}, {"gitAuthor", author}, {"evidence", "unverified"},
                    {"accessExpiresAtMs", nullptr}, {"refreshState", "not_applicable"}, {"oauthScopes", json::array()}, {"repositoryGrants", "unknown"}};
        return nullptr;
    };
    if (m == "tools.github.status") {
        auto sel = facts(scope); const bool inherit = cfgdb.value("agent:" + agent, json::object()).value("mode", "") == "inherit";
        json eff = scope == "agent" && (sel.is_null() || inherit) ? facts("system") : sel;
        if (eff.is_null()) eff = {{"source", "system-detected"}, {"credentialKind", "native"}, {"credentialState", "unavailable"}, {"account", nullptr}, {"gitAuthor", {{"name", nullptr}, {"email", nullptr}}},
                                  {"evidence", "none"}, {"accessExpiresAtMs", nullptr}, {"refreshState", "not_applicable"}, {"oauthScopes", json::array()}, {"repositoryGrants", "unknown"}};
        return Ok({{"agentId", agent}, {"selectedScope", scope}, {"selected", {{"scope", scope}, {"configured", !sel.is_null()}, {"identity", sel}}}, {"effective", eff}});
    }
    if (m == "tools.github.configure") {
        const auto key = scope == "system" ? std::string("system") : "agent:" + agent;
        const auto mode = str("mode");
        if (mode == "inherit") {
            if (scope == "system") return Fail("the system scope cannot inherit");
            cfgdb[key] = {{"mode", "inherit"}}; WriteJsonFile(MetaPath(root, "tools-github"), cfgdb);
            std::lock_guard<std::mutex> l(g_mu); auto db = PersonalRead(root); db["accounts"].erase(principal(scope)); PersonalWrite(root, db);
            return Ok({{"configured", true}, {"mode", "inherit"}});
        }
        if (mode != "managed") return Fail("mode must be managed or inherit");
        const auto sn = str("secretName"); const char* tok = sn.empty() ? nullptr : std::getenv(sn.c_str());
        if (!tok || !*tok) return Fail("temporary GitHub credential is unavailable");
        std::string api; try { api = GitHubApiBase(d.config.raw); } catch (const std::exception& e) { return Fail(e.what()); }
        auto u = GitHubRequest(api + "/user", "GET", tok);
        json acct; try { auto uj = json::parse(u.body); acct = {{"accountId", uj.at("id")}, {"login", uj.at("login")}}; } catch (...) {}
        if (u.status != 200 || acct.is_null()) return Fail("GitHub identity setup failed");
        json c{{"mode", "managed"}};
        if (p.contains("gitAuthor")) { json a = json::object(); for (auto k : {"name", "email"}) if (p["gitAuthor"].contains(k)) a[k] = Trim(p["gitAuthor"][k].get<std::string>()); c["gitAuthor"] = a; }
        cfgdb[key] = c; WriteJsonFile(MetaPath(root, "tools-github"), cfgdb);
        { std::lock_guard<std::mutex> l(g_mu); auto db = PersonalRead(root); db["accounts"][principal(scope)] = {{"generation", Uuid()}, {"account", acct}, {"token", tok}, {"kind", "managed-pat"}, {"connectedAtMs", NowMs()}}; PersonalWrite(root, db); }
        return Ok({{"configured", true}, {"mode", "managed"}, {"account", {{"login", acct["login"]}}}});
    }
    // Device flow reuses the personal GitHub flow under an agent/system principal.
    const auto prev = t_principal;
    json q = json::object();
    if (m == "tools.github.authorize.start") t_principal = principal(scope);
    else {
        auto rid = str("requestId"); if (rid.rfind("github-device-", 0) != 0) return Fail("invalid requestId");
        rid = rid.substr(14);
        std::lock_guard<std::mutex> l(g_mu); auto db = PersonalRead(root);
        for (auto& [k, v] : db["pending"].items()) { std::string kk = k; kk.erase(std::remove(kk.begin(), kk.end(), '-'), kk.end());
            if (kk == rid && v.value("principal", "").rfind("tools-github:", 0) == 0) { t_principal = v.value("principal", prev); rid = k; } }
        q["requestId"] = rid;
    }
    auto r = UsersGitHub(d, root, m == "tools.github.authorize.start" ? "users.github.authorize.start" : m == "tools.github.authorize.poll" ? "users.github.authorize.poll" : "users.github.authorize.cancel", q);
    t_principal = prev;
    if (r.ok && r.payload.contains("requestId")) { std::string id = r.payload["requestId"]; id.erase(std::remove(id.begin(), id.end(), '-'), id.end()); r.payload["requestId"] = "github-device-" + id; }
    if (r.ok && r.payload.contains("personal")) { r.payload.erase("personal"); }
    return r;
}


// ---------------------------------------------------------------- attach grants + gateway MCP server (POST /mcp, streamable-HTTP JSON responses)
struct AttachGrant { std::string sessionKey; long long expires = 0; };
std::mutex g_attach_mu; std::map<std::string, AttachGrant> g_attach;
std::string AttachGrantSession(const std::string& token) {
    std::lock_guard<std::mutex> l(g_attach_mu);
    auto it = g_attach.find(token); if (it == g_attach.end()) return {};
    if (it->second.expires < NowMs()) { g_attach.erase(it); return {}; }
    return it->second.sessionKey;
}
json HandleMcpHttp(const json& req, const std::string& session_key) {
    const auto id = req.contains("id") ? req["id"] : json(nullptr);
    const auto m = req.value("method", std::string());
    auto res = [&](json r) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", r}}; };
    auto err = [&](int c, const std::string& msg) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", c}, {"message", msg}}}}; };
    if (m == "initialize") return res({{"protocolVersion", req.value("params", json::object()).value("protocolVersion", std::string("2025-06-18"))},
                                       {"capabilities", {{"tools", {{"listChanged", false}}}}}, {"serverInfo", {{"name", "openclaw-gateway"}, {"version", "cpp"}}},
                                       {"instructions", "Tools run inside OpenClaw session " + session_key}});
    if (m == "ping") return res(json::object());
    if (m == "tools/list") {
        json tools = json::array();
        if (g_tool_deps) for (auto& n : g_tool_deps->tools.Names()) { auto* s = g_tool_deps->tools.Get(n); if (!s) continue;
            tools.push_back({{"name", n}, {"description", s->description}, {"inputSchema", s->parameters.is_object() ? s->parameters : json{{"type", "object"}}}}); }
        return res({{"tools", tools}});
    }
    if (m == "tools/call") {
        const auto pr = req.value("params", json::object());
        auto r = GatewayRpc("tools.invoke", {{"name", pr.value("name", std::string())}, {"args", pr.value("arguments", json::object())}, {"sessionKey", session_key}});
        if (!r.value("ok", false)) return res({{"content", json::array({{{"type", "text"}, {"text", r.value("error", json("tool failed")).dump()}}})}, {"isError", true}});
        auto out = r["payload"].value("result", r["payload"]);
        return res({{"content", json::array({{{"type", "text"}, {"text", out.is_string() ? out.get<std::string>() : out.dump()}}})}, {"isError", false}});
    }
    return err(-32601, "method not found: " + m);
}
ExtraResult Attach(ServerDeps& d, const std::string& m, const json& p) {
    if (m == "attach.revoke") {
        const auto tok = p.value("token", std::string()); if (tok.empty()) return Fail("token is required");
        std::lock_guard<std::mutex> l(g_attach_mu); return Ok({{"revoked", g_attach.erase(tok) > 0}});
    }
    auto key = p.value("sessionKey", std::string("main"));
    const auto agent = p.value("agentId", std::string("main"));
    if (key.rfind("agent:", 0) != 0) key = "agent:" + agent + ":" + key;
    const long long ttl = std::clamp<long long>(p.value("ttlMs", 3600000LL), 1000, 24LL * 3600000);
    unsigned char rb[24]; RAND_bytes(rb, 24); std::string tok = "oca_"; static const char* hx = "0123456789abcdef"; for (auto b : rb) { tok.push_back(hx[b >> 4]); tok.push_back(hx[b & 15]); }
    { std::lock_guard<std::mutex> l(g_attach_mu); g_attach[tok] = {key, NowMs() + ttl}; }
    const auto host = d.config.gateway_host.empty() || d.config.gateway_host == "0.0.0.0" ? std::string("127.0.0.1") : d.config.gateway_host;
    const auto url = "http://" + host + ":" + std::to_string(d.config.gateway_port) + "/mcp";
    return Ok({{"sessionKey", key}, {"token", tok}, {"expiresAtMs", NowMs() + ttl},
               {"mcpConfig", {{"type", "http"}, {"url", url}, {"headers", {{"Authorization", "Bearer ${OPENCLAW_MCP_TOKEN}"}}}}}, {"env", {{"OPENCLAW_MCP_TOKEN", tok}}}});
}

// ---------------------------------------------------------------- board widgets (put → grant → app view ticket → events / prompt authorization)
ExtraResult BoardWidgets(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    std::unique_lock<std::mutex> l(g_mu);
    auto db = ReadJsonFile(MetaPath(root, "board-widgets"), json{{"widgets", json::object()}, {"tickets", json::object()}, {"events", json::array()}});
    auto save = [&] { WriteJsonFile(MetaPath(root, "board-widgets"), db); };
    auto wkey = [&] { return str("sessionKey") + "\n" + str("name"); };
    if (m == "board.widget.put") {
        if (str("sessionKey").empty() || str("name").empty() || !p.contains("content")) return Fail("sessionKey, name and content are required");
        auto& w = db["widgets"][wkey()]; const int rev = w.is_object() ? w.value("revision", 0) + 1 : 1;
        const auto kind = p["content"].is_object() ? p["content"].value("kind", p["content"].value("type", std::string("markdown"))) : std::string("markdown");
        const bool needs_grant = kind == "html" || kind == "app" || kind == "mcp-app";
        const auto inst = Rid("wi");
        w = {{"sessionKey", str("sessionKey")}, {"name", str("name")}, {"title", str("title").empty() ? str("name") : str("title")}, {"content", p["content"]},
             {"presentation", p.value("presentation", json(nullptr))}, {"placement", p.value("placement", json(nullptr))}, {"revision", rev}, {"instanceId", inst},
             {"grant", needs_grant ? "pending" : "not_required"}, {"updatedAtMs", NowMs()}};
        save(); l.unlock(); GatewayEmit("board.changed", {{"sessionKey", str("sessionKey")}, {"widget", str("name")}, {"revision", rev}});
        return Ok({{"name", str("name")}, {"revision", rev}, {"instanceId", inst}, {"grant", needs_grant ? "pending" : "not_required"}});
    }
    if (m == "board.widget.grant" || m == "board.widget.appView") {
        if (!db["widgets"].contains(wkey())) return Fail("unknown widget");
        auto& w = db["widgets"][wkey()];
        if (p.value("revision", 0) != w["revision"].get<int>() || str("instanceId") != w.value("instanceId", std::string())) return Fail("widget changed; reload the board");
        if (m == "board.widget.grant") {
            if (str("decision") != "granted" && str("decision") != "rejected") return Fail("decision must be granted or rejected");
            w["grant"] = str("decision"); w["grantedBy"] = t_principal; save(); return Ok({{"name", str("name")}, {"grant", w["grant"]}});
        }
        if (w["grant"] == "pending" || w["grant"] == "rejected") return Fail("widget app view is not granted");
        const auto vid = Rid("bview"); const long long exp = NowMs() + 10 * 60 * 1000;
        db["tickets"][vid] = {{"sessionKey", w["sessionKey"]}, {"name", w["name"]}, {"revision", w["revision"]}, {"expiresAtMs", exp}};
        save(); return Ok({{"viewId", vid}, {"expiresAtMs", exp}});
    }
    // ticket-bound calls from a rendered widget
    json ticket = p.contains("ticket") ? p["ticket"] : json(nullptr);
    const auto tid = ticket.is_string() ? ticket.get<std::string>() : ticket.is_object() ? ticket.value("viewId", ticket.value("id", std::string())) : std::string();
    if (!db["tickets"].contains(tid) || db["tickets"][tid].value("expiresAtMs", 0LL) < NowMs()) return Fail("board view ticket expired or unknown");
    auto tk = db["tickets"][tid];
    if (m == "board.prompt.authorize") return Ok({{"authorized", true}, {"sessionKey", tk["sessionKey"]}, {"widget", tk["name"]}, {"expiresAtMs", tk["expiresAtMs"]}});
    if (m == "board.event") {
        json ev{{"ticket", tid}, {"widget", tk["name"]}, {"sessionKey", tk["sessionKey"]}, {"event", p.value("event", p.value("name", json("event")))}, {"payload", p.value("payload", p.value("data", json(nullptr)))}, {"at", NowMs()}};
        db["events"].push_back(ev); while (db["events"].size() > 500) db["events"].erase(0);
        save(); l.unlock();
        GatewayEmit("board.event", ev);
        if (p.value("prompt", false) || p.contains("message")) GatewayRpc("chat.inject", {{"sessionKey", tk["sessionKey"]}, {"message", "[board " + tk["name"].get<std::string>() + "] " + p.value("message", ev["event"].dump())}});
        return Ok({{"accepted", true}});
    }
    return {};
}

// ---------------------------------------------------------------- control UI helpers: link/GitHub previews, PR checks
ExtraResult ControlUi(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    std::string api; try { api = GitHubApiBase(d.config.raw); } catch (const std::exception& e) { if (m != "controlUi.linkPreview") return Fail(e.what()); }
    auto gh = [&](const std::string& path) -> json { auto r = GitHubRequest(api + path, "GET", GitHubToken()); if (r.status != 200) return nullptr; try { return json::parse(r.body); } catch (...) { return nullptr; } };
    if (m == "controlUi.linkPreview") {
        const auto url = str("url"); if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return Fail("url must be http(s)");
        auto f = net::SafeFetch(url, 512 * 1024, std::getenv("OPENCLAW_LINK_PREVIEW_ALLOW_PRIVATE") != nullptr, 10);
        if (!f.ok) return Fail("preview fetch failed: " + f.error);
        auto meta = [&](const std::string& prop) -> std::string {
            std::regex re("<meta[^>]+(?:property|name)=[\"']" + prop + "[\"'][^>]*content=[\"']([^\"']*)[\"']", std::regex::icase); std::smatch mm;
            return std::regex_search(f.body, mm, re) ? mm[1].str() : std::string(); };
        std::smatch tm; std::string title = meta("og:title");
        if (title.empty() && std::regex_search(f.body, tm, std::regex("<title[^>]*>([^<]*)</title>", std::regex::icase))) title = Trim(tm[1].str());
        std::string desc = meta("og:description"); if (desc.empty()) desc = meta("description");
        return Ok({{"url", url}, {"status", f.status}, {"title", title}, {"description", desc}, {"image", meta("og:image")}, {"siteName", meta("og:site_name")}});
    }
    auto parse = [&](std::string& owner, std::string& repo, std::string& kind, int& num) {
        owner = str("owner"); repo = str("repo"); num = p.value("number", 0); kind = str("kind");
        std::smatch mm; const auto url = str("url");
        if (std::regex_search(url, mm, std::regex(R"(github\.com/([^/]+)/([^/]+)(?:/(pull|issues)/(\d+))?)"))) { owner = mm[1]; repo = mm[2]; if (mm[3].matched) { kind = mm[3] == "pull" ? "pull" : "issue"; num = std::stoi(mm[4]); } }
        if (repo.size() > 4 && repo.substr(repo.size() - 4) == ".git") repo.resize(repo.size() - 4);
    };
    if (m == "controlUi.githubPreview" || m == "controlUi.githubDetail") {
        std::string o, r, k; int n = 0; parse(o, r, k, n);
        if (o.empty() || r.empty()) return Fail("a github.com URL or owner/repo is required");
        if (n == 0) { auto j = gh("/repos/" + o + "/" + r); if (j.is_null()) return Fail("repository not found or not accessible");
            return Ok({{"kind", "repository"}, {"fullName", j.value("full_name", o + "/" + r)}, {"description", j.value("description", json(nullptr))}, {"private", j.value("private", false)}, {"defaultBranch", j.value("default_branch", "")}}); }
        auto j = gh("/repos/" + o + "/" + r + (k == "pull" ? "/pulls/" : "/issues/") + std::to_string(n));
        if (j.is_null()) return Fail("not found or not accessible");
        json out{{"kind", k.empty() ? "issue" : k}, {"repository", o + "/" + r}, {"number", n}, {"title", j.value("title", "")}, {"state", j.value("state", "")},
                 {"author", j.contains("user") ? j["user"].value("login", "") : ""}, {"url", j.value("html_url", "")}};
        if (m == "controlUi.githubDetail") { out["body"] = j.value("body", json(nullptr)); out["labels"] = json::array(); for (auto& lb : j.value("labels", json::array())) out["labels"].push_back(lb.value("name", ""));
            if (k == "pull") { out["head"] = j.value("head", json::object()).value("ref", ""); out["base"] = j.value("base", json::object()).value("ref", ""); out["merged"] = j.value("merged", false); out["draft"] = j.value("draft", false); } }
        return Ok(out);
    }
    if (m == "controlUi.sessionPullRequests.checks" || m == "controlUi.sessionPullRequests.subscribe") {
        const auto key = str("sessionKey"); if (key.empty()) return Fail("sessionKey is required");
        json prs = json::array();
        { std::lock_guard<std::mutex> l(g_mu); auto db = ReadJsonFile(PubPath(root), json{{"requests", json::object()}});
          for (auto& [id, rec] : db["requests"].items()) if (rec.value("sessionKey", "") == key && rec.contains("effect")) prs.push_back({{"requestId", id}, {"url", rec["effect"].value("url", "")}, {"headCommit", rec["effect"].value("headCommit", "")}, {"repository", rec.value("repository", "")}}); }
        if (m == "controlUi.sessionPullRequests.subscribe") {
            std::lock_guard<std::mutex> l(g_mu); auto sub = ReadJsonFile(MetaPath(root, "pr-subscriptions"), json::object()); sub[key] = {{"since", NowMs()}, {"by", t_principal}};
            WriteJsonFile(MetaPath(root, "pr-subscriptions"), sub); return Ok({{"subscribed", true}, {"pullRequests", prs.size()}});
        }
        for (auto& pr : prs) {
            std::smatch mm; const auto u = pr["url"].get<std::string>(); std::string repo = pr["repository"];
            if (repo.empty() && std::regex_search(u, mm, std::regex(R"(github\.com/([^/]+/[^/]+)/pull/)"))) repo = mm[1];
            auto j = repo.empty() || pr["headCommit"] == "" ? json(nullptr) : gh("/repos/" + repo + "/commits/" + pr["headCommit"].get<std::string>() + "/check-runs");
            json runs = json::array(); std::string overall = j.is_null() ? "unknown" : "success";
            if (!j.is_null()) for (auto& c : j.value("check_runs", json::array())) { runs.push_back({{"name", c.value("name", "")}, {"status", c.value("status", "")}, {"conclusion", c.value("conclusion", json(nullptr))}});
                if (c.value("status", "") != "completed") overall = overall == "failure" ? overall : "pending"; else if (c.value("conclusion", json("")).is_string() && c["conclusion"] != "success" && c["conclusion"] != "skipped" && c["conclusion"] != "neutral") overall = "failure"; }
            pr["checks"] = runs; pr["state"] = overall;
        }
        return Ok({{"sessionKey", key}, {"pullRequests", prs}});
    }
    return {};
}

// ---------------------------------------------------------------- misc singles: conversations, web.login, worker.desktop, catalog, discussion, ui.command, config.openFile,
// artifacts.download, transcripts.summarize, chat.metadata.changed, nativeHook.invoke, claws, device scopes/token, memory migrations
std::string ShaHex256(const std::string& s) { unsigned char h[32]; SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), h); static const char* x = "0123456789abcdef"; std::string o; for (auto b : h) { o.push_back(x[b >> 4]); o.push_back(x[b & 15]); } return o; }
ExtraResult Singles(ServerDeps& d, const fs::path& root, const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    std::error_code ec;
    if (m == "conversations.send" || m == "conversations.turn") {
        const auto key = str("sessionKey").empty() ? (str("conversationId").empty() ? str("key") : str("conversationId")) : str("sessionKey");
        const auto msg = str("message").empty() ? str("text") : str("message");
        if (key.empty() || msg.empty()) return Fail("sessionKey and message are required");
        if (m == "conversations.send") { auto r = GatewayRpc("chat.inject", {{"sessionKey", key}, {"message", msg}}); return r.value("ok", false) ? Ok({{"sent", true}, {"sessionKey", key}}) : Fail("send failed"); }
        const auto tid = Rid("turn");
        { std::lock_guard<std::mutex> l(g_mu); auto db = ReadJsonFile(MetaPath(root, "conversation-turns"), json::object()); db[tid] = {{"sessionKey", key}, {"status", "running"}, {"startedAt", NowMs()}}; WriteJsonFile(MetaPath(root, "conversation-turns"), db); }
        auto r = GatewayRpc("chat.send", {{"sessionKey", key}, {"message", msg}});
        std::lock_guard<std::mutex> l(g_mu); auto db = ReadJsonFile(MetaPath(root, "conversation-turns"), json::object());
        const bool cancelled = db[tid].value("status", "") == "cancelled";
        db[tid]["status"] = cancelled ? "cancelled" : r.value("ok", false) ? "completed" : "failed"; db[tid]["endedAt"] = NowMs(); WriteJsonFile(MetaPath(root, "conversation-turns"), db);
        if (!r.value("ok", false)) return Fail("turn failed");
        return Ok({{"turnId", tid}, {"status", db[tid]["status"]}, {"reply", cancelled ? json(nullptr) : json(r["payload"].value("text", ""))}});
    }
    if (m == "conversations.turn.cancel") {
        std::lock_guard<std::mutex> l(g_mu); auto db = ReadJsonFile(MetaPath(root, "conversation-turns"), json::object());
        if (!db.contains(str("turnId"))) return Fail("unknown turn");
        auto& t = db[str("turnId")]; if (t["status"] != "running") return Ok({{"cancelled", false}, {"status", t["status"]}});
        t["status"] = "cancelled"; WriteJsonFile(MetaPath(root, "conversation-turns"), db); return Ok({{"cancelled", true}});
    }
    if (m == "web.login.start" || m == "web.login.wait") {
        const auto ch = str("channel").empty() ? std::string("whatsapp") : str("channel");
        const auto pm = ch + (m == "web.login.start" ? ".loginWithQrStart" : ".loginWithQrWait");
        if (!d.js_host || !d.js_host->HasGatewayMethod(pm)) return Fail("no loaded channel plugin provides QR login for " + ch + " (install the " + ch + " channel plugin)");
        json args = p; args.erase("channel");
        auto r = d.js_host->CallGatewayMethod(pm, args);
        if (!r.value("ok", true) || r.contains("error")) return Fail(r.value("error", json("login failed")).dump());
        return Ok(r.contains("payload") ? r["payload"] : r);
    }
    if (m == "worker.desktop.launch" || m == "worker.desktop.observe") {
        if (str("environmentId").empty()) return Fail("environmentId is required");
        auto& dc = desktop::Default();
        if (!dc.Available()) return Fail("no desktop is available in environment " + str("environmentId") + " (needs an X display)");
        if (m == "worker.desktop.launch") { auto r = dc.Launch({{"app", str("app")}}); if (!r.value("ok", true)) return Fail(r.value("error", std::string("launch failed"))); return Ok({{"app", str("app")}, {"status", "ready"}, {"detail", r}}); }
        return Ok(dc.Observe(p));
    }
    if (m == "catalog.browse" || m == "catalog.searchKeywords") {
        const auto q = str("query").empty() ? (m == "catalog.browse" ? std::string("") : str("q")) : str("query");
        if (m == "catalog.searchKeywords" && Trim(q).empty()) return Fail("query is required");
        auto r = clawhub::Search(q.empty() ? std::string("*") : q, std::clamp(p.value("limit", 20), 1, 100));
        if (!r.value("ok", false)) return Fail("ClawHub catalog unavailable: " + r.value("error", std::string("request failed")));
        if (m == "catalog.searchKeywords") { std::set<std::string> kw; for (auto& x : r["results"]) { for (auto& t : x.value("tags", json::array())) if (t.is_string()) kw.insert(t); auto s = x.value("slug", std::string()); if (!s.empty()) kw.insert(s); }
            json o = json::array(); for (auto& k : kw) o.push_back(k); return Ok({{"keywords", o}, {"registry", r["registry"]}}); }
        json items = json::array(); for (auto& x : r["results"]) { json i = x; i["kind"] = "skill"; items.push_back(i); }
        return Ok({{"items", items}, {"registry", r["registry"]}});
    }
    if (m == "session.discussion.info" || m == "session.discussion.open") {
        const auto key = str("sessionKey"); if (key.empty()) return Fail("sessionKey is required");
        const auto dk = key + ":discussion";
        if (m == "session.discussion.open" && !d.sessions.Exists(dk)) {
            if (!d.sessions.Exists(key)) return Fail("session not found");
            auto ms = d.sessions.Load(key); std::string ctx; for (size_t i = ms.size() > 6 ? ms.size() - 6 : 0; i < ms.size(); ++i) if (ms[i].role == "user" || ms[i].role == "assistant") ctx += ms[i].role + ": " + Utf8Cut(ms[i].content, 300) + "\n";
            agent::Message s; s.role = "system"; s.content = "Side discussion about session " + key + ". Recent context:\n" + ctx; d.sessions.Save(dk, {s});
        }
        const bool open = d.sessions.Exists(dk);
        return Ok({{"sessionKey", key}, {"state", open ? "open" : "none"}, {"discussionSessionKey", open ? json(dk) : json(nullptr)}, {"messages", open ? d.sessions.Load(dk).size() : 0}});
    }
    if (m == "ui.command") {
        if (!p.contains("command") || !p["command"].is_object()) return Fail("command object is required");
        GatewayEmit("ui.command", {{"command", p["command"]}, {"sessionKey", p.value("sessionKey", json(nullptr))}, {"from", t_principal}});
        return Ok({{"ok", true}});
    }
    if (m == "config.openFile") {
        const auto path = config::DefaultPath();
        if (!fs::exists(path, ec)) return Ok({{"ok", false}, {"path", path.string()}, {"error", "config file does not exist yet"}});
        if (const char* cmd = std::getenv("OPENCLAW_REVEAL_CMD")) { auto r = Run({cmd, path.string()}, {}); if (r.code != 0) return Ok({{"ok", false}, {"path", path.string()}, {"error", Trim(r.err)}}); }
        return Ok({{"ok", true}, {"path", path.string()}});
    }
    if (m == "artifacts.download") {
        auto a = d.media.GetJson(str("artifactId")); if (!a.value("ok", true) || !a.contains("path")) return Fail("artifact not found");
        json summary = a; summary.erase("path");
        if (str("transport") == "http") return Ok({{"artifact", summary}, {"url", "/v1/media/" + str("artifactId")}});
        const auto bytes = ReadAll(a["path"].get<std::string>());
        if (bytes.size() > 16 * 1024 * 1024) return Ok({{"artifact", summary}, {"unsupported", "too large for inline download; use transport=http"}});
        return Ok({{"artifact", summary}, {"encoding", "base64"}, {"data", channels::waweb::Base64(channels::waweb::Bytes(bytes.begin(), bytes.end()))}});
    }
    if (m == "transcripts.summarize") {
        const auto sel = p.value("selector", json::object()); const auto key = sel.value("sessionKey", sel.value("id", std::string()));
        if (key.empty() || !d.sessions.Exists(key)) return Fail("transcript not found");
        if (!d.router) return Fail("no model provider configured");
        std::string tr; for (auto& x : d.sessions.Load(key)) if (x.role == "user" || x.role == "assistant") tr += x.role + ": " + Utf8Cut(x.content, 2000) + "\n";
        agent::Message s; s.role = "system"; s.content = "Summarize the transcript. Reply as JSON {\"overview\":string,\"decisions\":[string],\"actionItems\":[string]}.";
        agent::Message u; u.role = "user"; u.content = Utf8Cut(tr, 60000);
        auto r = d.router->Complete({s, u}, json::array()); if (!r.ok) return Fail("summary model call failed: " + r.error);
        json sum; try { auto txt = r.message.content; auto a = txt.find('{'), b = txt.rfind('}'); sum = json::parse(txt.substr(a, b - a + 1)); } catch (...) { sum = {{"overview", r.message.content}, {"decisions", json::array()}, {"actionItems", json::array()}}; }
        sum["generatedAt"] = std::to_string(NowMs());
        { std::lock_guard<std::mutex> l(g_mu); auto db = ReadJsonFile(MetaPath(root, "transcript-summaries"), json::object()); db[key] = sum; WriteJsonFile(MetaPath(root, "transcript-summaries"), db); }
        return Ok({{"sessionKey", key}, {"summary", sum}});
    }
    if (m == "chat.metadata.changed") {
        const auto key = str("sessionKey"); if (key.empty()) return Fail("sessionKey is required");
        auto meta = ReadJsonFile(MetaPath(root, "session-meta"), json::object()).value("sessions", json::object()).value(key, json::object());
        GatewayEmit("chat.metadata.changed", {{"sessionKey", key}, {"metadata", meta}});
        return Ok({{"sessionKey", key}, {"metadata", meta}});
    }
    if (m == "nativeHook.invoke") {
        auto relays = ReadJsonFile(MetaPath(root, "native-hook-relays"), json::object());
        const auto rid = str("relayId");
        if (!relays.contains(rid)) return Fail("unknown native hook relay: " + rid);
        if (p.value("generation", -1) != relays[rid].value("generation", 0)) return Fail("stale native hook relay generation");
        if (str("provider") != relays[rid].value("provider", str("provider"))) return Fail("relay provider mismatch");
        json ev{{"relayId", rid}, {"provider", str("provider")}, {"event", p.value("event", json(nullptr))}, {"payload", p.value("rawPayload", json(nullptr))}, {"sessionKey", relays[rid].value("sessionKey", json(nullptr))}};
        GatewayEmit("nativeHook.event", ev);
        if (relays[rid].contains("sessionKey") && p.value("event", json("")).is_string()) GatewayRpc("chat.inject", {{"sessionKey", relays[rid]["sessionKey"]}, {"message", "[" + str("provider") + " hook] " + p["event"].get<std::string>()}});
        return Ok({{"delivered", true}});
    }
    if (m == "claws.monitors" || m == "claws.packages.remove") {
        std::lock_guard<std::mutex> l(g_mu); auto st = ClawStateRead(root);
        if (m == "claws.monitors") { json out = json::array(); for (auto& [a, ins] : st["installs"].items()) for (auto& mo : ins.value("monitors", json::array())) { json e = mo; e["agentId"] = a; out.push_back(e); }
            return Ok({{"monitors", out}}); }
        const auto agent = str("agentId"), pkg = str("packageId").empty() ? str("package") : str("packageId");
        if (!st["installs"].contains(agent)) return Fail("no claw install for agent " + agent);
        auto& pk = st["installs"][agent]["packages"]; if (!pk.is_array()) return Fail("package not installed: " + pkg);
        json keep = json::array(); json removed = nullptr;
        for (auto& x : pk) { if (x.value("id", x.value("name", std::string())) == pkg && removed.is_null()) removed = x; else keep.push_back(x); }
        if (removed.is_null()) return Fail("package not installed: " + pkg);
        const auto ws = WorkspaceOf(d, agent); if (removed.contains("path")) { fs::path pp = removed["path"].get<std::string>(); auto rel = fs::relative(pp, ws, ec);
            if (!ec && !rel.empty() && rel.string().rfind("..", 0) != 0) fs::remove_all(pp, ec); }
        pk = keep; WriteJsonFile(ClawStatePath(root), st);
        return Ok({{"removed", true}, {"package", removed}});
    }
    if (m == "device.token.rotate") {
        if (str("deviceId").empty() || str("role").empty()) return Fail("deviceId and role are required");
        if (!d.device_links) return Fail("device links are not enabled");
        auto r = d.device_links->Rotate(str("deviceId"), str("role"), p.value("scopes", json::array()));
        if (!r.value("ok", false)) return Fail(r.value("error", std::string("rotate failed")));
        if (t_principal != "device:" + str("deviceId")) { r.erase("token"); r["tokenDelivery"] = "withheld"; } else r["tokenDelivery"] = "response";
        return Ok(r);
    }
    if (m == "device.scopes.requestUpgrade" || m == "device.scopes.waitUpgrade") {
        std::unique_lock<std::mutex> l(g_mu); auto db = ReadJsonFile(MetaPath(root, "scope-upgrades"), json::object());
        if (m == "device.scopes.requestUpgrade") {
            if (!str("requestId").empty() && !str("decision").empty()) {  // operator decision
                if (!t_full_write) return Fail("deciding a scope upgrade needs operator write");
                if (!db.contains(str("requestId"))) return Fail("unknown upgrade request");
                auto& rq = db[str("requestId")]; if (rq["status"] != "pending") return Fail("request is " + rq["status"].get<std::string>());
                rq["status"] = str("decision") == "approve" || str("decision") == "approved" ? "approved" : "denied"; rq["decidedBy"] = t_principal;
                if (rq["status"] == "approved" && d.device_links) d.device_links->Rotate(rq["deviceId"].get<std::string>(), rq.value("role", std::string("device")), rq["scopes"]);
                WriteJsonFile(MetaPath(root, "scope-upgrades"), db); return Ok(rq);
            }
            const auto scopes = p.value("scopes", json::array()); if (!scopes.is_array() || scopes.empty()) return Fail("scopes are required");
            for (auto& s : scopes) if (!s.is_string() || !auth::DeviceLinkStore::ValidScope(s.get<std::string>())) return Fail("invalid scope " + s.dump());
            const auto id = Rid("scopeup");
            db[id] = {{"requestId", id}, {"deviceId", str("deviceId").empty() ? t_principal : str("deviceId")}, {"role", str("role").empty() ? std::string("device") : str("role")}, {"scopes", scopes}, {"status", "pending"}, {"createdAt", NowMs()}, {"reason", str("reason")}};
            WriteJsonFile(MetaPath(root, "scope-upgrades"), db); l.unlock();
            GatewayEmit("device.scopes.upgradeRequested", {{"requestId", id}, {"scopes", scopes}});
            return Ok({{"requestId", id}, {"status", "pending"}});
        }
        const auto id = str("requestId"); if (!db.contains(id)) return Fail("unknown upgrade request");
        const long long until = NowMs() + std::clamp(p.value("timeoutMs", 0), 0, 120000);
        l.unlock();
        for (;;) { auto cur = ReadJsonFile(MetaPath(root, "scope-upgrades"), json::object()).value(id, json::object());
            if (cur.value("status", "") != "pending" || NowMs() >= until) return Ok({{"requestId", id}, {"status", cur.value("status", "pending")}});
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    if (m == "migrations.memory.plan" || m == "migrations.memory.apply") {
        const auto agent = str("agentId"); if (agent.empty()) return Fail("agentId is required");
        const auto ws = WorkspaceOf(d, agent); const char* hm = std::getenv("HOME"); const fs::path home = hm ? hm : "/";
        struct Src { const char* provider; fs::path path; };
        std::vector<Src> srcs = {{"claude-code", home / ".claude" / "CLAUDE.md"}, {"claude-code", ws / "CLAUDE.md"}, {"codex", home / ".codex" / "AGENTS.md"}, {"codex", ws / "AGENTS.md"},
                                 {"cursor", ws / ".cursorrules"}, {"gemini", home / ".gemini" / "GEMINI.md"}, {"gemini", ws / "GEMINI.md"}};
        std::map<std::string, json> provs;
        for (auto& s : srcs) { if (!fs::is_regular_file(s.path, ec)) continue; const auto body = ReadAll(s.path);
            const auto iid = ShaHex256(s.path.string()).substr(0, 16); const auto dest = ws / "memory" / "imported" / (std::string(s.provider) + "-" + s.path.filename().string() + "-" + iid.substr(0, 6) + ".md");
            const bool exists = fs::exists(dest, ec);
            provs[s.provider]["items"].push_back({{"id", iid}, {"source", s.path.string()}, {"target", fs::relative(dest, ws, ec).string()}, {"bytes", body.size()}, {"sha256", ShaHex256(body)},
                                                 {"action", exists && !p.value("overwrite", false) ? "skip-existing" : exists ? "overwrite" : "create"}}); }
        json plan = json::array();
        for (auto& [k, v] : provs) { json pv{{"providerId", k}, {"items", v["items"]}}; pv["planFingerprint"] = ShaHex256(v["items"].dump()); plan.push_back(pv); }
        if (m == "migrations.memory.plan") return Ok({{"agentId", agent}, {"workspace", ws.string()}, {"providers", plan}});
        if (str("idempotencyKey").empty()) return Fail("idempotencyKey is required");
        std::lock_guard<std::mutex> l(g_mu); auto led = ReadJsonFile(MetaPath(root, "memory-migrations"), json::object());
        if (led.contains(str("idempotencyKey"))) return Ok(led[str("idempotencyKey")]);
        json pv; for (auto& x : plan) if (x["providerId"] == str("providerId")) pv = x;
        if (pv.is_null()) return Fail("unknown provider: " + str("providerId"));
        if (pv["planFingerprint"] != p.value("planFingerprint", json(""))) return Fail("plan changed; re-run migrations.memory.plan");
        json applied = json::array(), skipped = json::array();
        for (auto& want : p.value("itemIds", json::array())) { json it; for (auto& x : pv["items"]) if (x["id"] == want) it = x;
            if (it.is_null()) return Fail("unknown item " + want.dump());
            if (it["action"] == "skip-existing") { skipped.push_back(want); continue; }
            const auto dest = ws / it["target"].get<std::string>(); fs::create_directories(dest.parent_path(), ec);
            std::ofstream(dest) << "<!-- imported from " << it["source"].get<std::string>() << " -->\n" << ReadAll(it["source"].get<std::string>()); applied.push_back(want); }
        json out{{"agentId", agent}, {"providerId", str("providerId")}, {"applied", applied}, {"skipped", skipped}};
        led[str("idempotencyKey")] = out; WriteJsonFile(MetaPath(root, "memory-migrations"), led);
        return Ok(out);
    }
    return {};
}

ExtraResult TrySubsystemDispatch(ServerDeps& deps, const fs::path& root, const std::string& method, const json& params) {
    try {
        if (method.rfind("users.github.", 0) == 0) return UsersGitHub(deps, root, method, params);
        if (method.rfind("worktrees.", 0) == 0) { auto r = Worktrees(deps, root, method, params); if (r.handled) return r; }
        if (method == "storage.locations.list" || method == "storage.locations.probe") return Storage(deps, root, method, params);
        if (method.rfind("users.", 0) == 0 && method.rfind("users.authConnect.", 0) != 0) { auto r = Users(deps, root, method, params); if (r.handled) return r; }
        if (method == "device.pair.rename") return DeviceRename(root, params);
        if (method == "message.action") return MessageAction(root, params);
        { auto r = LocalGroups(deps, root, method, params); if (r.handled) return r; }
        { auto r = Batch3(deps, root, method, params); if (r.handled) return r; }
        { static const std::set<std::string> pm{"plugins.catalog.browse", "plugins.catalog.categories", "plugins.catalog.get", "plugins.controlUi.list", "plugins.controlUi.report",
              "plugins.controlUi.status", "plugins.credentials.inspect", "plugins.inspect", "plugins.install", "plugins.invokeRegistration", "plugins.listCapabilities", "plugins.reload", "plugins.skills.read", "plugins.uninstall"};
          if (pm.count(method)) { auto r = Plugins(deps, root, method, params); if (r.handled) return r; } }
        { static const std::set<std::string> sm{"sessions.activitySummary.ensure", "sessions.catalog.archive", "sessions.catalog.continue", "sessions.catalog.import",
              "sessions.catalog.startTerminal", "sessions.files.reveal", "sessions.github.options", "sessions.github.status", "sessions.pluginPatch",
              "sessions.providerReview.continue", "sessions.recover"};
          if (sm.count(method)) { auto r = Sessions2(deps, root, method, params); if (r.handled) return r; } }
        if (method.rfind("openclaw.", 0) == 0 || method.rfind("wizard.", 0) == 0 || method == "mcp.authLogin" || method == "models.authLogin") { auto r = SystemAgent(deps, root, method, params); if (r.handled) return r; }
        if (method.rfind("portal.", 0) == 0) { auto r = Portals(deps, method, params); if (r.handled) return r; }
        if (method.rfind("doctor.memory.", 0) == 0 && method != "doctor.memory.status") { auto r = DoctorMemory(deps, method, params); if (r.handled) return r; }
        if (method.rfind("gateway.suspend.", 0) == 0 || method == "gateway.stop.request" || (method.rfind("diagnostics.", 0) == 0 &&
            (method == "diagnostics.lanes" || method == "diagnostics.cpuProfile" || method == "diagnostics.heapProfile" || method == "diagnostics.heapSnapshot"))) {
            auto r = Lifecycle(deps, root, method, params); if (r.handled) return r; }
        if (method.rfind("push.", 0) == 0 && method != "push.list" && method != "push.status") { auto r = Push(deps, root, method, params); if (r.handled) return r; }
        if (method.rfind("users.authConnect.", 0) == 0) { auto r = AuthConnect(deps, root, method, params); if (r.handled) return r; }
        if (method.rfind("tools.github.", 0) == 0) { auto r = ToolsGitHub(deps, root, method, params); if (r.handled) return r; }
        if (method == "attach.grant" || method == "attach.revoke") { auto r = Attach(deps, method, params); if (r.handled) return r; }
        if (method.rfind("board.widget.", 0) == 0 || method == "board.event" || method == "board.prompt.authorize") { auto r = BoardWidgets(deps, root, method, params); if (r.handled) return r; }
        if (method.rfind("controlUi.", 0) == 0) { auto r = ControlUi(deps, root, method, params); if (r.handled) return r; }
        { static const std::set<std::string> sg{"conversations.send", "conversations.turn", "conversations.turn.cancel", "web.login.start", "web.login.wait", "worker.desktop.launch",
              "worker.desktop.observe", "catalog.browse", "catalog.searchKeywords", "session.discussion.info", "session.discussion.open", "ui.command", "config.openFile",
              "artifacts.download", "transcripts.summarize", "chat.metadata.changed", "nativeHook.invoke", "claws.monitors", "claws.packages.remove", "device.token.rotate",
              "device.scopes.requestUpgrade", "device.scopes.waitUpgrade", "migrations.memory.plan", "migrations.memory.apply"};
          if (sg.count(method)) { auto r = Singles(deps, root, method, params); if (r.handled) return r; } }
        if (method.rfind("talk.", 0) == 0) { auto r = Talk(deps, root, method, params); if (r.handled) return r; }
        if (method.rfind("mcp.app.", 0) == 0 && method != "mcp.app.listTools" && method != "mcp.app.callTool" && method != "mcp.app.listResources" &&
            method != "mcp.app.listResourceTemplates" && method != "mcp.app.readResource") { auto r = McpApp(deps, root, method, params); if (r.handled) return r; }
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
    { std::string extra_out; if (ExtraGatewayTool(name, a, agent.empty() ? std::string("main") : agent, session_key, d.config.raw, g_tool_root, &extra_out)) return extra_out; }
    session::Store& store = d.agent_pool ? d.agent_pool->Sessions(agent.empty() ? std::string("main") : agent) : d.sessions;
    // Accept the stored key, or the short form a caller used (k1 → agent:<agent>:k1 / any key ending in :k1).
    auto resolve = [&](const std::string& k) -> std::string {
        if (k.empty() || store.Exists(k)) return k;
        const std::string full = "agent:" + (agent.empty() ? std::string("main") : agent) + ":" + k;
        if (store.Exists(full)) return full;
        for (auto& s : store.List()) if (s.size() > k.size() && s.compare(s.size() - k.size() - 1, std::string::npos, ":" + k) == 0) return s;
        return k;
    };
    if (name == "sessions_list") {
        const int limit = std::clamp(a.value("limit", 50), 1, 200);
        json out = json::array();
        for (auto& k : store.List()) { if (static_cast<int>(out.size()) >= limit) break; out.push_back({{"key", k}, {"entry", store.IndexEntry(k)}}); }
        return json{{"sessions", out}}.dump();
    }
    if (name == "sessions_history") {
        const auto key = resolve(a.value("sessionKey", session_key));
        if (key.empty() || !store.Exists(key)) return fail("session not found: " + key);
        const int limit = std::clamp(a.value("limit", 20), 1, 200);
        const bool tools = a.value("includeTools", false);
        auto msgs = store.Load(key);
        json out = json::array();
        for (auto it = msgs.rbegin(); it != msgs.rend() && static_cast<int>(out.size()) < limit; ++it) {
            if (it->role == "system" || (!tools && it->role == "tool")) continue;
            out.insert(out.begin(), json{{"role", it->role}, {"content", it->content.substr(0, 4000)}});
        }
        return json{{"sessionKey", key}, {"messages", out}}.dump();
    }
    if (name == "session_status") {
        const auto key = resolve(a.value("sessionKey", session_key));
        if (key.empty() || !store.Exists(key)) return fail("session not found: " + key);
        json out{{"sessionKey", key}, {"entry", store.IndexEntry(key)}, {"messageCount", store.Load(key).size()}, {"agentId", agent}};
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
    // ---- tools that wrap gateway RPCs
    auto rpc = [&](const std::string& m, const json& p) -> std::string {
        auto r = GatewayRpc(m, p);
        if (r.value("ok", false)) return r.value("payload", json::object()).dump();
        auto e = r.contains("error") ? r["error"] : json("error");
        return fail(e.is_object() ? e.value("message", e.dump()) : e.is_string() ? e.get<std::string>() : e.dump());
    };
    const std::string me = agent.empty() ? std::string("main") : agent;
    if (name == "sessions_send") {
        const auto msg = a.value("message", std::string());
        auto key = a.value("sessionKey", a.value("label", std::string()));
        if (msg.empty() || key.empty()) return fail("sessionKey and message are required");
        key = resolve(key);
        if (key == session_key) return fail("cannot send to the current session");
        return rpc("chat.send", {{"sessionKey", key}, {"message", msg}, {"agentId", a.value("agentId", me)}, {"sender", "agent:" + me}});
    }
    if (name == "sessions_spawn") {
        const auto task = a.value("task", std::string());
        if (task.empty()) return fail("task is required");
        const auto label = a.value("label", a.value("taskName", std::string("subagent")));
        const auto key = "agent:" + a.value("agentId", me) + ":subagent:" + label + "-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 1000000000LL);
        auto reg = ReadJsonFile(fs::path(g_tool_root) / "subagents.json", json::object());
        reg[key] = {{"parent", session_key}, {"label", label}, {"task", task}, {"status", "running"}, {"startedAt", NowMs()}};
        WriteJsonFile(fs::path(g_tool_root) / "subagents.json", reg);
        json p{{"sessionKey", key}, {"message", task}, {"agentId", a.value("agentId", me)}};
        if (a.contains("model")) p["model"] = a["model"];
        auto r = GatewayRpc("chat.send", p);
        reg = ReadJsonFile(fs::path(g_tool_root) / "subagents.json", json::object());
        reg[key]["status"] = r.value("ok", false) ? "done" : "failed";
        reg[key]["result"] = r.value("ok", false) ? r["payload"].value("text", "") : r.value("error", json()).dump();
        reg[key]["endedAt"] = NowMs();
        WriteJsonFile(fs::path(g_tool_root) / "subagents.json", reg);
        return json{{"status", reg[key]["status"]}, {"runId", key}, {"childSessionKey", key}, {"result", reg[key]["result"]}}.dump();
    }
    if (name == "subagents") {
        auto reg = ReadJsonFile(fs::path(g_tool_root) / "subagents.json", json::object());
        const auto action = a.value("action", std::string("list"));
        json out = json::array();
        for (auto& [k, v] : reg.items())
            if (action == "list" ? v.value("parent", "") == session_key || a.value("all", false) : true) { json x = v; x["runId"] = k; out.push_back(x); }
        if (action == "wait" || action == "status") {
            json ids = a.contains("runIds") ? a["runIds"] : json::array({a.value("runId", std::string())});
            json res = json::array();
            for (auto& id : ids) if (id.is_string() && reg.contains(id.get<std::string>())) { json x = reg[id.get<std::string>()]; x["runId"] = id; res.push_back(x); }
            return json{{"runs", res}}.dump();
        }
        return json{{"runs", out}}.dump();
    }
    if (name == "sessions_yield") {
        return json{{"yielded", true}, {"message", a.value("message", std::string())}, {"waitFor", a.value("waitFor", json())},
                    {"note", "turn yields; the next inbound message resumes this session"}}.dump();
    }
    if (name == "sessions_search") {
        auto q = a.value("query", std::string());
        if (q.empty()) return fail("query is required");
        std::string ql = q; std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);
        const int limit = std::clamp(a.value("limit", 10), 1, 50);
        json res = json::array();
        for (auto& k : store.List()) {
            auto msgs = store.Load(k);
            for (size_t i = 0; i < msgs.size() && static_cast<int>(res.size()) < limit; ++i) {
                if (msgs[i].role == "system") continue;
                std::string c = msgs[i].content, cl = c; std::transform(cl.begin(), cl.end(), cl.begin(), ::tolower);
                auto pos = cl.find(ql);
                if (pos == std::string::npos) continue;
                const size_t s = pos > 60 ? pos - 60 : 0;
                res.push_back({{"sessionKey", k}, {"messageId", i}, {"role", msgs[i].role}, {"snippet", c.substr(s, 160)}});
            }
        }
        return json{{"status", "ok"}, {"query", q}, {"results", res}}.dump();
    }
    if (name == "message") {
        const auto action = a.value("action", std::string("send"));
        if (action == "send")
        {
            json sp{{"channel", a.value("channel", std::string())}, {"to", a.value("to", a.value("target", std::string()))},
                    {"message", a.value("message", a.value("text", std::string()))}};
            for (auto k : {"buttons", "media", "message_thread_id"}) if (a.contains(k)) sp[k] = a[k];  // 1.135: inline keyboards + attachments
            if (a.contains("path") && !a.contains("media")) sp["media"] = a["path"];
            return rpc("send", sp);
        }
        json p = a; p["action"] = action;
        return rpc("message.action", p);
    }
    if (name == "automations") {
        const auto action = a.value("action", std::string("list"));
        if (action == "list") return rpc("cron.list", json::object());
        if (action == "add" || action == "create")
            return rpc("cron.add", {{"id", a.value("id", "auto-" + std::to_string(NowMs()))}, {"cron", a.value("schedule", a.value("cron", std::string()))},
                                    {"payload", a.value("message", a.value("prompt", std::string()))}, {"enabled", a.value("enabled", true)}});
        if (action == "remove" || action == "delete") return rpc("cron.remove", {{"id", a.value("id", std::string())}});
        if (action == "run") return rpc("cron.run", {{"id", a.value("id", std::string())}});
        return fail("action must be list, add, remove or run");
    }
    if (name == "reminder") {
        const auto text = a.value("text", a.value("message", std::string()));
        if (text.empty()) return fail("text is required");
        long long at = a.value("atMs", 0LL);
        if (!at && a.contains("inSeconds")) at = NowMs() + a.value("inSeconds", 0LL) * 1000;
        if (!at) return fail("atMs or inSeconds is required");
        std::time_t tt = at / 1000; std::tm tm{}; localtime_r(&tt, &tm);
        char expr[64]; std::snprintf(expr, sizeof expr, "%d %d %d %d *", tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon + 1);
        const auto id = "reminder-" + std::to_string(at);
        auto r = rpc("cron.add", {{"id", id}, {"cron", expr}, {"payload", "Reminder: " + text}, {"enabled", true}});
        return json{{"id", id}, {"cron", expr}, {"atMs", at}, {"text", text}, {"cron_result", json::parse(r)}}.dump();
    }
    if (name == "tts") {
        const auto text = a.value("text", std::string());
        if (text.empty()) return fail("text is required");
        return rpc("tts.convert", {{"text", text}, {"channel", a.value("channel", std::string())}});
    }
    if (name == "presence") return rpc("system-presence", json::object());
    if (name == "skills_search") return rpc("skills.search", {{"query", a.value("query", std::string())}});
    if (name == "skills_read") {
        const auto nm = a.value("name", std::string());
        if (nm.empty() || nm.find("..") != std::string::npos || nm.find('/') != std::string::npos) return fail("valid skill name required");
        auto st = GatewayRpc("skills.status", json::object());
        for (auto& s : st.value("payload", json::object()).value("skills", json::array())) {
            if (s.value("name", "") != nm) continue;
            std::string path = s.value("path", s.value("filePath", s.value("baseDir", std::string())));
            if (fs::is_directory(path)) path = (fs::path(path) / "SKILL.md").string();
            std::ifstream in(path); std::stringstream ss; ss << in.rdbuf();
            if (!in) return fail("skill file unreadable: " + path);
            return json{{"name", nm}, {"path", path}, {"content", ss.str().substr(0, 60000)}}.dump();
        }
        return fail("skill not found: " + nm);
    }
    if (name == "docs_search") {
        const auto q = a.value("query", std::string());
        if (q.empty()) return fail("query is required");
        std::string ql = q; std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);
        std::vector<fs::path> roots;
        if (const char* d = std::getenv("OPENCLAW_DOCS_DIR")) roots.emplace_back(d);
        roots.emplace_back(fs::path(g_tool_root) / "docs");
        json res = json::array();
        for (auto& r : roots) {
            std::error_code ec;
            if (!fs::is_directory(r, ec)) continue;
            for (auto it = fs::recursive_directory_iterator(r, ec); it != fs::recursive_directory_iterator() && res.size() < 10; it.increment(ec)) {
                if (!it->is_regular_file() || (it->path().extension() != ".md" && it->path().extension() != ".mdx")) continue;
                std::ifstream in(it->path()); std::stringstream ss; ss << in.rdbuf(); std::string c = ss.str(), cl = c;
                std::transform(cl.begin(), cl.end(), cl.begin(), ::tolower);
                auto pos = cl.find(ql);
                if (pos == std::string::npos) continue;
                res.push_back({{"path", fs::relative(it->path(), r).string()}, {"snippet", c.substr(pos > 80 ? pos - 80 : 0, 240)}});
            }
        }
        return json{{"query", q}, {"results", res}, {"roots", json::array()}}.dump();
    }
    if (name == "create_goal" || name == "get_goal" || name == "update_goal") {
        static std::mutex goals_mu;
        std::lock_guard lk(goals_mu);
        const auto path = fs::path(g_tool_root) / "goals.json";
        auto db = ReadJsonFile(path, json::object());
        const auto key = a.value("sessionKey", session_key);
        if (name == "get_goal") return db.contains(key) ? db[key].dump() : json{{"goal", nullptr}}.dump();
        if (name == "create_goal") {
            const auto obj = a.value("objective", a.value("goal", std::string()));
            if (obj.empty()) return fail("objective is required");
            db[key] = {{"objective", obj}, {"status", "active"}, {"createdAt", NowMs()}, {"successCriteria", a.value("successCriteria", json::array())}};
        } else {
            if (!db.contains(key)) return fail("no goal for this session");
            for (auto f : {"status", "objective", "progress", "notes"}) if (a.contains(f)) db[key][f] = a[f];
            db[key]["updatedAt"] = NowMs();
        }
        WriteJsonFile(path, db);
        return db[key].dump();
    }
    if (name == "transcripts") {
        const auto path = fs::path(g_tool_root) / "transcripts.json";
        auto db = ReadJsonFile(path, json::object());
        const auto action = a.value("action", std::string("list"));
        if (action == "list") { json out = json::array(); for (auto& [k, v] : db.items()) out.push_back({{"id", k}, {"title", v.value("title", "")}, {"createdAt", v.value("createdAt", 0LL)}}); return json{{"transcripts", out}}.dump(); }
        if (action == "read" || action == "get") { auto id = a.value("id", a.value("sessionId", std::string())); return db.contains(id) ? db[id].dump() : fail("transcript not found: " + id); }
        if (action == "save" || action == "create") {
            const auto tx = a.value("transcript", std::string());
            if (tx.empty()) return fail("transcript is required");
            const auto id = "tx-" + std::to_string(NowMs());
            db[id] = {{"title", a.value("title", std::string("transcript"))}, {"transcript", tx}, {"createdAt", NowMs()}, {"sessionKey", session_key}};
            WriteJsonFile(path, db);
            return json{{"id", id}}.dump();
        }
        return fail("action must be list, read or save (live meeting capture is not available offline)");
    }
    if (name == "secrets") {
        const auto nm = a.value("name", std::string());
        auto lst = GatewayRpc("secrets.list", json::object());
        if (nm.empty()) return lst.value("ok", false) ? lst["payload"].dump() : fail("secrets.list failed");
        // Never return secret values to the model; report availability only.
        bool have = lst.value("payload", json::object()).dump().find("\"" + nm + "\"") != std::string::npos;
        return json{{"name", nm}, {"available", have}, {"reason", a.value("reason", std::string())}, {"note", "values are never returned to the model"}}.dump();
    }
    if (name == "image_generate" || name == "video_generate" || name == "music_generate")
        return GenerateMedia(name.substr(0, name.find('_')), a).dump();
    if (name == "image" || name == "pdf") {
        json paths = a.contains("paths") ? a["paths"] : a.contains("pdfs") ? a["pdfs"] : json::array({a.value("path", a.value("pdf", a.value("image", std::string())))});
        json out = json::array();
        for (auto& pj : paths) {
            if (!pj.is_string() || pj.get<std::string>().empty()) continue;
            const fs::path pth = pj.get<std::string>();
            std::error_code ec;
            if (!fs::is_regular_file(pth, ec)) { out.push_back({{"path", pth.string()}, {"error", "not found"}}); continue; }
            json item{{"path", pth.string()}, {"bytes", fs::file_size(pth, ec)}};
            if (name == "pdf") {
                const std::string cmd = "pdftotext -layout -q '" + std::regex_replace(pth.string(), std::regex("'"), "'\\''") + "' - 2>/dev/null";
                std::string text; if (FILE* f = popen(cmd.c_str(), "r")) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() < 200000) text.append(buf, n); pclose(f); }
                item["text"] = text; if (text.empty()) item["error"] = "no text extracted (pdftotext missing or image-only pdf)";
            } else {
                std::ifstream in(pth, std::ios::binary); unsigned char h[24] = {0}; in.read(reinterpret_cast<char*>(h), 24);
                if (h[0] == 0x89 && h[1] == 'P') { item["format"] = "png"; item["width"] = (h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19]; item["height"] = (h[20] << 24) | (h[21] << 16) | (h[22] << 8) | h[23]; }
                else if (h[0] == 0xFF && h[1] == 0xD8) item["format"] = "jpeg";
                else if (h[0] == 'G' && h[1] == 'I') item["format"] = "gif";
                else if (h[8] == 'W' && h[9] == 'E') item["format"] = "webp";
                item["description"] = "vision analysis requires a vision-capable provider; returning file metadata";
            }
            out.push_back(item);
        }
        if (out.empty()) return fail("path is required");
        return json{{"prompt", a.value("prompt", std::string())}, {"items", out}}.dump();
    }
    return fail("unknown gateway tool " + name);
}
}  // namespace openclaw::gateway

namespace openclaw::gateway {
namespace {
std::function<nlohmann::json(const std::string&, const nlohmann::json&)> g_gw_rpc;
std::mutex g_goals_mu;
}
std::function<void(const std::string&, const json&)> g_gw_emit;
void SetGatewayEmitter(std::function<void(const std::string&, const json&)> fn) { g_gw_emit = std::move(fn); }
void GatewayEmit(const std::string& e, const json& p) { if (g_gw_emit) g_gw_emit(e, p); }
void SetGatewayRpc(std::function<nlohmann::json(const std::string&, const nlohmann::json&)> fn) { g_gw_rpc = std::move(fn); }
nlohmann::json GatewayRpc(const std::string& m, const nlohmann::json& p) {
    if (!g_gw_rpc) return {{"ok", false}, {"error", {{"message", "gateway not running"}}}};
    return g_gw_rpc(m, p);
}
std::vector<std::string> GatewayToolNames() {
    return {"sessions_list", "sessions_history", "session_status", "agents_list", "nodes", "sessions_send", "sessions_spawn", "sessions_yield",
            "sessions_search", "subagents", "message", "automations", "reminder", "image", "pdf", "tts", "image_generate", "video_generate",
            "music_generate", "create_goal", "get_goal", "update_goal", "docs_search", "skills_search", "skills_read", "transcripts", "secrets", "presence",
            "web_search", "ask_user", "view_image", "agents_wait", "conversations_list", "conversations_send", "conversations_turn",
            "computer", "dashboard", "decision_evaluate", "gateway", "github_identity_status", "mobile_ui", "personal_instructions",
            "plugins", "portal", "progress_card", "screen", "skill_workshop", "terminal"};
}

nlohmann::json GenerateMedia(const std::string& kind, const nlohmann::json& a) {
    using nlohmann::json;
    const auto prompt = a.value("prompt", std::string());
    if (prompt.empty()) return {{"ok", false}, {"error", "prompt is required"}};
    // 1.139: prefer plugin image/video/music generation providers when registered.
    {
        const std::string kind_reg = kind == "image" ? "registerImageGenerationProvider"
                                    : kind == "video" ? "registerVideoGenerationProvider"
                                    : kind == "music" ? "registerMusicGenerationProvider" : "";
        if (!kind_reg.empty() && g_tool_deps && g_tool_deps->js_host) {
            auto r = plugins::InvokeRegistration(g_tool_deps->js_host, kind_reg, a, "generate");
            if (r && r->value("ok", false) && r->contains("result")) {
                json out = r->value("result", json::object());
                if (!out.is_object()) out = {{"data", out}};
                out["ok"] = true; out["provider"] = "plugin"; out["plugin"] = r->value("plugin", ""); out["kind"] = kind;
                return out;
            }
        }
    }
    const char* prov = std::getenv("OPENCLAW_MEDIA_PROVIDER");
    const char* def = std::getenv("OPENCLAW_DEFAULT_PROVIDER");
    const std::string provider = prov ? prov : (def && std::string(def) == "mock" ? "mock" : "");
    if (provider != "mock")
        return {{"ok", false}, {"error", kind + " generation needs a provider (set OPENCLAW_MEDIA_PROVIDER; only 'mock' is built in)"}, {"code", "UNAVAILABLE"}};
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(g_tool_root) / "media" / "generated";
    fs::create_directories(dir);
    const int count = std::clamp(a.value("count", 1), 1, 4);
    const std::string ext = kind == "image" ? ".png" : kind == "video" ? ".mp4" : ".wav";
    json files = json::array();
    const auto h = std::hash<std::string>{}(kind + prompt);
    for (int i = 0; i < count; ++i) {
        const fs::path f = dir / (kind + "-" + std::to_string(h % 1000000007ULL) + "-" + std::to_string(i) + ext);
        std::ofstream o(f, std::ios::binary);
        if (kind == "image") {  // 1x1 PNG
            static const unsigned char png[] = {0x89,'P','N','G',13,10,26,10,0,0,0,13,'I','H','D','R',0,0,0,1,0,0,0,1,8,6,0,0,0,0x1f,0x15,0xc4,0x89,0,0,0,13,'I','D','A','T',0x78,0x9c,0x63,0xf8,0x0f,0,0,0x01,0x01,0,0x05,0x18,0xd8,0x4e,0,0,0,0,'I','E','N','D',0xae,0x42,0x60,0x82};
            o.write(reinterpret_cast<const char*>(png), sizeof png);
        } else if (kind == "music") {  // 0.1 s silent 8 kHz mono WAV
            const uint32_t n = 800; std::string w = "RIFF"; auto u32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) w.push_back(char(v >> (8 * k))); };
            auto u16 = [&](uint16_t v) { w.push_back(char(v)); w.push_back(char(v >> 8)); };
            u32(36 + n); w += "WAVEfmt "; u32(16); u16(1); u16(1); u32(8000); u32(8000); u16(1); u16(8); w += "data"; u32(n); w.append(n, char(128));
            o << w;
        } else {
            o << "mock-video:" << prompt;  // placeholder container; not playable
        }
        files.push_back(f.string());
    }
    return {{"ok", true}, {"provider", "mock"}, {"kind", kind}, {"prompt", prompt}, {"files", files}};
}
}  // namespace openclaw::gateway
