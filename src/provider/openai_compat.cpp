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

}  // namespace openclaw::provider
