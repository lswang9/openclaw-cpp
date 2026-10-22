#include "openclaw/gateway/conversations.hpp"
#include "openclaw/gateway/core_tools.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <random>
#include <thread>

namespace openclaw::gateway {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
std::mutex g_mu;
std::condition_variable g_cv;
std::string g_root;
ConvSendFn g_send;
long long Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string Hex(const unsigned char* d, size_t n) { static const char* h = "0123456789abcdef"; std::string s; for (size_t i = 0; i < n; ++i) { s += h[d[i] >> 4]; s += h[d[i] & 15]; } return s; }
std::string RefFor(const std::string& channel, const std::string& account, const std::string& to, const std::string& thread) {
    const std::string k = channel + "\x1f" + account + "\x1f" + to + "\x1f" + thread;
    unsigned char md[SHA256_DIGEST_LENGTH]; SHA256(reinterpret_cast<const unsigned char*>(k.data()), k.size(), md);
    return "conv_" + Hex(md, 16);
}
fs::path DbPath() { return fs::path(g_root.empty() ? std::string("/tmp/openclaw-conv") : g_root) / "conversations.json"; }
json Load() { std::ifstream in(DbPath()); json j = json::parse(in, nullptr, false); return j.is_object() ? j : json::object(); }
void Save(const json& j) { std::error_code ec; fs::create_directories(DbPath().parent_path(), ec); auto tmp = DbPath().string() + ".tmp"; { std::ofstream o(tmp); o << j.dump(2); } fs::rename(tmp, DbPath(), ec); }
std::string RandHex(int n) { static thread_local std::mt19937_64 rng{std::random_device{}()}; std::string s; static const char* h = "0123456789abcdef"; for (int i = 0; i < n; ++i) s += h[rng() & 15]; return s; }

struct Turn { std::string ref, channel, to; bool done = false, cancelled = false; std::string reply; long long at = 0; };
std::map<std::string, Turn> g_turns;  // turnId → turn
struct Question { json rec; bool done = false; };
std::map<std::string, Question> g_questions;  // id → question
json Ok(json p) { return {{"ok", true}, {"payload", std::move(p)}}; }
json Fail(const std::string& e) { return {{"ok", false}, {"error", e}}; }
}  // namespace

void ConvSetRoot(const std::string& r) { std::lock_guard<std::mutex> l(g_mu); g_root = r; }
void ConvSetSender(ConvSendFn fn) { std::lock_guard<std::mutex> l(g_mu); g_send = std::move(fn); }

std::string ConvRecord(const json& a) {
    const auto channel = a.value("channel", std::string()), to = a.value("to", std::string());
    if (channel.empty() || to.empty()) return {};
    const auto account = a.value("accountId", std::string()), thread = a.value("threadId", std::string());
    const auto ref = RefFor(channel, account, to, thread);
    std::lock_guard<std::mutex> l(g_mu);
    auto db = Load();
    json& e = db[ref];
    if (!e.is_object()) e = {{"conversationRef", ref}, {"createdAt", Now()}};
    e["channel"] = channel; e["to"] = to; e["accountId"] = account; e["threadId"] = thread;
    for (auto k : {"chatType", "sessionKey", "agentId", "title"}) if (a.contains(k) && a[k].is_string() && !a[k].get<std::string>().empty()) e[k] = a[k];
    e["lastInboundAt"] = Now();
    Save(db);
    return ref;
}

bool ConvConsumeInbound(const std::string& channel, const std::string& to, const std::string& session_key, const std::string& text) {
    std::lock_guard<std::mutex> l(g_mu);
    // Oldest waiting conversations_turn on this exact address gets the reply.
    Turn* best = nullptr;
    for (auto& [id, t] : g_turns) if (!t.done && !t.cancelled && t.channel == channel && t.to == to && (!best || t.at < best->at)) best = &t;
    if (best) { best->done = true; best->reply = text; g_cv.notify_all(); return true; }
    // A pending question on this session is answered by the next inbound message.
    for (auto& [id, q] : g_questions) {
        if (q.done || q.rec.value("sessionKey", std::string()) != session_key || session_key.empty()) continue;
        json answers = json::object();
        for (auto& qq : q.rec["questions"]) answers[qq.value("id", std::string("q1"))] = text;
        q.rec["answers"] = answers; q.rec["status"] = "answered"; q.rec["resolvedBy"] = "channel:" + channel; q.rec["resolvedAt"] = Now();
        q.done = true; g_cv.notify_all();
        return true;
    }
    return false;
}

