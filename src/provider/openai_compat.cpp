#include "openclaw/provider/openai_compat.hpp"

#include <curl/curl.h>

#include <stdexcept>
#include <string>

namespace openclaw::provider {
namespace {

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

agent::CompletionResult Fail(int http, const std::string& body, const std::string& hint) {
    agent::CompletionResult r;
    r.ok = false;
    r.http_status = http;
    r.finish_reason = "error";
    r.error = hint;
    if (!body.empty()) {
        try {
            const auto j = nlohmann::json::parse(body);
            if (j.contains("error")) {
                if (j["error"].is_object() && j["error"].contains("message")) {
                    r.error += ": " + j["error"]["message"].get<std::string>();
                } else if (j["error"].is_string()) {
                    r.error += ": " + j["error"].get<std::string>();
                }
            }
        } catch (...) {
            r.error += " | body=" + body.substr(0, 400);
        }
    }
    return r;
}

}  // namespace

OpenAiCompatProvider::OpenAiCompatProvider(std::string name, std::string api_key,
                                           std::string base_url, std::string model)
    : name_(std::move(name)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)),
      model_(std::move(model)) {}

agent::CompletionResult OpenAiCompatProvider::Complete(const std::vector<agent::Message>& messages,
                                                       const nlohmann::json& tools) {
    nlohmann::json body;
    body["model"] = model_;
    body["stream"] = false;
    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : messages) msgs.push_back(m.ToJson());
    body["messages"] = msgs;
    if (!tools.empty()) body["tools"] = tools;

    const std::string payload = body.dump();
    std::string url = base_url_;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/v1/chat/completions";

    CURL* curl = curl_easy_init();
    if (!curl) return Fail(0, "", "curl_easy_init failed");

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string auth = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        return Fail(0, "", std::string(name_) + " curl error: " + curl_easy_strerror(rc));
    }
    if (http_code == 402) return Fail(402, response, name_ + " HTTP 402");
    if (http_code == 401) return Fail(401, response, name_ + " HTTP 401");
    if (http_code == 429) return Fail(429, response, name_ + " HTTP 429");
    if (http_code < 200 || http_code >= 300) {
        return Fail(static_cast<int>(http_code), response,
                    name_ + " HTTP " + std::to_string(http_code));
    }

    try {
        const auto j = nlohmann::json::parse(response);
        if (!j.contains("choices") || j["choices"].empty()) {
            return Fail(static_cast<int>(http_code), response, "no choices");
        }
        const auto& choice = j["choices"][0];
        agent::CompletionResult r;
        r.ok = true;
        r.http_status = static_cast<int>(http_code);
        r.finish_reason = choice.value("finish_reason", "stop");
        r.message = agent::Message::FromJson(choice.at("message"));
        if (r.message.tool_calls.is_array()) {
            for (const auto& tc : r.message.tool_calls) {
                agent::ToolCall call;
                call.id = tc.value("id", "");
                if (tc.contains("function")) {
                    call.name = tc["function"].value("name", "");
                    if (tc["function"].contains("arguments")) {
                        const auto& args = tc["function"]["arguments"];
                        call.arguments_json =
                            args.is_string() ? args.get<std::string>() : args.dump();
                    }
                }
                r.tool_calls.push_back(std::move(call));
            }
        }
        return r;
    } catch (const std::exception& e) {
        return Fail(static_cast<int>(http_code), response, std::string("parse: ") + e.what());
    }
}

