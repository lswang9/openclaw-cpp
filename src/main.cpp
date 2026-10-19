// openclaw-cpp first knife: stdin/CLI → DeepSeek chat completions → stdout reply.
// Alignment target: OpenClaw 2026.10.5 capabilities (full rewrite over time).

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string EnvOr(const char* key, std::string_view fallback) {
    if (const char* v = std::getenv(key); v && *v) {
        return std::string{v};
    }
    return std::string{fallback};
}

std::string ReadUserMessage(int argc, char** argv) {
    if (argc > 1) {
        std::ostringstream oss;
        for (int i = 1; i < argc; ++i) {
            if (i > 1) {
                oss << ' ';
            }
            oss << argv[i];
        }
        return oss.str();
    }
    std::ostringstream oss;
    oss << std::cin.rdbuf();
    std::string s = oss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

std::string ChatCompletion(const std::string& api_key,
                           const std::string& base_url,
                           const std::string& model,
                           const std::string& user_message) {
    nlohmann::json body = {
        {"model", model},
        {"messages",
         nlohmann::json::array({
             {{"role", "user"}, {"content", user_message}},
         })},
        {"stream", false},
    };
    const std::string payload = body.dump();
    const std::string url = base_url + "/v1/chat/completions";

    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string auth = "Authorization: Bearer " + api_key;
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
        throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(rc));
    }
    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error("HTTP " + std::to_string(http_code) + ": " + response);
    }

    const auto j = nlohmann::json::parse(response);
    if (!j.contains("choices") || j["choices"].empty()) {
        throw std::runtime_error("no choices in response: " + response);
    }
    const auto& msg = j["choices"][0]["message"];
    if (!msg.contains("content") || !msg["content"].is_string()) {
        throw std::runtime_error("missing message.content: " + response);
    }
    return msg["content"].get<std::string>();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string api_key = EnvOr("DEEPSEEK_API_KEY", "");
        if (api_key.empty()) {
            std::cerr << "error: set DEEPSEEK_API_KEY (see .env.example)\n";
            return 1;
        }

        const std::string user_message = ReadUserMessage(argc, argv);
        if (user_message.empty()) {
            std::cerr << "usage: openclaw-cpp <message>\n"
                         "   or: echo <message> | openclaw-cpp\n";
            return 1;
        }

        const std::string base_url = EnvOr("DEEPSEEK_BASE_URL", "https://api.deepseek.com");
        const std::string model = EnvOr("DEEPSEEK_MODEL", "deepseek-chat");

        curl_global_init(CURL_GLOBAL_DEFAULT);
        const std::string reply = ChatCompletion(api_key, base_url, model, user_message);
        curl_global_cleanup();

        std::cout << reply << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        curl_global_cleanup();
        return 1;
    }
}