bool IsConvRpc(const std::string& m, const json& p) {
    // conversationRef-addressed calls are external conversations; sessionKey-addressed ones keep the local-session behaviour.
    if (m == "conversations.send" || m == "conversations.turn") return p.is_object() && p.contains("conversationRef");
    if (m == "conversations.turn.cancel") {
        if (p.is_object() && p.contains("conversationRef")) return true;
        std::lock_guard<std::mutex> l(g_mu);
        return p.is_object() && p.contains("turnId") && p["turnId"].is_string() && g_turns.count(p["turnId"].get<std::string>());
    }
    return m == "conversations.list" || m == "question.request" || m == "question.get" || m == "question.list" || m == "question.resolve" || m == "question.waitAnswer";
}

namespace {
json WaitQuestion(const std::string& id, long long timeout_ms) {
    std::unique_lock<std::mutex> l(g_mu);
    auto it = g_questions.find(id);
    if (it == g_questions.end()) return Fail("unknown question " + id);
    g_cv.wait_for(l, std::chrono::milliseconds(std::max(0LL, timeout_ms)), [&] { return g_questions[id].done; });
    auto& q = g_questions[id];
    if (!q.done) return Ok({{"status", "pending"}, {"id", id}});
    return Ok({{"status", q.rec.value("status", std::string("answered"))}, {"id", id}, {"answers", q.rec.value("answers", json::object())}});
}
json Send(const json& e, const std::string& text, const json& extra_in = json::object()) {
    ConvSendFn fn; { fn = g_send; }
    if (!fn) return Fail("gateway outbound is not available");
    json extra = extra_in.is_object() ? extra_in : json::object();
    if (!e.value("threadId", std::string()).empty()) extra["message_thread_id"] = e["threadId"];
    if (!e.value("accountId", std::string()).empty()) extra["accountId"] = e["accountId"];
    return fn(e.value("channel", std::string()), e.value("to", std::string()), text, extra);
}
}  // namespace

