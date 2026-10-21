#include "openclaw/acp/acpx.hpp"

#include "openclaw/voice/stt.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <map>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace openclaw::acp::acpx {
namespace fs = std::filesystem;
namespace {

std::string B64Decode(const std::string& in) {
    static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : in) {
        auto pos = tbl.find(static_cast<char>(c));
        if (pos == std::string::npos) continue;
        val = (val << 6) + static_cast<int>(pos); bits += 6;
        if (bits >= 0) { out.push_back(static_cast<char>((val >> bits) & 0xff)); bits -= 8; }
    }
    return out;
}

// Path must be absolute and resolve (weakly, symlinks followed) inside root.
bool Inside(const fs::path& root, const std::string& p, fs::path* out) {
    if (p.empty() || p[0] != '/') return false;
    std::error_code ec;
    auto r = fs::weakly_canonical(root, ec);
    auto c = fs::weakly_canonical(fs::path(p), ec);
    if (ec) return false;
    auto rs = r.string(), cs = c.string();
    if (cs != rs && cs.rfind(rs + "/", 0) != 0) return false;
    *out = c;
    return true;
}

struct Proc {
    pid_t pid = -1;
    int in = -1, out = -1;
    std::string buf;
    bool Spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::vector<std::string>& env, std::string* err) {
        int a[2], b[2];
        if (pipe(a) != 0 || pipe(b) != 0) { *err = "pipe"; return false; }
        pid = fork();
        if (pid < 0) { *err = "fork"; return false; }
        if (pid == 0) {
            dup2(a[0], 0); dup2(b[1], 1);
            close(a[0]); close(a[1]); close(b[0]); close(b[1]);
            if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
            for (auto& e : env) putenv(const_cast<char*>(e.c_str()));
            std::vector<char*> av;
            for (auto& s : argv) av.push_back(const_cast<char*>(s.c_str()));
            av.push_back(nullptr);
            execvp(av[0], av.data());
            _exit(127);
        }
        close(a[0]); close(b[1]);
        in = a[1]; out = b[0];
        signal(SIGPIPE, SIG_IGN);
        return true;
    }
    bool Write(const nlohmann::json& j) {
        auto s = j.dump() + "\n";
        const char* p = s.data(); size_t n = s.size();
        while (n) { auto w = ::write(in, p, n); if (w <= 0) return false; p += w; n -= static_cast<size_t>(w); }
        return true;
    }
    // -1 timeout, 0 EOF, 1 line
    int ReadLine(std::string& line, int timeout_ms) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            auto nl = buf.find('\n');
            if (nl != std::string::npos) { line = buf.substr(0, nl); buf.erase(0, nl + 1); return 1; }
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            if (left <= 0) return -1;
            pollfd pf{out, POLLIN, 0};
            if (poll(&pf, 1, static_cast<int>(std::min<long long>(left, 1000))) <= 0) continue;
            char tmp[65536];
            auto r = ::read(out, tmp, sizeof tmp);
            if (r <= 0) return 0;
            buf.append(tmp, static_cast<size_t>(r));
        }
    }
    int Close() {
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        int st = 0;
        if (pid > 0) {
            for (int i = 0; i < 20; ++i) { if (waitpid(pid, &st, WNOHANG) == pid) return st; usleep(50000); }
            kill(pid, SIGTERM); usleep(100000); kill(pid, SIGKILL); waitpid(pid, &st, 0);
        }
        return st;
    }
};

struct Term {
    pid_t pid = -1;
    int out = -1;
    std::string buf;
    size_t limit = 1 << 20;
    bool truncated = false, exited = false;
    int status = 0;
    void Drain(int timeout_ms) {
        if (out < 0) return;
        pollfd pf{out, POLLIN, 0};
        while (poll(&pf, 1, timeout_ms) > 0) {
            char tmp[8192];
            auto r = ::read(out, tmp, sizeof tmp);
            if (r <= 0) { close(out); out = -1; break; }
            buf.append(tmp, static_cast<size_t>(r));
            if (buf.size() > limit) {  // keep the tail, cut at a char boundary (ACP spec)
                size_t cut = buf.size() - limit;
                while (cut < buf.size() && (static_cast<unsigned char>(buf[cut]) & 0xC0) == 0x80) ++cut;
                buf.erase(0, cut); truncated = true;
            }
            timeout_ms = 0;
        }
    }
    void Reap(bool block) {
        if (exited || pid <= 0) return;
        int st = 0;
        if (waitpid(pid, &st, block ? 0 : WNOHANG) == pid) { exited = true; status = st; }
    }
    nlohmann::json ExitJson() const {
        if (!exited) return nullptr;
        if (WIFSIGNALED(status)) return {{"exitCode", nullptr}, {"signal", std::string(strsignal(WTERMSIG(status)))}};
        return {{"exitCode", WEXITSTATUS(status)}, {"signal", nullptr}};
    }
};

}  // namespace

