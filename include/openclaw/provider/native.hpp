#pragma once
// Config-driven model providers (1.125): `models.providers.<id>` + built-in defaults, routed per "provider/model".
// Native wire formats: OpenAI chat completions, OpenAI Responses, Anthropic Messages, Google Gemini generateContent,
// plus Bedrock Converse and Vertex (anthropic-vertex / google-vertex) via cloud.cpp (1.137).
#include "openclaw/provider/provider.hpp"

#include <memory>
#include <string>

namespace openclaw::provider {

struct ProviderSpec {
    std::string id, api, base_url, api_key, model;
    std::string auth;              // "", "api-key", "aws-sdk", "google-adc"
    std::string region, project, location;  // Bedrock region / Vertex project+location
    nlohmann::json headers = nlohmann::json::object();
    int max_tokens = 4096;
};

// Built-in defaults for a provider id (base URL, api kind, env var names). Returns false when unknown.
bool BuiltinProviderDefaults(const std::string& id, std::string* api, std::string* base_url, std::string* env_keys);

// Resolve "provider/model" against config + built-ins. Returns false (with *why) when no provider can be built
// (unknown provider, or a provider that needs a key and none is configured).
bool ResolveProviderSpec(const nlohmann::json& cfg_raw, const std::string& route, ProviderSpec* out, std::string* why);

std::shared_ptr<Provider> MakeNativeProvider(const ProviderSpec& spec);

// Convenience: resolve + make. nullptr when unresolvable.
std::shared_ptr<Provider> MakeConfiguredProvider(const nlohmann::json& cfg_raw, const std::string& route, std::string* why = nullptr);

}  // namespace openclaw::provider