agent::CompletionResult StreamChatCompletion(const std::string& url, const std::string& bearer,
                                             const nlohmann::json& body_in, const Provider::DeltaFn& on_delta,
                                             const std::string& label) {
    nlohmann::json body = body_in;
    body["stream"] = true;
    const std::string payload = body.dump();
    struct St {
        std::string buf, raw, content, finish = "stop", err;
        std::vector<agent::ToolCall> calls;
        const Provider::DeltaFn* cb = nullptr;
    } st;
    st.cb = &on_delta;
    auto write = [](char* ptr, size_t size, size_t n, void* ud) -> size_t {
        auto* s = static_cast<St*>(ud);
        s->buf.append(ptr, size * n);
        if (s->raw.size() < (8u << 20)) s->raw.append(ptr, size * n);
        for (;;) {
            auto pos = s->buf.find('\n');
            if (pos == std::string::npos) break;
            std::string line = s->buf.substr(0, pos);
            s->buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) != 0) continue;
            std::string data = line.substr(5);
            while (!data.empty() && data.front() == ' ') data.erase(data.begin());
            if (data == "[DONE]") continue;
            auto j = nlohmann::json::parse(data, nullptr, false);
            if (!j.is_object()) continue;
            if (j.contains("error")) { s->err = j["error"].dump(); continue; }
            if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) continue;
            const auto& ch = j["choices"][0];
            if (ch.contains("finish_reason") && ch["finish_reason"].is_string()) s->finish = ch["finish_reason"];
            const auto d = ch.value("delta", nlohmann::json::object());
            if (d.contains("content") && d["content"].is_string()) {
                const std::string piece = d["content"];
                s->content += piece;
                if (*s->cb && !piece.empty()) (*s->cb)(piece);
            }
            if (d.contains("tool_calls") && d["tool_calls"].is_array())
                for (const auto& tc : d["tool_calls"]) {
                    const std::size_t idx = tc.value("index", static_cast<int>(s->calls.size()));
                    if (s->calls.size() <= idx) s->calls.resize(idx + 1);
                    auto& c = s->calls[idx];
                    if (tc.contains("id") && tc["id"].is_string()) c.id = tc["id"];
                    if (tc.contains("function")) {
                        const auto& f = tc["function"];
                        if (f.contains("name") && f["name"].is_string()) c.name += f["name"].get<std::string>();
                        if (f.contains("arguments") && f["arguments"].is_string()) c.arguments_json += f["arguments"].get<std::string>();
                    }
                }
        }
        return size * n;
    };
    CURL* curl = curl_easy_init();
    if (!curl) return Fail(0, "", "curl_easy_init failed");
    curl_slist* h = curl_slist_append(nullptr, "Content-Type: application/json");
    h = curl_slist_append(h, "Accept: text/event-stream");
    const std::string auth = "Authorization: Bearer " + bearer;
    h = curl_slist_append(h, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    const CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) return Fail(0, "", label + " curl error: " + curl_easy_strerror(rc));
    if (code < 200 || code >= 300) return Fail(static_cast<int>(code), st.raw, label + " HTTP " + std::to_string(code));
    if (!st.err.empty()) return Fail(static_cast<int>(code), st.err, label + " stream error");
    if (st.content.empty() && st.calls.empty()) {
        // Server ignored stream:true and answered with a plain completion object.
        auto j = nlohmann::json::parse(st.raw, nullptr, false);
        if (j.is_object() && j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            agent::CompletionResult r;
            r.ok = true;
            r.http_status = static_cast<int>(code);
            r.finish_reason = j["choices"][0].value("finish_reason", "stop");
            r.message = agent::Message::FromJson(j["choices"][0].at("message"));
            if (r.message.tool_calls.is_array())
                for (const auto& tc : r.message.tool_calls) {
                    agent::ToolCall c;
                    c.id = tc.value("id", "");
                    if (tc.contains("function")) {
                        c.name = tc["function"].value("name", "");
                        const auto& a = tc["function"].value("arguments", nlohmann::json("{}"));
                        c.arguments_json = a.is_string() ? a.get<std::string>() : a.dump();
                    }
                    r.tool_calls.push_back(c);
                }
            if (on_delta && !r.message.content.empty()) on_delta(r.message.content);
            return r;
        }
    }
    agent::CompletionResult r;
    r.ok = true;
    r.http_status = static_cast<int>(code);
    r.finish_reason = st.finish;
    r.message.role = "assistant";
    r.message.content = st.content;
    if (!st.calls.empty()) {
        r.message.tool_calls = nlohmann::json::array();
        for (std::size_t i = 0; i < st.calls.size(); ++i) {
            auto& c = st.calls[i];
            if (c.id.empty()) c.id = "call_" + std::to_string(i);
            if (c.arguments_json.empty()) c.arguments_json = "{}";
            r.message.tool_calls.push_back({{"id", c.id}, {"type", "function"},
                                            {"function", {{"name", c.name}, {"arguments", c.arguments_json}}}});
        }
        r.tool_calls = st.calls;
        if (r.finish_reason == "stop") r.finish_reason = "tool_calls";
    }
    return r;
}

agent::CompletionResult OpenAiCompatProvider::CompleteStreaming(const std::vector<agent::Message>& messages,
                                                                const nlohmann::json& tools, DeltaFn on_delta) {
    nlohmann::json body{{"model", model_}};
    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : messages) msgs.push_back(m.ToJson());
    body["messages"] = msgs;
    if (!tools.empty()) body["tools"] = tools;
    std::string url = base_url_;
    if (!url.empty() && url.back() == '/') url.pop_back();
    return StreamChatCompletion(url + "/v1/chat/completions", api_key_, body, on_delta, name_);
}

}  // namespace openclaw::provider