json ConvRpc(const std::string& m, const json& p) {
    auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
    if (m == "conversations.list") {
        const auto ch = str("channel"), q = str("query"), agent = str("agentId");
        const int limit = std::clamp(p.value("limit", 50), 1, 100);
        json db; { std::lock_guard<std::mutex> l(g_mu); db = Load(); }
        std::vector<json> rows;
        for (auto& [k, v] : db.items()) {
            if (!ch.empty() && v.value("channel", std::string()) != ch) continue;
            if (!agent.empty() && v.contains("agentId") && v.value("agentId", std::string()) != agent) continue;
            if (!q.empty()) {
                std::string hay = v.dump(), ql = q; std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower); std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);
                if (hay.find(ql) == std::string::npos) continue;
            }
            rows.push_back(v);
        }
        std::sort(rows.begin(), rows.end(), [](const json& a, const json& b) { return a.value("lastInboundAt", 0LL) > b.value("lastInboundAt", 0LL); });
        json out = json::array();
        for (auto& r : rows) { if (static_cast<int>(out.size()) >= limit) break; out.push_back(r); }
        return Ok({{"conversations", out}, {"count", out.size()}});
    }
    if (m == "conversations.send" || m == "conversations.turn") {
        const auto ref = str("conversationRef"), msg = str("message");
        if (ref.empty() || msg.empty()) return Fail("invalid params: conversationRef and message are required");
        json e; { std::lock_guard<std::mutex> l(g_mu); auto db = Load(); if (!db.contains(ref)) return Fail("unknown conversationRef " + ref); e = db[ref]; }
        std::string turn_id;
        if (m == "conversations.turn") {  // register before sending so a fast reply is not lost
            turn_id = "turn_" + RandHex(16);
            std::lock_guard<std::mutex> l(g_mu);
            g_turns[turn_id] = Turn{ref, e.value("channel", std::string()), e.value("to", std::string()), false, false, "", Now()};
        }
        auto sent = Send(e, msg);
        const bool ok = !(sent.is_object() && sent.contains("ok") && sent["ok"] == false);
        if (!ok) { std::lock_guard<std::mutex> l(g_mu); g_turns.erase(turn_id); return Fail("delivery failed: " + sent.dump()); }
        if (m == "conversations.send") return Ok({{"status", "sent"}, {"conversationRef", ref}, {"delivery", sent}});
        const int secs = std::clamp(p.value("timeoutSeconds", 60), 1, 300);
        std::unique_lock<std::mutex> l(g_mu);
        g_cv.wait_for(l, std::chrono::seconds(secs), [&] { auto& t = g_turns[turn_id]; return t.done || t.cancelled; });
        Turn t = g_turns[turn_id]; g_turns.erase(turn_id);
        if (t.done) return Ok({{"status", "replied"}, {"conversationRef", ref}, {"turnId", turn_id}, {"reply", {{"text", t.reply}}}});
        return Ok({{"status", t.cancelled ? "cancelled" : "timeout"}, {"conversationRef", ref}, {"turnId", turn_id}});
    }
    if (m == "conversations.turn.cancel") {
        std::lock_guard<std::mutex> l(g_mu);
        int n = 0;
        for (auto& [id, t] : g_turns) if ((str("turnId") == id || (!str("conversationRef").empty() && t.ref == str("conversationRef"))) && !t.done) { t.cancelled = true; ++n; }
        g_cv.notify_all();
        return Ok({{"cancelled", n}});
    }
    if (m == "question.request") {
        json qs = p.value("questions", json::array());
        if (!qs.is_array() || qs.empty()) {
            const auto one = str("question").empty() ? str("prompt") : str("question");
            if (one.empty()) return Fail("invalid params: questions is required");
            qs = json::array({{{"question", one}, {"options", p.value("options", json::array())}}});
        }
        int i = 0;
        for (auto& q : qs) { if (!q.is_object()) return Fail("invalid params: each question must be an object"); ++i; if (!q.contains("id")) q["id"] = "q" + std::to_string(i); }
        const auto id = str("id").empty() ? "question_" + RandHex(16) : str("id");
        json rec{{"id", id}, {"sessionKey", str("sessionKey")}, {"agentId", str("agentId")}, {"questions", qs}, {"status", "pending"}, {"createdAt", Now()},
                 {"expiresAt", Now() + std::clamp(p.value("timeoutMs", 300000LL), 1000LL, 3600000LL)}};
        std::lock_guard<std::mutex> l(g_mu);
        if (g_questions.count(id)) return Fail("question " + id + " already exists");
        g_questions[id] = Question{rec, false};
        return Ok(rec);
    }
    if (m == "question.get" || m == "question.list") {
        std::lock_guard<std::mutex> l(g_mu);
        for (auto& [id, q] : g_questions) if (!q.done && Now() > q.rec.value("expiresAt", 0LL)) { q.done = true; q.rec["status"] = "expired"; }
        if (m == "question.get") { auto it = g_questions.find(str("id")); if (it == g_questions.end()) return Fail("unknown question " + str("id")); return Ok(it->second.rec); }
        json out = json::array();
        for (auto& [id, q] : g_questions)
            if ((str("sessionKey").empty() || q.rec.value("sessionKey", std::string()) == str("sessionKey")) && (str("status").empty() || q.rec.value("status", std::string()) == str("status")))
                out.push_back(q.rec);
        return Ok({{"questions", out}});
    }
    if (m == "question.resolve") {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_questions.find(str("id"));
        if (it == g_questions.end()) return Fail("unknown question " + str("id"));
        auto& q = it->second;
        if (q.done) return {{"ok", false}, {"error", "question already " + q.rec.value("status", std::string("resolved"))}, {"reason", "already_resolved"}};
        if (p.value("cancel", false)) q.rec["status"] = "cancelled";
        else {
            json ans = p.value("answers", json());
            if (!ans.is_object()) { if (p.contains("answer")) ans = {{q.rec["questions"][0].value("id", std::string("q1")), p["answer"]}}; else return Fail("invalid params: answers or cancel is required"); }
            q.rec["answers"] = ans; q.rec["status"] = "answered";
        }
        q.rec["resolvedBy"] = str("resolvedBy").empty() ? std::string("rpc") : str("resolvedBy"); q.rec["resolvedAt"] = Now();
        q.done = true; g_cv.notify_all();
        return Ok(q.rec);
    }
    if (m == "question.waitAnswer") return WaitQuestion(str("id"), std::clamp(p.value("timeoutMs", 30000LL), 0LL, 3600000LL));
    return Fail("unknown method " + m);
}