nlohmann::json Presets() {
    using A = nlohmann::json;
    nlohmann::json o = nlohmann::json::object();
    o["codex"] = A::array({"npx", "-y", "@zed-industries/codex-acp"});
    o["claude"] = A::array({"npx", "-y", "@zed-industries/claude-code-acp"});
    o["gemini"] = A::array({"gemini", "--experimental-acp"});
    o["openclaw"] = A::array({"openclaw-cpp", "acp-stdio"});
    return o;
}

std::vector<std::string> ResolveCommand(const nlohmann::json& cfg, const std::string& agent, std::string* err) {
    std::vector<std::string> out;
    const nlohmann::json* a = nullptr;
    if (cfg.is_object() && cfg.contains("acp") && cfg["acp"].is_object() && cfg["acp"].contains("agents") &&
        cfg["acp"]["agents"].is_object() && cfg["acp"]["agents"].contains(agent))
        a = &cfg["acp"]["agents"][agent];
    if (a && a->is_object() && a->contains("command")) {
        const auto& c = (*a)["command"];
        if (c.is_string()) out.push_back(c.get<std::string>());
        else if (c.is_array()) for (auto& x : c) out.push_back(x.get<std::string>());
        if (a->contains("args") && (*a)["args"].is_array()) for (auto& x : (*a)["args"]) out.push_back(x.get<std::string>());
    } else if (const auto pre = Presets(); pre.contains(agent)) {
        for (auto& x : pre[agent]) out.push_back(x.get<std::string>());
        if (agent == "openclaw") {
            char self[4096]; auto n = readlink("/proc/self/exe", self, sizeof self - 1);
            if (n > 0) { self[n] = 0; out[0] = self; }
        }
    } else if (err) *err = "unknown ACP agent '" + agent + "' (configure acp.agents." + agent + ".command)";
    return out;
}

