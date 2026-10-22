#pragma once
// Amazon Bedrock Converse + Google Vertex (Anthropic rawPredict / Gemini generateContent) providers.
#include "openclaw/provider/native.hpp"

#include <memory>

namespace openclaw::provider {

std::shared_ptr<Provider> MakeCloudProvider(const ProviderSpec& spec);

}  // namespace openclaw::provider