void GatewayEmit(const std::string& event, const json& payload);  // subsystems.cpp

namespace {
size_t CurlW(char* p, size_t s, size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }
std::string Esc(CURL* c, const std::string& s) { char* e = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size())); std::string r = e ? e : ""; curl_free(e); return r; }
long Http(const std::string& url, const std::vector<std::string>& hdrs, const std::string* body, std::string* out) {
    CURL* c = curl_easy_init(); if (!c) return 0;
    curl_slist* h = nullptr; for (auto& x : hdrs) h = curl_slist_append(h, x.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str()); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, CurlW); curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L); curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (body) { curl_easy_setopt(c, CURLOPT_POST, 1L); curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->c_str()); }
    long code = 0; if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h); curl_easy_cleanup(c); return code;
}
std::string CfgStr(const json& cfg, std::initializer_list<const char*> path) {
    const json* j = &cfg; for (auto k : path) { if (!j->is_object() || !j->contains(k)) return {}; j = &(*j)[k]; }
    return j->is_string() ? j->get<std::string>() : std::string();
}
std::string Env(const char* k) { const char* v = std::getenv(k); return v ? v : ""; }

std::function<json(const std::string&, const json&)> g_plugin_search;
std::string WebSearch(const json& a, const json& cfg) {
    const auto query = a.value("query", std::string());
    if (query.empty()) return json{{"ok", false}, {"error", "query is required"}}.dump();
    const int count = std::clamp(a.value("count", 5), 1, 20);
    std::string provider = a.value("provider", CfgStr(cfg, {"tools", "web", "search", "provider"}));
    const std::string cfg_key = CfgStr(cfg, {"tools", "web", "search", "apiKey"});
    std::string brave = Env("BRAVE_API_KEY"), tav = Env("TAVILY_API_KEY");
    if (provider.empty()) provider = !brave.empty() ? "brave" : !tav.empty() ? "tavily" : !cfg_key.empty() ? "brave" : "";
    const bool builtin = provider == "brave" || provider == "tavily";
    if (g_plugin_search && (!builtin || (provider == "brave" && brave.empty() && cfg_key.empty()) || (provider == "tavily" && tav.empty() && cfg_key.empty()))) {
        json args = a; args.erase("provider");
        auto r = g_plugin_search(builtin ? std::string() : provider, args);
        if (r.value("ok", false)) return json{{"provider", r.value("provider", provider)}, {"plugin", r.value("plugin", "")}, {"query", query}, {"result", r["result"]}, {"externalContent", true}}.dump();
        if (!provider.empty() && !builtin) return json{{"ok", false}, {"error", r.value("error", std::string("plugin web search failed"))}}.dump();
    }
    if (provider == "brave" && brave.empty()) brave = cfg_key;
    if (provider == "tavily" && tav.empty()) tav = cfg_key;
    if (provider.empty() || (provider == "brave" && brave.empty()) || (provider == "tavily" && tav.empty()))
        return json{{"ok", false}, {"error", "no web search provider configured (set BRAVE_API_KEY or TAVILY_API_KEY, or tools.web.search.apiKey)"}}.dump();
    std::string base = CfgStr(cfg, {"tools", "web", "search", "baseUrl"});
    json results = json::array(); std::string body; long code = 0;
    if (provider == "brave") {
        if (base.empty()) base = Env("OPENCLAW_BRAVE_BASE_URL").empty() ? "https://api.search.brave.com" : Env("OPENCLAW_BRAVE_BASE_URL");
        CURL* c = curl_easy_init(); const auto q = Esc(c, query); curl_easy_cleanup(c);
        code = Http(base + "/res/v1/web/search?q=" + q + "&count=" + std::to_string(count), {"Accept: application/json", "X-Subscription-Token: " + brave}, nullptr, &body);
        auto j = json::parse(body, nullptr, false);
        if (code == 200 && j.is_object() && j.contains("web"))
            for (auto& r : j["web"].value("results", json::array())) results.push_back({{"title", r.value("title", "")}, {"url", r.value("url", "")}, {"snippet", r.value("description", "")}});
    } else if (provider == "tavily") {
        if (base.empty()) base = Env("OPENCLAW_TAVILY_BASE_URL").empty() ? "https://api.tavily.com" : Env("OPENCLAW_TAVILY_BASE_URL");
        const std::string req = json{{"api_key", tav}, {"query", query}, {"max_results", count}}.dump();
        code = Http(base + "/search", {"Content-Type: application/json"}, &req, &body);
        auto j = json::parse(body, nullptr, false);
        if (code == 200 && j.is_object())
            for (auto& r : j.value("results", json::array())) results.push_back({{"title", r.value("title", "")}, {"url", r.value("url", "")}, {"snippet", r.value("content", "")}});
    } else return json{{"ok", false}, {"error", "unsupported web search provider " + provider + " (brave, tavily)"}}.dump();
    if (code != 200) return json{{"ok", false}, {"provider", provider}, {"error", "search provider returned HTTP " + std::to_string(code)}, {"body", body.substr(0, 500)}}.dump();
    while (static_cast<int>(results.size()) > count) results.erase(results.end() - 1);
    // Results are external, untrusted content.
    return json{{"provider", provider}, {"query", query}, {"count", results.size()}, {"results", results}, {"externalContent", true}}.dump();
}

