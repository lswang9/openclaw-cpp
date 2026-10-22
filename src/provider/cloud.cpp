#include "openclaw/provider/cloud.hpp"
#include <cctype>
#include <cstdio>
#include <optional>
#include "openclaw/provider/cloud_auth.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <functional>
#include <map>
#include <sstream>

namespace openclaw::provider {
namespace {

using json = nlohmann::json;
using cloud_auth::AwsCreds;
using cloud_auth::ResolveAwsFromEnv;
using cloud_auth::ResolveGoogleAdcAccessToken;
using cloud_auth::SignAwsV4;

size_t Sink(char* p, size_t s, size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }
struct HttpRes { long status = 0; std::string body, error; };
HttpRes Post(const std::string& url, const std::vector<std::string>& headers, const std::string& body) {
    HttpRes r; CURL* c = curl_easy_init(); if (!c) { r.error = "curl init failed"; return r; }
    curl_slist* h = nullptr; for (auto& x : headers) h = curl_slist_append(h, x.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str()); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str()); curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Sink); curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L); curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    auto rc = curl_easy_perform(c); if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status); curl_slist_free_all(h); curl_easy_cleanup(c);
    return r;
}
struct SseState { std::string buf, ev, data, raw; long status = 0; const std::function<void(const std::string&, const std::string&)>* cb; CURL* c; };
size_t SseSink(char* p, size_t s, size_t n, void* u) {
    auto* st = static_cast<SseState*>(u); const size_t len = s * n;
    if (!st->status) curl_easy_getinfo(st->c, CURLINFO_RESPONSE_CODE, &st->status);
    if (st->status && (st->status < 200 || st->status >= 300)) { st->raw.append(p, len); return len; }
    st->buf.append(p, len); std::size_t nl;
    while ((nl = st->buf.find('\n')) != std::string::npos) {
        std::string line = st->buf.substr(0, nl); st->buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) { if (!st->data.empty()) (*st->cb)(st->ev, st->data); st->ev.clear(); st->data.clear(); continue; }
        if (line.rfind("event:", 0) == 0) { st->ev = line.substr(6); while (!st->ev.empty() && st->ev[0] == ' ') st->ev.erase(0, 1); }
        else if (line.rfind("data:", 0) == 0) { std::string d = line.substr(5); if (!d.empty() && d[0] == ' ') d.erase(0, 1); if (!st->data.empty()) st->data += "\n"; st->data += d; }
    }
    return len;
}
HttpRes PostSse(const std::string& url, std::vector<std::string> headers, const std::string& body, const std::function<void(const std::string&, const std::string&)>& cb) {
    HttpRes r; CURL* c = curl_easy_init(); if (!c) { r.error = "curl init failed"; return r; }
    headers.push_back("Accept: text/event-stream");
    curl_slist* h = nullptr; for (auto& x : headers) h = curl_slist_append(h, x.c_str());
    SseState st; st.cb = &cb; st.c = c;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str()); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str()); curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, SseSink); curl_easy_setopt(c, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L); curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    auto rc = curl_easy_perform(c); if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    if (!st.data.empty()) cb(st.ev, st.data);
    r.body = st.raw; curl_slist_free_all(h); curl_easy_cleanup(c); return r;
}
agent::CompletionResult Fail(const std::string& label, const HttpRes& h) {
    agent::CompletionResult r; r.ok = false; r.finish_reason = "error"; r.http_status = static_cast<int>(h.status);
    if (!h.error.empty()) { r.error = label + " request failed: " + h.error; return r; }
    r.error = label + " HTTP " + std::to_string(h.status);
    try { auto j = json::parse(h.body); if (j.contains("error") || j.contains("message")) r.error += ": " + (j.contains("message") ? j.value("message", "") : j["error"].is_object() ? j["error"].value("message", j["error"].dump()) : j["error"].dump()); }
    catch (...) { if (!h.body.empty()) r.error += " | " + h.body.substr(0, 300); }
    return r;
}
std::string NoSlash(std::string s) { while (!s.empty() && s.back() == '/') s.pop_back(); return s; }
std::string UrlEncodePath(const std::string& s) {
    std::string o; char buf[8];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { std::snprintf(buf, sizeof(buf), "%%%02X", c); o += buf; }
    }
    return o;
}
std::string HostOf(const std::string& base) {
    auto u = base;
    if (u.rfind("https://", 0) == 0) u = u.substr(8); else if (u.rfind("http://", 0) == 0) u = u.substr(7);
    auto slash = u.find('/'); return u.substr(0, slash);
}
std::vector<std::string> BaseHeaders(const ProviderSpec& s) {
    std::vector<std::string> h{"Content-Type: application/json"};
    for (auto& [k, v] : s.headers.items()) if (v.is_string()) h.push_back(k + ": " + v.get<std::string>());
    return h;
}
json ParseArgs(const std::string& s) { try { auto j = json::parse(s.empty() ? "{}" : s); return j.is_object() ? j : json{{"value", j}}; } catch (...) { return json{{"raw", s}}; } }
struct ToolDef { std::string name, description; json params; };
std::vector<ToolDef> Tools(const json& tools) {
    std::vector<ToolDef> v;
    if (!tools.is_array()) return v;
    for (auto& t : tools) {
        const json& f = t.contains("function") ? t["function"] : t;
        ToolDef d{f.value("name", ""), f.value("description", ""), f.value("parameters", json{{"type", "object"}, {"properties", json::object()}})};
        if (!d.name.empty()) v.push_back(d);
    }
    return v;
}

