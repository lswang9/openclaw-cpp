#include "openclaw/provider/native.hpp"
#include "openclaw/provider/cloud.hpp"

#include "openclaw/config/config.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <functional>
#include <map>
#include <sstream>

namespace openclaw::provider {
namespace {

using json = nlohmann::json;

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

// Server-sent events POST: on_event(event_name, data) for each event; returns status/body-on-error.
struct SseState { std::string buf, ev, data, raw; long status = 0; const std::function<void(const std::string&, const std::string&)>* cb; CURL* c; };
size_t SseSink(char* p, size_t s, size_t n, void* u) {
    auto* st = static_cast<SseState*>(u); const size_t len = s * n;
    if (!st->status) curl_easy_getinfo(st->c, CURLINFO_RESPONSE_CODE, &st->status);
    if (st->status && (st->status < 200 || st->status >= 300)) { st->raw.append(p, len); return len; }
    st->buf.append(p, len);
    std::size_t nl;
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
    try {
        auto j = json::parse(h.body);
        if (j.contains("error")) { const auto& e = j["error"]; r.error += ": " + (e.is_object() ? e.value("message", e.dump()) : e.is_string() ? e.get<std::string>() : e.dump()); }
    } catch (...) { if (!h.body.empty()) r.error += " | " + h.body.substr(0, 300); }
    return r;
}

std::string NoSlash(std::string s) { while (!s.empty() && s.back() == '/') s.pop_back(); return s; }

std::vector<std::string> BaseHeaders(const ProviderSpec& s) {
    std::vector<std::string> h{"Content-Type: application/json"};
    for (auto& [k, v] : s.headers.items()) if (v.is_string()) h.push_back(k + ": " + v.get<std::string>());
    return h;
}

// OpenAI tool schema → {name, description, parameters}
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

json ParseArgs(const std::string& s) { try { auto j = json::parse(s.empty() ? "{}" : s); return j.is_object() ? j : json{{"value", j}}; } catch (...) { return json{{"raw", s}}; } }

// data:<mime>;base64,<data> → (mime, data)
bool DataUrl(const std::string& url, std::string* mime, std::string* data) {
    if (url.rfind("data:", 0) != 0) return false;
    auto semi = url.find(";base64,"); if (semi == std::string::npos) return false;
    *mime = url.substr(5, semi - 5); *data = url.substr(semi + 8); return true;
}

// ---------------------------------------------------------------- OpenAI chat completions
class ChatCompletions : public Provider {
public:
    explicit ChatCompletions(ProviderSpec s) : s_(std::move(s)) {}
    json Body(const std::vector<agent::Message>& messages, const json& tools) const {
        json msgs = json::array(); for (auto& m : messages) msgs.push_back(m.ToJson());
        json b = {{"model", s_.model}, {"messages", msgs}};
        if (tools.is_array() && !tools.empty()) b["tools"] = tools;
        return b;
    }
    std::string Url() const { return NoSlash(s_.base_url) + "/chat/completions"; }
    agent::CompletionResult Complete(const std::vector<agent::Message>& messages, const json& tools) override {
        auto h = BaseHeaders(s_); if (!s_.api_key.empty()) h.push_back("Authorization: Bearer " + s_.api_key);
        auto res = Post(Url(), h, Body(messages, tools).dump());
        if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
        agent::CompletionResult r; r.http_status = static_cast<int>(res.status);
        try {
            auto j = json::parse(res.body); const auto& ch = j.at("choices").at(0);
            r.finish_reason = ch.value("finish_reason", "stop"); r.message = agent::Message::FromJson(ch.at("message"));
            for (auto& tc : r.message.tool_calls) {
                agent::ToolCall c; c.id = tc.value("id", ""); const auto& f = tc.value("function", json::object());
                c.name = f.value("name", ""); c.arguments_json = f.contains("arguments") ? (f["arguments"].is_string() ? f["arguments"].get<std::string>() : f["arguments"].dump()) : "{}";
                r.tool_calls.push_back(c);
            }
            if (!r.tool_calls.empty()) r.finish_reason = "tool_calls";
        } catch (const std::exception& e) { r.ok = false; r.finish_reason = "error"; r.error = s_.id + " parse error: " + e.what(); }
        return r;
    }
    agent::CompletionResult CompleteStreaming(const std::vector<agent::Message>& messages, const json& tools, DeltaFn on_delta) override {
        if (!on_delta) return Complete(messages, tools);
        return StreamChatCompletion(Url(), s_.api_key, Body(messages, tools), on_delta, s_.id);
    }
private:
    ProviderSpec s_;
};

// ---------------------------------------------------------------- OpenAI Responses API
class Responses : public Provider {
public:
    explicit Responses(ProviderSpec s) : s_(std::move(s)) {}
    agent::CompletionResult Complete(const std::vector<agent::Message>& messages, const json& tools) override { return Run(messages, tools, nullptr); }
    agent::CompletionResult CompleteStreaming(const std::vector<agent::Message>& messages, const json& tools, DeltaFn on_delta) override {
        return on_delta ? Run(messages, tools, &on_delta) : Run(messages, tools, nullptr);
    }
    agent::CompletionResult Run(const std::vector<agent::Message>& messages, const json& tools, const DeltaFn* on_delta) {
        json input = json::array(); std::string instructions;
        for (auto& m : messages) {
            if (m.role == "system") { instructions += (instructions.empty() ? "" : "\n\n") + m.content; continue; }
            if (m.role == "tool") { input.push_back({{"type", "function_call_output"}, {"call_id", m.tool_call_id.value_or("")}, {"output", m.content}}); continue; }
            if (m.role == "assistant") {
                if (!m.content.empty()) input.push_back({{"role", "assistant"}, {"content", m.content}});
                for (auto& tc : m.tool_calls) { const auto& f = tc.value("function", json::object()); input.push_back({{"type", "function_call"}, {"call_id", tc.value("id", "")}, {"name", f.value("name", "")}, {"arguments", f.contains("arguments") && f["arguments"].is_string() ? f["arguments"].get<std::string>() : f.value("arguments", json::object()).dump()}}); }
                continue;
            }
            if (m.HasMultimodal()) {
                json parts = json::array();
                for (auto& p : m.multimodal_parts) {
                    if (p.value("type", "") == "text") parts.push_back({{"type", "input_text"}, {"text", p.value("text", "")}});
                    else if (p.value("type", "") == "image_url") parts.push_back({{"type", "input_image"}, {"image_url", p["image_url"].is_object() ? p["image_url"].value("url", "") : p["image_url"].get<std::string>()}});
                }
                input.push_back({{"role", "user"}, {"content", parts}});
            } else input.push_back({{"role", "user"}, {"content", m.content}});
        }
        json b = {{"model", s_.model}, {"input", input}, {"store", false}};
        if (!instructions.empty()) b["instructions"] = instructions;
        auto td = Tools(tools);
        if (!td.empty()) { b["tools"] = json::array(); for (auto& t : td) b["tools"].push_back({{"type", "function"}, {"name", t.name}, {"description", t.description}, {"parameters", t.params}}); }
        auto h = BaseHeaders(s_); if (!s_.api_key.empty()) h.push_back("Authorization: Bearer " + s_.api_key);
        if (on_delta) {
            b["stream"] = true;
            agent::CompletionResult r; r.message.role = "assistant"; r.finish_reason = "stop"; std::string err;
            auto res = PostSse(NoSlash(s_.base_url) + "/responses", h, b.dump(), [&](const std::string& ev, const std::string& data) {
                json e; try { e = json::parse(data); } catch (...) { return; }
                const std::string type = e.value("type", ev);
                if (type == "response.output_text.delta") { const auto d = e.value("delta", std::string()); r.message.content += d; if (!d.empty()) (*on_delta)(d); }
                else if (type == "response.output_item.done" && e.contains("item") && e["item"].value("type", "") == "function_call") {
                    const auto& item = e["item"]; agent::ToolCall c; c.id = item.value("call_id", item.value("id", "")); c.name = item.value("name", ""); c.arguments_json = item.value("arguments", "{}");
                    r.tool_calls.push_back(c); r.message.tool_calls.push_back({{"id", c.id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", c.arguments_json}}}});
                } else if (type == "response.incomplete") r.finish_reason = "length";
                else if (type == "response.failed" || type == "error") err = e.contains("response") && e["response"].contains("error") ? e["response"]["error"].value("message", data) : e.value("message", data);
            });
            if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
            if (!err.empty()) { r.ok = false; r.finish_reason = "error"; r.error = s_.id + ": " + err; return r; }
            r.http_status = static_cast<int>(res.status); if (!r.tool_calls.empty()) r.finish_reason = "tool_calls";
            return r;
        }
        auto res = Post(NoSlash(s_.base_url) + "/responses", h, b.dump());
        if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
        agent::CompletionResult r; r.http_status = static_cast<int>(res.status); r.message.role = "assistant"; r.finish_reason = "stop";
        try {
            auto j = json::parse(res.body);
            for (auto& item : j.value("output", json::array())) {
                const auto type = item.value("type", "");
                if (type == "message") { for (auto& c : item.value("content", json::array())) if (c.value("type", "") == "output_text") r.message.content += c.value("text", ""); }
                else if (type == "function_call") {
                    agent::ToolCall c; c.id = item.value("call_id", item.value("id", "")); c.name = item.value("name", ""); c.arguments_json = item.value("arguments", "{}");
                    r.tool_calls.push_back(c); r.message.tool_calls.push_back({{"id", c.id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", c.arguments_json}}}});
                }
            }
            if (!r.tool_calls.empty()) r.finish_reason = "tool_calls";
            else if (j.value("status", "") == "incomplete") r.finish_reason = "length";
        } catch (const std::exception& e) { r.ok = false; r.finish_reason = "error"; r.error = s_.id + " parse error: " + e.what(); }
        return r;
    }
private:
    ProviderSpec s_;
};

// ---------------------------------------------------------------- Anthropic Messages
class AnthropicMessages : public Provider {
public:
    explicit AnthropicMessages(ProviderSpec s) : s_(std::move(s)) {}
    agent::CompletionResult Complete(const std::vector<agent::Message>& messages, const json& tools) override { return Run(messages, tools, nullptr); }
    agent::CompletionResult CompleteStreaming(const std::vector<agent::Message>& messages, const json& tools, DeltaFn on_delta) override {
        return on_delta ? Run(messages, tools, &on_delta) : Run(messages, tools, nullptr);
    }
    agent::CompletionResult Run(const std::vector<agent::Message>& messages, const json& tools, const DeltaFn* on_delta) {
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
                    push("assistant", {{"type", "tool_use"}, {"id", tc.value("id", "")}, {"name", f.value("name", "")}, {"input", ParseArgs(f.contains("arguments") && f["arguments"].is_string() ? f["arguments"].get<std::string>() : f.value("arguments", json::object()).dump())}});
                }
                continue;
            }
            if (m.HasMultimodal()) {
                for (auto& p : m.multimodal_parts) {
                    std::string mime, data;
                    if (p.value("type", "") == "text") push("user", {{"type", "text"}, {"text", p.value("text", "")}});
                    else if (p.value("type", "") == "image_url") {
                        const std::string url = p["image_url"].is_object() ? p["image_url"].value("url", "") : p["image_url"].get<std::string>();
                        if (DataUrl(url, &mime, &data)) push("user", {{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", mime}, {"data", data}}}});
                        else push("user", {{"type", "image"}, {"source", {{"type", "url"}, {"url", url}}}});
                    }
                }
            } else push("user", {{"type", "text"}, {"text", m.content.empty() ? std::string(" ") : m.content}});
        }
        json b = {{"model", s_.model}, {"max_tokens", s_.max_tokens}, {"messages", msgs}};
        if (!system.empty()) b["system"] = system;
        auto td = Tools(tools);
        if (!td.empty()) { b["tools"] = json::array(); for (auto& t : td) b["tools"].push_back({{"name", t.name}, {"description", t.description}, {"input_schema", t.params}}); }
        auto h = BaseHeaders(s_); h.push_back("anthropic-version: 2023-06-01");
        if (!s_.api_key.empty()) { if (s_.api_key.rfind("sk-ant-oat", 0) == 0) { h.push_back("Authorization: Bearer " + s_.api_key); h.push_back("anthropic-beta: oauth-2025-04-20"); } else h.push_back("x-api-key: " + s_.api_key); }
        if (on_delta) {
            b["stream"] = true;
            agent::CompletionResult r; r.message.role = "assistant"; r.finish_reason = "stop"; std::string err;
            std::map<int, agent::ToolCall> calls; std::map<int, std::string> partial;
            auto res = PostSse(NoSlash(s_.base_url) + "/v1/messages", h, b.dump(), [&](const std::string& ev, const std::string& data) {
                json e; try { e = json::parse(data); } catch (...) { return; }
                const std::string type = e.value("type", ev); const int idx = e.value("index", 0);
                if (type == "content_block_start" && e.contains("content_block") && e["content_block"].value("type", "") == "tool_use") { calls[idx].id = e["content_block"].value("id", ""); calls[idx].name = e["content_block"].value("name", ""); }
                else if (type == "content_block_delta" && e.contains("delta")) {
                    const auto& d = e["delta"];
                    if (d.value("type", "") == "text_delta") { const auto t = d.value("text", std::string()); r.message.content += t; if (!t.empty()) (*on_delta)(t); }
                    else if (d.value("type", "") == "input_json_delta") partial[idx] += d.value("partial_json", std::string());
                } else if (type == "message_delta" && e.contains("delta")) { const auto stop = e["delta"].value("stop_reason", std::string()); if (stop == "max_tokens") r.finish_reason = "length"; }
                else if (type == "error") err = e.contains("error") ? e["error"].value("message", data) : data;
            });
            if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
            if (!err.empty()) { r.ok = false; r.finish_reason = "error"; r.error = s_.id + ": " + err; return r; }
            for (auto& [i, c] : calls) { c.arguments_json = partial[i].empty() ? "{}" : partial[i]; r.tool_calls.push_back(c); r.message.tool_calls.push_back({{"id", c.id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", c.arguments_json}}}}); }
            r.http_status = static_cast<int>(res.status); if (!r.tool_calls.empty()) r.finish_reason = "tool_calls";
            return r;
        }
        auto res = Post(NoSlash(s_.base_url) + "/v1/messages", h, b.dump());
        if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
        agent::CompletionResult r; r.http_status = static_cast<int>(res.status); r.message.role = "assistant";
        try {
            auto j = json::parse(res.body);
            for (auto& c : j.value("content", json::array())) {
                const auto type = c.value("type", "");
                if (type == "text") r.message.content += c.value("text", "");
                else if (type == "tool_use") {
                    agent::ToolCall tc; tc.id = c.value("id", ""); tc.name = c.value("name", ""); tc.arguments_json = c.value("input", json::object()).dump();
                    r.tool_calls.push_back(tc); r.message.tool_calls.push_back({{"id", tc.id}, {"type", "function"}, {"function", {{"name", tc.name}, {"arguments", tc.arguments_json}}}});
                }
            }
            const auto stop = j.value("stop_reason", "end_turn");
            r.finish_reason = stop == "tool_use" ? "tool_calls" : stop == "max_tokens" ? "length" : "stop";
        } catch (const std::exception& e) { r.ok = false; r.finish_reason = "error"; r.error = s_.id + " parse error: " + e.what(); }
        return r;
    }
private:
    ProviderSpec s_;
};

// ---------------------------------------------------------------- Google Gemini generateContent
json GeminiSchema(const json& s) {
    if (s.is_object()) {
        json o = json::object();
        for (auto& [k, v] : s.items()) { if (k == "$schema" || k == "additionalProperties" || k == "$id" || k == "default" || k == "examples") continue; o[k] = GeminiSchema(v); }
        return o;
    }
    if (s.is_array()) { json a = json::array(); for (auto& x : s) a.push_back(GeminiSchema(x)); return a; }
    return s;
}
class Gemini : public Provider {
public:
    explicit Gemini(ProviderSpec s) : s_(std::move(s)) {}
    agent::CompletionResult Complete(const std::vector<agent::Message>& messages, const json& tools) override { return Run(messages, tools, nullptr); }
    agent::CompletionResult CompleteStreaming(const std::vector<agent::Message>& messages, const json& tools, DeltaFn on_delta) override {
        return on_delta ? Run(messages, tools, &on_delta) : Run(messages, tools, nullptr);
    }
    agent::CompletionResult Run(const std::vector<agent::Message>& messages, const json& tools, const DeltaFn* on_delta) {
        std::string system; json contents = json::array(); std::map<std::string, std::string> call_names;
        auto push = [&](const std::string& role, const json& part) {
            if (!contents.empty() && contents.back()["role"] == role) contents.back()["parts"].push_back(part);
            else contents.push_back({{"role", role}, {"parts", json::array({part})}});
        };
        for (auto& m : messages) {
            if (m.role == "system") { system += (system.empty() ? "" : "\n\n") + m.content; continue; }
            if (m.role == "tool") {
                std::string name = m.name.value_or(""); if (name.empty()) name = call_names[m.tool_call_id.value_or("")];
                json resp; try { resp = json::parse(m.content); if (!resp.is_object()) resp = json{{"content", resp}}; } catch (...) { resp = json{{"content", m.content}}; }
                push("user", {{"functionResponse", {{"name", name}, {"response", resp}}}}); continue;
            }
            if (m.role == "assistant") {
                if (!m.content.empty()) push("model", {{"text", m.content}});
                for (auto& tc : m.tool_calls) {
                    const auto& f = tc.value("function", json::object()); call_names[tc.value("id", "")] = f.value("name", "");
                    push("model", {{"functionCall", {{"name", f.value("name", "")}, {"args", ParseArgs(f.contains("arguments") && f["arguments"].is_string() ? f["arguments"].get<std::string>() : f.value("arguments", json::object()).dump())}}}});
                }
                continue;
            }
            if (m.HasMultimodal()) {
                for (auto& p : m.multimodal_parts) {
                    std::string mime, data;
                    if (p.value("type", "") == "text") push("user", {{"text", p.value("text", "")}});
                    else if (p.value("type", "") == "image_url") {
                        const std::string url = p["image_url"].is_object() ? p["image_url"].value("url", "") : p["image_url"].get<std::string>();
                        if (DataUrl(url, &mime, &data)) push("user", {{"inlineData", {{"mimeType", mime}, {"data", data}}}});
                        else push("user", {{"fileData", {{"fileUri", url}}}});
                    }
                }
            } else push("user", {{"text", m.content}});
        }
        json b = {{"contents", contents}, {"generationConfig", {{"maxOutputTokens", s_.max_tokens}}}};
        if (!system.empty()) b["systemInstruction"] = {{"parts", json::array({{{"text", system}}})}};
        auto td = Tools(tools);
        if (!td.empty()) { json decl = json::array(); for (auto& t : td) decl.push_back({{"name", t.name}, {"description", t.description}, {"parameters", GeminiSchema(t.params)}}); b["tools"] = json::array({{{"functionDeclarations", decl}}}); }
        auto h = BaseHeaders(s_); if (!s_.api_key.empty()) h.push_back("x-goog-api-key: " + s_.api_key);
        if (on_delta) {
            agent::CompletionResult r; r.message.role = "assistant"; r.finish_reason = "stop"; int n = 0; std::string err;
            auto res = PostSse(NoSlash(s_.base_url) + "/models/" + s_.model + ":streamGenerateContent?alt=sse", h, b.dump(), [&](const std::string&, const std::string& data) {
                json e; try { e = json::parse(data); } catch (...) { return; }
                if (e.contains("error")) { err = e["error"].value("message", data); return; }
                if (!e.contains("candidates") || e["candidates"].empty()) return;
                const auto& cand = e["candidates"][0];
                for (auto& p : cand.value("content", json::object()).value("parts", json::array())) {
                    if (p.contains("text") && !p.value("thought", false)) { const auto t = p.value("text", std::string()); r.message.content += t; if (!t.empty()) (*on_delta)(t); }
                    else if (p.contains("functionCall")) {
                        agent::ToolCall tc; tc.id = "gemini_call_" + std::to_string(++n); tc.name = p["functionCall"].value("name", ""); tc.arguments_json = p["functionCall"].value("args", json::object()).dump();
                        r.tool_calls.push_back(tc); r.message.tool_calls.push_back({{"id", tc.id}, {"type", "function"}, {"function", {{"name", tc.name}, {"arguments", tc.arguments_json}}}});
                    }
                }
                if (cand.value("finishReason", "") == "MAX_TOKENS") r.finish_reason = "length";
            });
            if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
            if (!err.empty()) { r.ok = false; r.finish_reason = "error"; r.error = s_.id + ": " + err; return r; }
            r.http_status = static_cast<int>(res.status); if (!r.tool_calls.empty()) r.finish_reason = "tool_calls";
            return r;
        }
        auto res = Post(NoSlash(s_.base_url) + "/models/" + s_.model + ":generateContent", h, b.dump());
        if (!res.error.empty() || res.status < 200 || res.status >= 300) return Fail(s_.id, res);
        agent::CompletionResult r; r.http_status = static_cast<int>(res.status); r.message.role = "assistant"; r.finish_reason = "stop";
        try {
            auto j = json::parse(res.body);
            if (!j.contains("candidates") || j["candidates"].empty()) { r.ok = false; r.finish_reason = "error"; r.error = s_.id + ": no candidates" + (j.contains("promptFeedback") ? " (" + j["promptFeedback"].dump() + ")" : ""); return r; }
            const auto& cand = j["candidates"][0]; int n = 0;
            for (auto& p : cand.value("content", json::object()).value("parts", json::array())) {
                if (p.contains("text") && !p.value("thought", false)) r.message.content += p.value("text", "");
                else if (p.contains("functionCall")) {
                    agent::ToolCall tc; tc.id = "gemini_call_" + std::to_string(++n); tc.name = p["functionCall"].value("name", ""); tc.arguments_json = p["functionCall"].value("args", json::object()).dump();
                    r.tool_calls.push_back(tc); r.message.tool_calls.push_back({{"id", tc.id}, {"type", "function"}, {"function", {{"name", tc.name}, {"arguments", tc.arguments_json}}}});
                }
            }
            if (!r.tool_calls.empty()) r.finish_reason = "tool_calls"; else if (cand.value("finishReason", "") == "MAX_TOKENS") r.finish_reason = "length";
        } catch (const std::exception& e) { r.ok = false; r.finish_reason = "error"; r.error = s_.id + " parse error: " + e.what(); }
        return r;
    }
private:
    ProviderSpec s_;
};

struct Builtin { const char* api; const char* base; const char* env; };
const std::map<std::string, Builtin>& Builtins() {
    static const std::map<std::string, Builtin> m = {
        {"openai", {"openai-responses", "https://api.openai.com/v1", "OPENAI_API_KEY"}},
        {"anthropic", {"anthropic-messages", "https://api.anthropic.com", "ANTHROPIC_API_KEY,ANTHROPIC_OAUTH_TOKEN"}},
        {"google", {"google-generative-ai", "https://generativelanguage.googleapis.com/v1beta", "GEMINI_API_KEY,GOOGLE_API_KEY"}},
        {"deepseek", {"openai-completions", "https://api.deepseek.com/v1", "DEEPSEEK_API_KEY"}},
        {"openrouter", {"openai-completions", "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY"}},
        {"groq", {"openai-completions", "https://api.groq.com/openai/v1", "GROQ_API_KEY"}},
        {"mistral", {"openai-completions", "https://api.mistral.ai/v1", "MISTRAL_API_KEY"}},
        {"xai", {"openai-completions", "https://api.x.ai/v1", "XAI_API_KEY"}},
        {"together", {"openai-completions", "https://api.together.xyz/v1", "TOGETHER_API_KEY"}},
        {"fireworks", {"openai-completions", "https://api.fireworks.ai/inference/v1", "FIREWORKS_API_KEY"}},
        {"cerebras", {"openai-completions", "https://api.cerebras.ai/v1", "CEREBRAS_API_KEY"}},
        {"deepinfra", {"openai-completions", "https://api.deepinfra.com/v1/openai", "DEEPINFRA_API_KEY"}},
        {"perplexity", {"openai-completions", "https://api.perplexity.ai", "PERPLEXITY_API_KEY"}},
        {"moonshot", {"openai-completions", "https://api.moonshot.ai/v1", "MOONSHOT_API_KEY"}},
        {"zai", {"openai-completions", "https://api.z.ai/api/paas/v4", "ZAI_API_KEY"}},
        {"nvidia", {"openai-completions", "https://integrate.api.nvidia.com/v1", "NVIDIA_API_KEY"}},
        {"huggingface", {"openai-completions", "https://router.huggingface.co/v1", "HF_TOKEN,HUGGINGFACE_HUB_TOKEN"}},
        {"venice", {"openai-completions", "https://api.venice.ai/api/v1", "VENICE_API_KEY"}},
        {"cohere", {"openai-completions", "https://api.cohere.ai/compatibility/v1", "COHERE_API_KEY"}},
        {"qwen", {"openai-completions", "https://dashscope-intl.aliyuncs.com/compatible-mode/v1", "DASHSCOPE_API_KEY,QWEN_API_KEY"}},
        {"minimax", {"anthropic-messages", "https://api.minimax.io/anthropic", "MINIMAX_API_KEY"}},
        {"kimi-coding", {"anthropic-messages", "https://api.kimi.com/coding", "KIMI_API_KEY"}},
        {"ollama", {"openai-completions", "http://127.0.0.1:11434/v1", ""}},
        {"lmstudio", {"openai-completions", "http://127.0.0.1:1234/v1", ""}},
        {"vllm", {"openai-completions", "http://127.0.0.1:8000/v1", ""}},
        {"sglang", {"openai-completions", "http://127.0.0.1:30000/v1", ""}},
        {"llama-cpp", {"openai-completions", "http://127.0.0.1:8080/v1", ""}},
        {"litellm", {"openai-completions", "http://127.0.0.1:4000/v1", "LITELLM_API_KEY"}},
        {"amazon-bedrock", {"bedrock-converse", "https://bedrock-runtime.us-east-1.amazonaws.com", ""}},
        {"anthropic-vertex", {"anthropic-vertex", "https://us-east1-aiplatform.googleapis.com", ""}},
        {"google-vertex", {"google-vertex", "https://us-east1-aiplatform.googleapis.com", ""}}};
    return m;
}

std::string NormalizeApi(std::string a) {
    if (a == "openai" || a == "openai-chat" || a == "openai-completions" || a == "chat-completions") return "openai-completions";
    if (a == "responses" || a == "openai-responses" || a == "openai-codex-responses") return "openai-responses";
    if (a == "anthropic" || a == "anthropic-messages") return "anthropic-messages";
    if (a == "google" || a == "gemini" || a == "google-generative-ai") return "google-generative-ai";
    if (a == "bedrock" || a == "bedrock-converse" || a == "bedrock-converse-stream") return "bedrock-converse";
    if (a == "anthropic-vertex") return "anthropic-vertex";
    if (a == "google-vertex" || a == "vertex" || a == "vertex-ai") return "google-vertex";
    return a;
}

}  // namespace

bool BuiltinProviderDefaults(const std::string& id, std::string* api, std::string* base_url, std::string* env_keys) {
    auto it = Builtins().find(id); if (it == Builtins().end()) return false;
    *api = it->second.api; *base_url = it->second.base; *env_keys = it->second.env; return true;
}

bool ResolveProviderSpec(const json& cfg_raw, const std::string& route, ProviderSpec* out, std::string* why) {
    auto slash = route.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= route.size()) { if (why) *why = "model ref must be provider/model"; return false; }
    ProviderSpec s; s.id = route.substr(0, slash); s.model = route.substr(slash + 1);
    json pc = json::object();
    if (cfg_raw.is_object() && cfg_raw.contains("models") && cfg_raw["models"].is_object() && cfg_raw["models"].contains("providers") && cfg_raw["models"]["providers"].contains(s.id))
        pc = cfg_raw["models"]["providers"][s.id];
    try { pc = config::ResolveSecretRefs(pc, cfg_raw, "models.providers." + s.id); } catch (const std::exception& e) { if (why) *why = e.what(); return false; }
    std::string api, base, envs;
    const bool builtin = BuiltinProviderDefaults(s.id, &api, &base, &envs);
    if (!builtin && pc.empty()) { if (why) *why = "unknown provider \"" + s.id + "\" (configure models.providers." + s.id + ")"; return false; }
    if (pc.contains("api") && pc["api"].is_string()) api = pc["api"].get<std::string>();
    if (pc.contains("baseUrl") && pc["baseUrl"].is_string()) base = pc["baseUrl"].get<std::string>();
    if (api.empty()) api = "openai-completions";
    s.api = NormalizeApi(api); s.base_url = base;
    if (s.base_url.empty()) { if (why) *why = "provider \"" + s.id + "\" has no baseUrl"; return false; }
    if (pc.contains("apiKey") && pc["apiKey"].is_string()) {
        s.api_key = pc["apiKey"].get<std::string>();
        if (s.api_key.size() > 3 && s.api_key[0] == '$' && s.api_key[1] == '{' && s.api_key.back() == '}') { const char* e = std::getenv(s.api_key.substr(2, s.api_key.size() - 3).c_str()); s.api_key = e ? e : ""; }
    }
    if (s.api_key.empty() && !envs.empty()) { std::stringstream ss(envs); std::string k; while (std::getline(ss, k, ',')) if (const char* e = std::getenv(k.c_str()); e && *e) { s.api_key = e; break; } }
    if (pc.contains("auth") && pc["auth"].is_string()) s.auth = pc["auth"].get<std::string>();
    if (pc.contains("region") && pc["region"].is_string()) s.region = pc["region"].get<std::string>();
    if (pc.contains("project") && pc["project"].is_string()) s.project = pc["project"].get<std::string>();
    if (pc.contains("location") && pc["location"].is_string()) s.location = pc["location"].get<std::string>();
    if (s.auth.empty()) {
        if (s.api == "bedrock-converse") s.auth = "aws-sdk";
        else if (s.api == "anthropic-vertex" || s.api == "google-vertex") s.auth = "google-adc";
    }
    const bool cloud_auth = s.auth == "aws-sdk" || s.auth == "google-adc";
    const bool local = base.find("127.0.0.1") != std::string::npos || base.find("localhost") != std::string::npos;
    if (s.api_key.empty() && !local && builtin && !envs.empty() && !cloud_auth) { if (why) *why = "no API key for provider \"" + s.id + "\" (set models.providers." + s.id + ".apiKey or " + envs + ")"; return false; }
    if (pc.contains("headers") && pc["headers"].is_object()) s.headers = pc["headers"];
    for (auto& m : pc.value("models", json::array())) if (m.is_object() && m.value("id", "") == s.model) { s.max_tokens = m.value("maxTokens", s.max_tokens); if (m.contains("api") && m["api"].is_string()) s.api = NormalizeApi(m["api"].get<std::string>()); }
    static const char* kOk[] = {"openai-completions","openai-responses","anthropic-messages","google-generative-ai","bedrock-converse","anthropic-vertex","google-vertex"};
    bool ok_api = false; for (auto* x : kOk) if (s.api == x) { ok_api = true; break; }
    if (!ok_api) { if (why) *why = "unsupported provider api \"" + s.api + "\""; return false; }
    *out = s; return true;
}

std::shared_ptr<Provider> MakeNativeProvider(const ProviderSpec& s) {
    if (auto cloud = MakeCloudProvider(s)) return cloud;
    if (s.api == "anthropic-messages") return std::make_shared<AnthropicMessages>(s);
    if (s.api == "google-generative-ai") return std::make_shared<Gemini>(s);
    if (s.api == "openai-responses") return std::make_shared<Responses>(s);
    return std::make_shared<ChatCompletions>(s);
}

std::shared_ptr<Provider> MakeConfiguredProvider(const json& cfg_raw, const std::string& route, std::string* why) {
    ProviderSpec s; if (!ResolveProviderSpec(cfg_raw, route, &s, why)) return nullptr;
    return MakeNativeProvider(s);
}

}  // namespace openclaw::provider