nlohmann::json Run(const RunSpec& spec) {
    nlohmann::json rep{{"ok", false}, {"agent", spec.agent}};
    if (spec.argv.empty()) { rep["error"] = "no command"; return rep; }
    const fs::path root = spec.cwd.empty() ? fs::current_path() : fs::path(spec.cwd);
    rep["argv"] = spec.argv;
    Proc p;
    std::string err;
    if (!p.Spawn(spec.argv, root.string(), spec.env, &err)) { rep["error"] = err; return rep; }
    int next_id = 1;
    std::string text;
    nlohmann::json updates = nlohmann::json::array(), tool_calls = nlohmann::json::object(), perms = nlohmann::json::array(), fsops = nlohmann::json::array();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(spec.timeout_ms);
    auto left = [&] { return static_cast<int>(std::max<long long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count())); };

    std::map<std::string, Term> terms;
    int term_seq = 0;
    nlohmann::json termlog = nlohmann::json::array();
    auto handle_agent_request = [&](const nlohmann::json& m) {
        const auto method = m.value("method", std::string());
        const auto& prm = m.contains("params") ? m["params"] : nlohmann::json::object();
        nlohmann::json resp{{"jsonrpc", "2.0"}, {"id", m["id"]}};
        if (method == "session/request_permission") {
            std::string chosen;
            for (auto& o : prm.value("options", nlohmann::json::array())) {
                const auto kind = o.value("kind", std::string());
                if (spec.permission == "deny" && kind.rfind("reject", 0) == 0 && chosen.empty()) chosen = o.value("optionId", "");
                if (spec.permission == "allow_once" && kind == "allow_once") chosen = o.value("optionId", "");
                if (spec.permission == "allow_always" && (kind == "allow_always" || (kind == "allow_once" && chosen.empty()))) chosen = o.value("optionId", "");
            }
            resp["result"] = chosen.empty() ? nlohmann::json{{"outcome", {{"outcome", "cancelled"}}}}
                                            : nlohmann::json{{"outcome", {{"outcome", "selected"}, {"optionId", chosen}}}};
            perms.push_back({{"toolCall", prm.value("toolCall", nlohmann::json::object())}, {"policy", spec.permission}, {"optionId", chosen}});
        } else if (method == "fs/read_text_file") {
            fs::path f;
            if (!Inside(root, prm.value("path", std::string()), &f)) resp["error"] = {{"code", -32602}, {"message", "path outside session cwd"}};
            else {
                std::ifstream in(f);
                if (!in) resp["error"] = {{"code", -32002}, {"message", "not found"}};
                else {
                    std::string content, l; int ln = 0;
                    const int from = prm.value("line", 1), lim = prm.value("limit", 1 << 30);
                    int taken = 0;
                    while (std::getline(in, l)) { ++ln; if (ln >= from && taken < lim) { content += l + "\n"; ++taken; } }
                    resp["result"] = {{"content", content}};
                }
            }
            fsops.push_back({{"op", "read"}, {"path", prm.value("path", "")}, {"ok", !resp.contains("error")}});
        } else if (method == "fs/write_text_file") {
            fs::path f;
            if (!spec.fs_write) resp["error"] = {{"code", -32000}, {"message", "fs writes disabled by openclaw policy"}};
            else if (!Inside(root, prm.value("path", std::string()), &f)) resp["error"] = {{"code", -32602}, {"message", "path outside session cwd"}};
            else { fs::create_directories(f.parent_path()); std::ofstream(f) << prm.value("content", std::string()); resp["result"] = nullptr; }
            fsops.push_back({{"op", "write"}, {"path", prm.value("path", "")}, {"ok", !resp.contains("error")}});
        } else if (method.rfind("terminal/", 0) == 0 && spec.terminal) {
            const auto tid = prm.value("terminalId", std::string());
            auto it = terms.find(tid);
            if (method == "terminal/create") {
                fs::path wd = root;
                if (prm.contains("cwd") && prm["cwd"].is_string() && !Inside(root, prm["cwd"].get<std::string>(), &wd))
                    resp["error"] = {{"code", -32602}, {"message", "terminal cwd outside session cwd"}};
                else if (terms.size() >= 16) resp["error"] = {{"code", -32000}, {"message", "too many terminals"}};
                else {
                    std::vector<std::string> argv{prm.value("command", std::string())};
                    for (auto& a : prm.value("args", nlohmann::json::array())) if (a.is_string()) argv.push_back(a.get<std::string>());
                    std::vector<std::string> envs;
                    for (auto& e : prm.value("env", nlohmann::json::array()))
                        if (e.is_object()) envs.push_back(e.value("name", std::string()) + "=" + e.value("value", std::string()));
                    int pp[2];
                    if (argv[0].empty() || pipe(pp) != 0) resp["error"] = {{"code", -32602}, {"message", "command required"}};
                    else {
                        pid_t pid = fork();
                        if (pid == 0) {
                            setpgid(0, 0);
                            int dn = open("/dev/null", O_RDONLY); dup2(dn, 0);
                            dup2(pp[1], 1); dup2(pp[1], 2); close(pp[0]); close(pp[1]);
                            if (chdir(wd.c_str()) != 0) _exit(126);
                            for (auto& e : envs) putenv(const_cast<char*>(e.c_str()));
                            std::vector<char*> av; for (auto& s : argv) av.push_back(const_cast<char*>(s.c_str())); av.push_back(nullptr);
                            execvp(av[0], av.data());
                            _exit(127);
                        }
                        setpgid(pid, pid);  // race-free: both sides set the group
                        close(pp[1]);
                        Term tm; tm.pid = pid; tm.out = pp[0];
                        if (prm.contains("outputByteLimit") && prm["outputByteLimit"].is_number_unsigned()) tm.limit = std::min<size_t>(prm["outputByteLimit"].get<size_t>(), 16u << 20);
                        const std::string id = "term-" + std::to_string(++term_seq);
                        terms[id] = std::move(tm);
                        resp["result"] = {{"terminalId", id}};
                        termlog.push_back({{"terminalId", id}, {"argv", argv}, {"cwd", wd.string()}});
                    }
                }
            } else if (it == terms.end()) {
                resp["error"] = {{"code", -32602}, {"message", "unknown terminalId"}};
            } else if (method == "terminal/output") {
                it->second.Drain(0); it->second.Reap(false);
                resp["result"] = {{"output", it->second.buf}, {"truncated", it->second.truncated}, {"exitStatus", it->second.ExitJson()}};
            } else if (method == "terminal/wait_for_exit") {
                auto& tm = it->second;
                while (!tm.exited && tm.out >= 0 && left() > 1) { tm.Drain(std::min(200, left())); tm.Reap(false); }
                if (!tm.exited) { tm.Reap(false); if (!tm.exited && left() <= 1) { resp["error"] = {{"code", -32000}, {"message", "timeout"}}; } else tm.Reap(true); }
                if (!resp.contains("error")) { tm.Drain(0); resp["result"] = tm.ExitJson(); }
            } else if (method == "terminal/kill") {
                if (!it->second.exited) { (kill(-it->second.pid, SIGKILL) == 0 || kill(it->second.pid, SIGKILL) == 0); it->second.Reap(true); }
                resp["result"] = nullptr;
            } else if (method == "terminal/release") {
                auto& tm = it->second;
                if (!tm.exited) { if (kill(-tm.pid, SIGKILL) != 0) kill(tm.pid, SIGKILL); tm.Reap(true); }
                if (tm.out >= 0) close(tm.out);
                for (auto& l : termlog) if (l["terminalId"] == tid) { l["exit"] = tm.ExitJson(); l["output_bytes"] = tm.buf.size(); }
                terms.erase(it);
                resp["result"] = nullptr;
            } else resp["error"] = {{"code", -32601}, {"message", "unknown terminal method"}};
        } else {
            resp["error"] = {{"code", -32601}, {"message", "client method not supported: " + method}};
        }
        p.Write(resp);
    };
    auto on_update = [&](const nlohmann::json& prm) {
        const auto& u = prm.value("update", nlohmann::json::object());
        const auto kind = u.value("sessionUpdate", std::string());
        if (kind == "agent_message_chunk" && u.contains("content") && u["content"].is_object() && u["content"].value("type", "") == "text")
            text += u["content"].value("text", "");
        else if (kind == "tool_call" || kind == "tool_call_update") {
            const auto id = u.value("toolCallId", std::string());
            auto& t = tool_calls[id];
            if (t.is_null()) t = nlohmann::json::object();
            for (auto& [k, v] : u.items()) if (k != "sessionUpdate") t[k] = v;
        }
        if (updates.size() < 200) updates.push_back(u);
    };
    // Send request, pump until its response, serving agent requests + notifications meanwhile.
    auto call = [&](const std::string& method, const nlohmann::json& params) -> nlohmann::json {
        const int id = next_id++;
        if (!p.Write({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}})) return {{"error", {{"message", "write failed"}}}};
        std::string line;
        for (;;) {
            int r = p.ReadLine(line, left());
            if (r == -1) return {{"error", {{"message", "timeout waiting for " + method}}}};
            if (r == 0) return {{"error", {{"message", "agent exited during " + method}}}};
            nlohmann::json m;
            try { m = nlohmann::json::parse(line); } catch (...) { continue; }  // agents may log to stdout
            if (m.contains("method") && m.contains("id")) { handle_agent_request(m); continue; }
            if (m.contains("method")) { if (m["method"] == "session/update") on_update(m.value("params", nlohmann::json::object())); continue; }
            if (m.contains("id") && m["id"] == id) return m;
        }
    };
    auto finish = [&](nlohmann::json r) {
        r["text"] = text; r["updates"] = updates; r["permissions"] = perms; r["fs"] = fsops;
        nlohmann::json tc = nlohmann::json::array();
        for (auto& [k, v] : tool_calls.items()) { auto x = v; x["toolCallId"] = k; tc.push_back(x); }
        r["tool_calls"] = tc;
        for (auto& [id, tm] : terms) { if (!tm.exited) { if (kill(-tm.pid, SIGKILL) != 0) kill(tm.pid, SIGKILL); tm.Reap(true); } if (tm.out >= 0) close(tm.out); }
        if (!termlog.empty()) r["terminals"] = termlog;
        const int st = p.Close();
        r["exit"] = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        return r;
    };

    auto init = call("initialize", {{"protocolVersion", 1},
                                    {"clientCapabilities", {{"fs", {{"readTextFile", true}, {"writeTextFile", spec.fs_write}}}, {"terminal", spec.terminal}}},
                                    {"clientInfo", {{"name", "openclaw-cpp"}, {"version", "acpx"}}}});
    if (init.contains("error")) { rep["error"] = "initialize: " + init["error"].value("message", std::string()); return finish(rep); }
    const auto& ir = init["result"];
    rep["agentInfo"] = ir.value("agentInfo", nlohmann::json());
    rep["protocolVersion"] = ir.value("protocolVersion", 0);
    const auto caps = ir.value("agentCapabilities", nlohmann::json::object());
    const bool audio_ok = caps.value("promptCapabilities", nlohmann::json::object()).value("audio", false);
    if (ir.contains("authMethods") && ir["authMethods"].is_array() && !ir["authMethods"].empty()) rep["authMethods"] = ir["authMethods"];

    // Audio blocks: pass through when supported, else transcribe locally.
    nlohmann::json prompt = nlohmann::json::array();
    nlohmann::json audio_rep = nlohmann::json::array();
    for (auto b : spec.prompt) {
        if (b.is_object() && b.value("type", "") == "audio" && !audio_ok) {
            auto raw = B64Decode(b.value("data", std::string()));
            const auto mime = b.value("mimeType", std::string("audio/wav"));
            auto tr = voice::DefaultStt().Transcribe(raw, mime, "audio." + mime.substr(mime.find('/') + 1), "");
            const auto t = tr.value("text", std::string());
            audio_rep.push_back({{"mode", "transcribed_locally"}, {"ok", !t.empty()}});
            b = {{"type", "text"}, {"text", t.empty() ? std::string("[audio: transcription unavailable]") : "[Audio] Transcript: " + t}};
        } else if (b.is_object() && b.value("type", "") == "audio") audio_rep.push_back({{"mode", "passthrough"}});
        prompt.push_back(b);
    }
    if (!audio_rep.empty()) rep["audio"] = audio_rep;

    const bool can_load = caps.value("loadSession", false);
    const nlohmann::json auth_methods = ir.value("authMethods", nlohmann::json::array());
    auto authenticate = [&]() -> bool {
        std::string mid = spec.auth_method;
        if (mid == "auto" && auth_methods.is_array() && !auth_methods.empty()) mid = auth_methods[0].value("id", std::string());
        bool known = false;
        for (auto& m : auth_methods) if (m.value("id", std::string()) == mid) known = true;
        if (!known) { rep["error"] = "auth method '" + mid + "' not advertised by agent"; return false; }
        auto ar = call("authenticate", {{"methodId", mid}});
        if (ar.contains("error")) { rep["error"] = "authenticate: " + ar["error"].value("message", std::string()); return false; }
        rep["authenticated"] = mid;
        return true;
    };
    nlohmann::json mcp = nlohmann::json::array();
    auto open_session = [&]() -> nlohmann::json {
        if (!spec.load_session_id.empty()) {
            if (!can_load) return {{"error", {{"message", "agent does not support session/load"}, {"code", -32601}}}};
            auto lr = call("session/load", {{"sessionId", spec.load_session_id}, {"cwd", root.string()}, {"mcpServers", mcp}});
            if (!lr.contains("error")) lr["result"] = {{"sessionId", spec.load_session_id}};
            return lr;
        }
        return call("session/new", {{"cwd", root.string()}, {"mcpServers", mcp}});
    };
    auto ns = open_session();
    if (ns.contains("error") && ns["error"].value("code", 0) == -32000 && !spec.auth_method.empty()) {
        if (!authenticate()) { rep["auth_required"] = true; return finish(rep); }
        ns = open_session();
    }
    if (ns.contains("error")) {
        rep["error"] = (spec.load_session_id.empty() ? "session/new: " : "session/load: ") + ns["error"].value("message", std::string());
        if (ns["error"].value("code", 0) == -32000) rep["auth_required"] = true;  // ACP auth_required
        return finish(rep);
    }
    const auto sid = ns["result"].value("sessionId", std::string());
    rep["sessionId"] = sid;
    if (!spec.load_session_id.empty()) rep["loaded"] = true;
    std::vector<nlohmann::json> turns{prompt};
    for (const auto& t : spec.more_turns) turns.push_back(t);
    nlohmann::json turn_rep = nlohmann::json::array();
    for (size_t i = 0; i < turns.size(); ++i) {
        const auto before = text.size();
        auto pr = call("session/prompt", {{"sessionId", sid}, {"prompt", turns[i]}});
        if (pr.contains("error")) {
            p.Write({{"jsonrpc", "2.0"}, {"method", "session/cancel"}, {"params", {{"sessionId", sid}}}});
            rep["error"] = "session/prompt: " + pr["error"].value("message", std::string());
            rep["turns"] = turn_rep;
            return finish(rep);
        }
        const auto stop = pr["result"].value("stopReason", std::string());
        turn_rep.push_back({{"stopReason", stop}, {"text", text.substr(before)}});
        rep["stopReason"] = stop;
        if (i + 1 < turns.size()) text += "\n";
    }
    if (turns.size() > 1) rep["turns"] = turn_rep;
    rep["ok"] = true;
    return finish(rep);
}