std::string ViewImage(const json& a) {
    std::string src = a.value("image", a.value("path", a.value("url", std::string())));
    if (src.empty()) return json{{"ok", false}, {"error", "image (local path or data: URL) is required"}}.dump();
    std::string bytes;
    if (src.rfind("data:", 0) == 0) {
        auto comma = src.find(','); if (comma == std::string::npos || src.find(";base64") > comma) return json{{"ok", false}, {"error", "only base64 data: URLs are supported"}}.dump();
        std::string b64 = src.substr(comma + 1); std::string out((b64.size() / 4 + 1) * 3, '\0');
        int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()), reinterpret_cast<const unsigned char*>(b64.data()), static_cast<int>(b64.size()));
        if (n < 0) return json{{"ok", false}, {"error", "invalid base64 image"}}.dump();
        while (!b64.empty() && b64.back() == '=') { b64.pop_back(); --n; }
        out.resize(std::max(0, n)); bytes = out;
    } else if (src.rfind("http://", 0) == 0 || src.rfind("https://", 0) == 0) {
        return json{{"ok", false}, {"error", "remote images are not fetched by view_image; download with web_fetch/http_get to a file first"}}.dump();
    } else {
        if (src.rfind("file://", 0) == 0) src = src.substr(7);
        std::error_code ec;
        if (!fs::is_regular_file(src, ec)) return json{{"ok", false}, {"error", "image not found: " + src}}.dump();
        if (fs::file_size(src, ec) > 20u * 1024 * 1024) return json{{"ok", false}, {"error", "image larger than 20MB"}}.dump();
        std::ifstream in(src, std::ios::binary); std::ostringstream ss; ss << in.rdbuf(); bytes = ss.str();
    }
    auto u = [&](size_t i) { return i < bytes.size() ? static_cast<unsigned char>(bytes[i]) : 0u; };
    json r{{"source", src.rfind("data:", 0) == 0 ? std::string("data-url") : src}, {"bytes", bytes.size()}};
    std::string mime;
    if (u(0) == 0x89 && u(1) == 'P') { mime = "image/png"; r["width"] = (u(16) << 24) | (u(17) << 16) | (u(18) << 8) | u(19); r["height"] = (u(20) << 24) | (u(21) << 16) | (u(22) << 8) | u(23); }
    else if (u(0) == 0xFF && u(1) == 0xD8) {
        mime = "image/jpeg";
        for (size_t i = 2; i + 9 < bytes.size();) {  // SOFn marker carries the dimensions
            if (u(i) != 0xFF) { ++i; continue; }
            const unsigned mk = u(i + 1); const size_t len = (u(i + 2) << 8) | u(i + 3);
            if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) { r["height"] = (u(i + 5) << 8) | u(i + 6); r["width"] = (u(i + 7) << 8) | u(i + 8); break; }
            i += 2 + len;
        }
    } else if (u(0) == 'G' && u(1) == 'I' && u(2) == 'F') { mime = "image/gif"; r["width"] = u(6) | (u(7) << 8); r["height"] = u(8) | (u(9) << 8); }
    else if (u(8) == 'W' && u(9) == 'E' && u(10) == 'B') mime = "image/webp";
    else return json{{"ok", false}, {"error", "unsupported or unrecognised image format (png, jpeg, gif, webp)"}}.dump();
    r["mimeType"] = mime;
    const size_t max_inline = static_cast<size_t>(std::clamp(a.value("maxInlineBytes", 1 << 20), 0, 8 << 20));
    if (bytes.size() <= max_inline) {
        std::string b64(4 * ((bytes.size() + 2) / 3) + 1, '\0');
        int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(b64.data()), reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
        b64.resize(n); r["dataUrl"] = "data:" + mime + ";base64," + b64;
    } else r["note"] = "image larger than maxInlineBytes; dataUrl omitted";
    if (a.contains("prompt")) r["prompt"] = a["prompt"];
    return r.dump();
}

