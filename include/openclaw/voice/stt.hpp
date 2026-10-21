#pragma once
// Speech-to-text interface. HTTP backend: OpenAI-compatible `POST {OPENCLAW_STT_URL}` multipart
// (file, model=OPENCLAW_STT_MODEL|whisper-1, language?) with Bearer OPENCLAW_STT_API_KEY → {text}.
#include <nlohmann/json.hpp>
#include <string>

namespace openclaw::voice {

class SttEngine {
public:
    virtual ~SttEngine() = default;
    virtual nlohmann::json Transcribe(const std::string& audio, const std::string& mime, const std::string& filename,
                                      const std::string& language) = 0;
};
// From env; returns an engine that reports {ok:false,error} when no URL is configured.
SttEngine& DefaultStt();

}  // namespace openclaw::voice