std::vector<std::string> AwsAuthHeaders(const ProviderSpec& s, const std::string& path, const std::string& payload) {
    auto creds = ResolveAwsFromEnv();
    if (!s.region.empty()) creds.region = s.region;
    if (!creds.bearer_token.empty()) return {"Authorization: Bearer " + creds.bearer_token};
    if (creds.access_key_id.empty() || creds.secret_access_key.empty()) return {};  // caller fails
    return SignAwsV4(creds, "POST", HostOf(s.base_url), path, "", payload, "bedrock");
}

std::string GoogleBearer(std::string* why) {
    auto t = ResolveGoogleAdcAccessToken(why);
    return t ? t->access_token : "";
}

std::string VertexProject(const ProviderSpec& s) {
    if (!s.project.empty()) return s.project;
    if (const char* e = std::getenv("GOOGLE_CLOUD_PROJECT"); e && *e) return e;
    if (const char* e = std::getenv("GCLOUD_PROJECT"); e && *e) return e;
    return {};
}
std::string VertexLocation(const ProviderSpec& s) {
    if (!s.location.empty()) return s.location;
    if (const char* e = std::getenv("GOOGLE_CLOUD_LOCATION"); e && *e) return e;
    if (!s.region.empty()) return s.region;
    return "us-east1";
}