std::string AgentsWait(const json& a, const std::string& session_key, const std::string& root) {
    json ids = a.value("ids", json::array());
    if (!ids.is_array() || ids.empty() || ids.size() > 32) return json{{"success", false}, {"error", "ids must list 1-32 run ids"}}.dump();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(std::max(0.0, a.value("timeoutSeconds", 30.0)) * 1000));
    const bool await_results = a.value("awaitResults", true);
    for (;;) {
        std::ifstream in(fs::path(root) / "subagents.json"); json reg = json::parse(in, nullptr, false); if (!reg.is_object()) reg = json::object();
        json completed = json::array(), pending = json::array(), errors = json::array();
        for (auto& idj : ids) {
            const auto id = idj.is_string() ? idj.get<std::string>() : std::string();
            if (!reg.contains(id)) { errors.push_back({{"runId", id}, {"error", "not_found"}}); continue; }
            const auto& v = reg[id];
            if (!session_key.empty() && v.value("parent", session_key) != session_key && !a.value("all", false)) { errors.push_back({{"runId", id}, {"error", "not_owner"}}); continue; }
            const auto st = v.value("status", std::string("running"));
            if (st == "running") { pending.push_back(id); continue; }
            json c{{"runId", id}, {"status", st == "done" ? "done" : st == "killed" ? "killed" : "failed"}, {"sessionKey", id}, {"label", v.value("label", "")}};
            c["result"] = await_results ? (v.contains("result") && v["result"].is_string() ? v["result"].get<std::string>() : v.value("result", json()).dump()) : std::string();
            completed.push_back(c);
        }
        if (pending.empty() || std::chrono::steady_clock::now() >= deadline) {
            json out{{"completed", completed}, {"pending", pending}};
            if (!errors.empty()) out["errors"] = errors;
            return out.dump();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

std::string AskUser(const json& a, const std::string& agent, const std::string& session_key) {
    json qs = a.value("questions", json::array());
    if (!qs.is_array() || qs.empty()) {
        const auto one = a.value("question", a.value("prompt", std::string()));
        if (one.empty()) return json{{"ok", false}, {"error", "question (or questions[]) is required"}}.dump();
        qs = json::array({{{"question", one}, {"options", a.value("options", json::array())}}});
    }
    const long long timeout_ms = static_cast<long long>(std::clamp(a.value("timeoutSeconds", 300.0), 1.0, 3600.0) * 1000);
    auto req = ConvRpc("question.request", {{"sessionKey", session_key}, {"agentId", agent}, {"questions", qs}, {"timeoutMs", timeout_ms}});
    if (!req.value("ok", false)) return req.dump();
    const json rec = req["payload"];
    const auto id = rec.value("id", std::string());
    GatewayEmit("question.requested", rec);
    // Mirror the prompt to the external conversation behind this session, when there is one.
    std::string text;
    for (auto& q : rec["questions"]) {
        if (!text.empty()) text += "\n\n";
        text += q.value("question", q.value("prompt", std::string()));
        int i = 0;
        for (auto& o : q.value("options", json::array())) text += "\n" + std::to_string(++i) + ". " + (o.is_string() ? o.get<std::string>() : o.value("label", o.dump()));
    }
    json conv;
    { std::lock_guard<std::mutex> l(g_mu); json db = Load(); for (auto& [k, v] : db.items()) if (!session_key.empty() && v.value("sessionKey", std::string()) == session_key) conv = v; }
    bool mirrored = false;
    // 1.135: single-question prompts with options render as inline buttons on channels that support them (Telegram);
    // a press arrives as the option text, which resolves the question like a typed answer.
    json buttons = json::array();
    if (rec["questions"].size() == 1)
        for (auto& o : rec["questions"][0].value("options", json::array())) buttons.push_back(json::array({o.is_string() ? o.get<std::string>() : o.value("label", o.dump())}));
    if (conv.is_object()) { auto s = Send(conv, text, buttons.empty() ? json::object() : json{{"buttons", buttons}}); mirrored = !(s.is_object() && s.contains("ok") && s["ok"] == false); }
    auto w = WaitQuestion(id, timeout_ms);
    json res = w.value("payload", json::object());
    if (res.value("status", std::string()) == "pending") {
        ConvRpc("question.resolve", {{"id", id}, {"cancel", true}, {"resolvedBy", "timeout"}});
        { std::lock_guard<std::mutex> l(g_mu); g_questions[id].rec["status"] = "expired"; }
        GatewayEmit("question.resolved", {{"id", id}, {"status", "expired"}});
        return json{{"status", "timeout"}, {"questionId", id}, {"note", "the user did not answer in time; continue without the answer or ask again"}}.dump();
    }
    GatewayEmit("question.resolved", {{"id", id}, {"status", res.value("status", std::string())}});
    if (res.value("status", std::string()) != "answered") return json{{"status", res.value("status", std::string("cancelled")), }, {"questionId", id}}.dump();
    json answers = res.value("answers", json::object()), lines = json::array();
    for (auto& q : rec["questions"]) {
        const auto qid = q.value("id", std::string());
        json ans = answers.contains(qid) ? answers[qid] : json();
        auto opts = q.value("options", json::array());
        if (ans.is_string() && !opts.empty()) {  // "2" picks the second option
            try { size_t pos = 0; int k = std::stoi(ans.get<std::string>(), &pos); if (pos == ans.get<std::string>().size() && k >= 1 && k <= static_cast<int>(opts.size())) ans = opts[k - 1].is_string() ? opts[k - 1] : opts[k - 1].value("label", json()); } catch (...) {}
        }
        lines.push_back({{"id", qid}, {"question", q.value("question", std::string())}, {"answer", ans}});
    }
    return json{{"status", "answered"}, {"questionId", id}, {"answers", lines}, {"mirroredToChannel", mirrored}}.dump();
}
}  // namespace

void SetPluginWebSearch(std::function<json(const std::string&, const json&)> fn) { g_plugin_search = std::move(fn); }

std::vector<std::string> ExtraGatewayToolNames() {
    std::vector<std::string> v{"web_search", "ask_user", "view_image", "agents_wait", "conversations_list", "conversations_send", "conversations_turn"};
    for (auto& n : CoreGatewayToolNames()) v.push_back(n);
    return v;
}

bool ExtraGatewayTool(const std::string& name, const json& a, const std::string& agent, const std::string& session_key, const json& cfg,
                      const std::string& root, std::string* out) {
    if (name == "web_search") { *out = WebSearch(a, cfg); return true; }
    if (name == "view_image") { *out = ViewImage(a); return true; }
    if (name == "agents_wait") { *out = AgentsWait(a, session_key, root); return true; }
    if (name == "ask_user") { *out = AskUser(a, agent, session_key); return true; }
    auto rpc = [&](const std::string& m, json p) { auto r = ConvRpc(m, p); *out = (r.value("ok", false) ? r["payload"] : r).dump(); return true; };
    if (name == "conversations_list") { json p = a; p["agentId"] = agent; return rpc("conversations.list", p); }
    if (name == "conversations_send") return rpc("conversations.send", {{"conversationRef", a.value("conversationRef", std::string())}, {"message", a.value("message", std::string())}});
    if (name == "conversations_turn")
        return rpc("conversations.turn", {{"conversationRef", a.value("conversationRef", std::string())}, {"message", a.value("message", std::string())}, {"timeoutSeconds", a.value("timeoutSeconds", 60)}});
    return CoreGatewayTool(name, a, agent, session_key, cfg, out);
}
}  // namespace openclaw::gateway
