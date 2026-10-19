#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace openclaw::voice {

// Optional HTTP TTS. Env:
//   OPENCLAW_TTS_URL      — POST JSON {text, voice?} ; response body saved as audio bytes
//   OPENCLAW_TTS_API_KEY  — optional Bearer / X-API-Key
// Without URL: record-only (mode=record_only), no fake audio synthesis.
class TtsEngine {
public:
    TtsEngine();
    nlohmann::json Status() const;
    nlohmann::json Providers() const;
    void SetProvider(const std::string& id);
    void SetPersona(const std::string& persona);
    void SetEnabled(bool on);
    bool Enabled() const { return enabled_; }
    std::string Provider() const { return provider_; }
    std::string Persona() const { return persona_; }

    // Speak: returns {mode, delivered, ...}. Saves audio to media_dir when HTTP returns bytes.
    nlohmann::json Speak(const std::string& text, const std::string& voice = {});
    nlohmann::json Convert(const std::string& text);  // alias Speak
    nlohmann::json Log() const { return log_; }

private:
    bool enabled_ = false;
    std::string provider_ = "http";
    std::string persona_ = "default";
    std::string url_;
    std::string api_key_;
    std::vector<nlohmann::json> log_;
    nlohmann::json HttpPost(const std::string& text, const std::string& voice) const;
};

}  // namespace openclaw::voice