// ---------------------------------------------------------------- Bedrock Converse
class BedrockConverse : public Provider {
public:
    explicit BedrockConverse(ProviderSpec s) : s_(std::move(s)) {}
    agent::CompletionResult Complete(const std::vector<agent::Message>& messages, const json& tools) override { return Run(messages, tools, nullptr); }
    agent::CompletionResult CompleteStreaming(const std::vector<agent::Message>& messages, const json& tools, DeltaFn on_delta) override {
        return on_delta ? Run(messages, tools, &on_delta) : Run(messages, tools, nullptr);
    }
    json BuildBody(const std::vector<agent::Message>& messages, const json& tools) const {
        json system = json::array(); json msgs = json::array();
        auto push = [&](const std::string& role, const json& block) {
            if (!msgs.empty() && msgs.back()["role"] == role) msgs.back()["content"].push_back(block);
            else msgs.push_back({{"role", role}, {"content", json::array({block})}});
        };
        for (auto& m : messages) {
            if (m.role == "system") { system.push_back({{"text", m.content}}); continue; }
            if (m.role == "tool") {
                push("user", {{"toolResult", {{"toolUseId", m.tool_call_id.value_or("")}, {"content", json::array({{{"text", m.content}}})}}}});
                continue;
            }
            if (m.role == "assistant") {
                if (!m.content.empty()) push("assistant", {{"text", m.content}});
                for (auto& tc : m.tool_calls) {
                    const auto& f = tc.value("function", json::object());
                    push("assistant", {{"toolUse", {{"toolUseId", tc.value("id", "")}, {"name", f.value("name", "")},
                                                    {"input", ParseArgs(f.contains("arguments") && f["arguments"].is_string() ? f["arguments"].get<std::string>() : f.value("arguments", json::object()).dump())}}}});
                }
                continue;
            }
            push("user", {{"text", m.content.empty() ? std::string(" ") : m.content}});
        }
        json b = {{"messages", msgs}, {"inferenceConfig", {{"maxTokens", s_.max_tokens}}}};
        if (!system.empty()) b["system"] = system;
        auto td = Tools(tools);
        if (!td.empty()) {
            json tools_arr = json::array();
            for (auto& t : td) tools_arr.push_back({{"toolSpec", {{"name", t.name}, {"description", t.description}, {"inputSchema", {{"json", t.params}}}}}});
            b["toolConfig"] = {{"tools", tools_arr}};
        }
        return b;
    }
    agent::CompletionResult ParseConverse(const json& j) const {
        agent::CompletionResult r; r.message.role = "assistant"; r.finish_reason = "stop";
        const auto& msg = j.value("output", json::object()).value("message", json::object());
        for (auto& c : msg.value("content", json::array())) {
            if (c.contains("text")) r.message.content += c.value("text", "");
            else if (c.contains("toolUse")) {
                const auto& tu = c["toolUse"];
                agent::ToolCall tc; tc.id = tu.value("toolUseId", ""); tc.name = tu.value("name", ""); tc.arguments_json = tu.value("input", json::object()).dump();
                r.tool_calls.push_back(tc); r.message.tool_calls.push_back({{"id", tc.id}, {"type", "function"}, {"function", {{"name", tc.name}, {"arguments", tc.arguments_json}}}});
            }
        }
        const auto stop = j.value("stopReason", "end_turn");
        r.finish_reason = stop == "tool_use" ? "tool_calls" : (stop == "max_tokens" ? "length" : "stop");
        return r;
    }
    agent::CompletionResult Run(const std::vector<agent::Message>& messages, const json& tools, const DeltaFn* on_delta) {
        const std::string path = "/model/" + UrlEncodePath(s_.model) + (on_delta ? "/converse-stream" : "/converse");
        const std::string url = NoSlash(s_.base_url) + path;
        const auto body = BuildBody(messages, tools).dump();
        auto h = BaseHeaders(s_);
        auto auth = AwsAuthHeaders(s_, path, body);
        if (auth.empty()) { agent::CompletionResult r; r.ok = false; r.finish_reason = "error"; r.error = s_.id + ": no AWS credentials (AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY or AWS_BEARER_TOKEN_BEDROCK)"; return r; }
        for (auto& x : auth) h.push_back(x);
        if (on_delta) {
            // Bedrock's native event stream is binary; for offline/custom endpoints we accept SSE with the same JSON event shapes the fake serves.
            agent::CompletionResult r; r.message.role = "assistant"; r.finish_reason = "stop";
            auto res = PostSse(url, h, body, [&](const std::string&, const std::string& data) {
                json e; try { e = json::parse(data); } catch (...) { return; }
                if (e.contains("contentBlockDelta") && e["contentBlockDelta"].contains("delta")) {
                    const auto& d = e["contentBlockDelta"]["delta"];
                    if (d.contains("text")) { const auto t = d.value("text", std::string()); r.message.content += t; if (!t.empty()) (*on_delta)(t); }
                } else if (e.contains("contentBlockStart") && e["contentBlockStart"].contains("start") && e["contentBlockStart"]["start"].contains("toolUse")) {
                    const auto& tu = e["contentBlockStart"]["start"]["toolUse"];
                    agent::ToolCall tc; tc.id = tu.value("toolUseId", ""); tc.name = tu.value("name", ""); tc.arguments_json = "{}";
                    r.tool_calls.push_back(tc);
                } else if (e.contains("contentBlockDelta") && e["contentBlockDelta"].contains("delta") && e["contentBlockDelta"]["delta"].contains("toolUse")) {
                    if (!r.tool_calls.empty()) r.tool_calls.back().arguments_json += e["contentBlockDelta"]["delta"]["toolUse"].value("input", "");
                } else if (e.contains("messageStop")) {
                    const auto stop = e["messageStop"].value("stopReason", "end_turn");
                    r.finish_reason = stop == "tool_use" ? "tool_calls" : (stop == "max_tokens" ? "length" : "stop");
                }
            });
            if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
            for (auto& tc : r.tool_calls) {
                if (tc.arguments_json.empty()) tc.arguments_json = "{}";
                r.message.tool_calls.push_back({{"id", tc.id}, {"type", "function"}, {"function", {{"name", tc.name}, {"arguments", tc.arguments_json}}}});
            }
            if (!r.tool_calls.empty()) r.finish_reason = "tool_calls";
            r.http_status = static_cast<int>(res.status); return r;
        }
        auto res = Post(url, h, body);
        if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
        try { auto r = ParseConverse(json::parse(res.body)); r.http_status = static_cast<int>(res.status); return r; }
        catch (const std::exception& e) { agent::CompletionResult r; r.ok = false; r.finish_reason = "error"; r.error = s_.id + std::string(" parse: ") + e.what(); return r; }
    }
private:
    ProviderSpec s_;
};