nlohmann::json RunFromParams(const nlohmann::json& config, const nlohmann::json& p, bool trusted_cli) {
    RunSpec s;
    s.agent = p.value("agent", std::string());
    std::string err;
    s.argv = ResolveCommand(config, s.agent, &err);
    if (s.argv.empty()) return {{"ok", false}, {"error", err.empty() ? "agent required" : err}, {"presets", Presets()}};
    s.cwd = p.value("cwd", fs::current_path().string());
    if (s.cwd.empty() || s.cwd[0] != '/') return {{"ok", false}, {"error", "cwd must be absolute"}};
    s.permission = p.value("permission", std::string("deny"));
    if (s.permission != "deny" && s.permission != "allow_once" && s.permission != "allow_always") return {{"ok", false}, {"error", "permission: deny | allow_once | allow_always"}};
    s.fs_write = p.value("fs_write", false);
    s.terminal = p.value("terminal", false);
    if (s.terminal && !trusted_cli) {
        bool allowed = false;
        try { allowed = config.at("acp").at("agents").at(s.agent).value("allowTerminal", false); } catch (...) {}
        if (!allowed) return {{"ok", false}, {"error", "terminal capability requires acp.agents." + s.agent + ".allowTerminal=true"}};
    }
    s.load_session_id = p.value("session_id", std::string());
    s.auth_method = p.value("auth_method", std::string());
    if (s.auth_method.empty() && config.is_object()) {
        try { s.auth_method = config.at("acp").at("agents").at(s.agent).value("authMethod", std::string()); } catch (...) {}
    }
    if (p.contains("turns") && p["turns"].is_array())
        for (auto& t : p["turns"]) {
            if (t.is_string()) s.more_turns.push_back(nlohmann::json::array({{{"type", "text"}, {"text", t.get<std::string>()}}}));
            else if (t.is_array()) s.more_turns.push_back(t);
        }
    s.timeout_ms = p.value("timeout_ms", 300000);
    if (p.contains("prompt") && p["prompt"].is_array()) s.prompt = p["prompt"];
    else if (p.contains("prompt") && p["prompt"].is_string()) s.prompt.push_back({{"type", "text"}, {"text", p["prompt"].get<std::string>()}});
    if (p.contains("audio_path")) {
        std::ifstream f(p["audio_path"].get<std::string>(), std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(f)), {});
        if (raw.empty()) return {{"ok", false}, {"error", "audio_path unreadable"}};
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64; int val = 0, bits = -6;
        for (unsigned char ch : raw) { val = (val << 8) + ch; bits += 8; while (bits >= 0) { b64.push_back(tbl[(val >> bits) & 0x3f]); bits -= 6; } }
        if (bits > -6) b64.push_back(tbl[((val << 8) >> (bits + 8)) & 0x3f]);
        while (b64.size() % 4) b64.push_back('=');
        auto ext = fs::path(p["audio_path"].get<std::string>()).extension().string();
        const std::string mime = ext == ".wav" ? "audio/wav" : ext == ".mp3" ? "audio/mpeg" : "audio/ogg";
        s.prompt.push_back({{"type", "audio"}, {"data", b64}, {"mimeType", mime}});
    }
    if (s.prompt.empty()) return {{"ok", false}, {"error", "prompt or audio_path required"}};
    auto r = Run(s);
    if (!p.value("include_updates", false)) r.erase("updates");  // keep RPC/tool payloads small; text/tool_calls/permissions/fs carry the outcome
    return r;
}

}  // namespace openclaw::acp::acpx