// ---------------------------------------------------------------- Anthropic on Vertex (rawPredict)
class AnthropicVertex : public Provider {
public:
    explicit AnthropicVertex(ProviderSpec s) : s_(std::move(s)) {}
    agent::CompletionResult Complete(const std::vector<agent::Message>& messages, const json& tools) override { return Run(messages, tools, nullptr); }
    agent::CompletionResult CompleteStreaming(const std::vector<agent::Message>& messages, const json& tools, DeltaFn on_delta) override {
        return on_delta ? Run(messages, tools, &on_delta) : Run(messages, tools, nullptr);
    }
    agent::CompletionResult Run(const std::vector<agent::Message>& messages, const json& tools, const DeltaFn* on_delta) {
        std::string why, token = GoogleBearer(&why);
        if (token.empty() && !s_.api_key.empty()) token = s_.api_key;
        if (token.empty()) { agent::CompletionResult r; r.ok = false; r.finish_reason = "error"; r.error = s_.id + ": " + (why.empty() ? "no Vertex ADC / apiKey" : why); return r; }
        const auto project = VertexProject(s_); const auto loc = VertexLocation(s_);
        if (project.empty()) { agent::CompletionResult r; r.ok = false; r.finish_reason = "error"; r.error = s_.id + ": set GOOGLE_CLOUD_PROJECT or models.providers." + s_.id + ".project"; return r; }
        std::string system; json msgs = json::array();
        auto push = [&](const std::string& role, const json& block) {
            if (!msgs.empty() && msgs.back()["role"] == role) msgs.back()["content"].push_back(block);
            else msgs.push_back({{"role", role}, {"content", json::array({block})}});
        };
        for (auto& m : messages) {
            if (m.role == "system") { system += (system.empty() ? "" : "\n\n") + m.content; continue; }
            if (m.role == "tool") { push("user", {{"type", "tool_result"}, {"tool_use_id", m.tool_call_id.value_or("")}, {"content", m.content}}); continue; }
            if (m.role == "assistant") {
                if (!m.content.empty()) push("assistant", {{"type", "text"}, {"text", m.content}});
                for (auto& tc : m.tool_calls) {
                    const auto& f = tc.value("function", json::object());
                    push("assistant", {{"type", "tool_use"}, {"id", tc.value("id", "")}, {"name", f.value("name", "")},
                                       {"input", ParseArgs(f.contains("arguments") && f["arguments"].is_string() ? f["arguments"].get<std::string>() : f.value("arguments", json::object()).dump())}});
                }
                continue;
            }
            push("user", {{"type", "text"}, {"text", m.content.empty() ? std::string(" ") : m.content}});
        }
        json b = {{"anthropic_version", "vertex-2023-10-16"}, {"max_tokens", s_.max_tokens}, {"messages", msgs}};
        if (!system.empty()) b["system"] = system;
        auto td = Tools(tools);
        if (!td.empty()) { b["tools"] = json::array(); for (auto& t : td) b["tools"].push_back({{"name", t.name}, {"description", t.description}, {"input_schema", t.params}}); }
        const std::string verb = on_delta ? ":streamRawPredict" : ":rawPredict";
        const std::string url = NoSlash(s_.base_url) + "/v1/projects/" + project + "/locations/" + loc + "/publishers/anthropic/models/" + s_.model + verb;
        auto h = BaseHeaders(s_); h.push_back("Authorization: Bearer " + token);
        if (on_delta) {
            b["stream"] = true;
            agent::CompletionResult r; r.message.role = "assistant"; r.finish_reason = "stop";
            std::map<int, agent::ToolCall> calls; std::map<int, std::string> partial;
            auto res = PostSse(url, h, b.dump(), [&](const std::string& ev, const std::string& data) {
                json e; try { e = json::parse(data); } catch (...) { return; }
                const std::string type = e.value("type", ev); const int idx = e.value("index", 0);
                if (type == "content_block_start" && e.contains("content_block") && e["content_block"].value("type", "") == "tool_use") { calls[idx].id = e["content_block"].value("id", ""); calls[idx].name = e["content_block"].value("name", ""); }
                else if (type == "content_block_delta" && e.contains("delta") && e["delta"].value("type", "") == "text_delta") { const auto t = e["delta"].value("text", std::string()); r.message.content += t; if (!t.empty()) (*on_delta)(t); }
                else if (type == "content_block_delta" && e.contains("delta") && e["delta"].value("type", "") == "input_json_delta") partial[idx] += e["delta"].value("partial_json", "");
            });
            if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
            for (auto& [i, c] : calls) { c.arguments_json = partial[i].empty() ? "{}" : partial[i]; r.tool_calls.push_back(c); r.message.tool_calls.push_back({{"id", c.id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", c.arguments_json}}}}); }
            if (!r.tool_calls.empty()) r.finish_reason = "tool_calls";
            r.http_status = static_cast<int>(res.status); return r;
        }
        auto res = Post(url, h, b.dump());
        if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
        agent::CompletionResult r; r.http_status = static_cast<int>(res.status); r.message.role = "assistant";
        try {
            auto j = json::parse(res.body);
            for (auto& c : j.value("content", json::array())) {
                if (c.value("type", "") == "text") r.message.content += c.value("text", "");
                else if (c.value("type", "") == "tool_use") {
                    agent::ToolCall tc; tc.id = c.value("id", ""); tc.name = c.value("name", ""); tc.arguments_json = c.value("input", json::object()).dump();
                    r.tool_calls.push_back(tc); r.message.tool_calls.push_back({{"id", tc.id}, {"type", "function"}, {"function", {{"name", tc.name}, {"arguments", tc.arguments_json}}}});
                }
            }
            const auto stop = j.value("stop_reason", "end_turn");
            r.finish_reason = stop == "tool_use" ? "tool_calls" : stop == "max_tokens" ? "length" : "stop";
        } catch (const std::exception& e) { r.ok = false; r.finish_reason = "error"; r.error = s_.id + std::string(" parse: ") + e.what(); }
        return r;
    }
private:
    ProviderSpec s_;
};

// ---------------------------------------------------------------- Gemini on Vertex
class GoogleVertex : public Provider {
public:
    explicit GoogleVertex(ProviderSpec s) : s_(std::move(s)) {}
    agent::CompletionResult Complete(const std::vector<agent::Message>& messages, const json& tools) override { return Run(messages, tools, nullptr); }
    agent::CompletionResult CompleteStreaming(const std::vector<agent::Message>& messages, const json& tools, DeltaFn on_delta) override {
        return on_delta ? Run(messages, tools, &on_delta) : Run(messages, tools, nullptr);
    }
    agent::CompletionResult Run(const std::vector<agent::Message>& messages, const json& tools, const DeltaFn* on_delta) {
        std::string why, token = GoogleBearer(&why);
        if (token.empty() && !s_.api_key.empty()) token = s_.api_key;
        if (token.empty()) { agent::CompletionResult r; r.ok = false; r.finish_reason = "error"; r.error = s_.id + ": " + (why.empty() ? "no Vertex ADC / apiKey" : why); return r; }
        const auto project = VertexProject(s_); const auto loc = VertexLocation(s_);
        if (project.empty()) { agent::CompletionResult r; r.ok = false; r.finish_reason = "error"; r.error = s_.id + ": set GOOGLE_CLOUD_PROJECT or models.providers." + s_.id + ".project"; return r; }
        std::string system; json contents = json::array();
        auto push = [&](const std::string& role, const json& part) {
            if (!contents.empty() && contents.back()["role"] == role) contents.back()["parts"].push_back(part);
            else contents.push_back({{"role", role}, {"parts", json::array({part})}});
        };
        for (auto& m : messages) {
            if (m.role == "system") { system += (system.empty() ? "" : "\n\n") + m.content; continue; }
            if (m.role == "tool") { push("user", {{"functionResponse", {{"name", m.name.value_or("tool")}, {"response", {{"content", m.content}}}}}}); continue; }
            if (m.role == "assistant") {
                if (!m.content.empty()) push("model", {{"text", m.content}});
                for (auto& tc : m.tool_calls) {
                    const auto& f = tc.value("function", json::object());
                    push("model", {{"functionCall", {{"name", f.value("name", "")}, {"args", ParseArgs(f.contains("arguments") && f["arguments"].is_string() ? f["arguments"].get<std::string>() : f.value("arguments", json::object()).dump())}}}});
                }
                continue;
            }
            push("user", {{"text", m.content.empty() ? std::string(" ") : m.content}});
        }
        json b = {{"contents", contents}};
        if (!system.empty()) b["systemInstruction"] = {{"parts", json::array({{{"text", system}}})}};
        auto td = Tools(tools);
        if (!td.empty()) {
            json decl = json::array(); for (auto& t : td) decl.push_back({{"name", t.name}, {"description", t.description}, {"parameters", t.params}});
            b["tools"] = json::array({{{"functionDeclarations", decl}}});
        }
        const std::string verb = on_delta ? ":streamGenerateContent?alt=sse" : ":generateContent";
        const std::string url = NoSlash(s_.base_url) + "/v1/projects/" + project + "/locations/" + loc + "/publishers/google/models/" + s_.model + verb;
        auto h = BaseHeaders(s_); h.push_back("Authorization: Bearer " + token);
        if (on_delta) {
            agent::CompletionResult r; r.message.role = "assistant"; r.finish_reason = "stop"; int n = 0;
            auto res = PostSse(url, h, b.dump(), [&](const std::string&, const std::string& data) {
                json e; try { e = json::parse(data); } catch (...) { return; }
                if (!e.contains("candidates") || e["candidates"].empty()) return;
                for (auto& p : e["candidates"][0].value("content", json::object()).value("parts", json::array())) {
                    if (p.contains("text")) { const auto t = p.value("text", std::string()); r.message.content += t; if (!t.empty()) (*on_delta)(t); }
                    else if (p.contains("functionCall")) {
                        agent::ToolCall tc; tc.id = "gv_" + std::to_string(++n); tc.name = p["functionCall"].value("name", ""); tc.arguments_json = p["functionCall"].value("args", json::object()).dump();
                        r.tool_calls.push_back(tc); r.message.tool_calls.push_back({{"id", tc.id}, {"type", "function"}, {"function", {{"name", tc.name}, {"arguments", tc.arguments_json}}}});
                    }
                }
            });
            if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
            if (!r.tool_calls.empty()) r.finish_reason = "tool_calls";
            r.http_status = static_cast<int>(res.status); return r;
        }
        auto res = Post(url, h, b.dump());
        if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
        agent::CompletionResult r; r.http_status = static_cast<int>(res.status); r.message.role = "assistant"; r.finish_reason = "stop";
        try {
            auto j = json::parse(res.body); int n = 0;
            if (!j.contains("candidates") || j["candidates"].empty()) { r.ok = false; r.finish_reason = "error"; r.error = s_.id + ": no candidates"; return r; }
            for (auto& p : j["candidates"][0].value("content", json::object()).value("parts", json::array())) {
                if (p.contains("text")) r.message.content += p.value("text", "");
                else if (p.contains("functionCall")) {
                    agent::ToolCall tc; tc.id = "gv_" + std::to_string(++n); tc.name = p["functionCall"].value("name", ""); tc.arguments_json = p["functionCall"].value("args", json::object()).dump();
                    r.tool_calls.push_back(tc); r.message.tool_calls.push_back({{"id", tc.id}, {"type", "function"}, {"function", {{"name", tc.name}, {"arguments", tc.arguments_json}}}});
                }
            }
            if (!r.tool_calls.empty()) r.finish_reason = "tool_calls";
        } catch (const std::exception& e) { r.ok = false; r.finish_reason = "error"; r.error = s_.id + std::string(" parse: ") + e.what(); }
        return r;
    }
private:
    ProviderSpec s_;
};

}  // namespace

std::shared_ptr<Provider> MakeCloudProvider(const ProviderSpec& s) {
    if (s.api == "bedrock-converse" || s.api == "bedrock-converse-stream") return std::make_shared<BedrockConverse>(s);
    if (s.api == "anthropic-vertex") return std::make_shared<AnthropicVertex>(s);
    if (s.api == "google-vertex") return std::make_shared<GoogleVertex>(s);
    return nullptr;
}

}  // namespace openclaw::provider
