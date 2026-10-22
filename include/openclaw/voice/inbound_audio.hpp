#pragma once
// Auto-transcription of inbound voice notes (Telegram voice/audio, WhatsApp Cloud audio, Discord audio attachments,
// generic media[].url): resolves the media bytes via the channel API (configured base + token) or an SSRF-guarded
// fetch, runs DefaultStt(), and prepends "[Audio] Transcript: ..." to the message text. Disabled by
// tools.media.audio.enabled=false or OPENCLAW_AUDIO_TRANSCRIBE=0; needs an STT provider (OPENCLAW_STT_*).
#include "openclaw/channels/registry.hpp"

#include <nlohmann/json.hpp>

namespace openclaw::voice {
nlohmann::json TranscribeInboundAudio(const nlohmann::json& cfg, channels::InboundMessage& m);
// Extract audio refs from common channel payload shapes (Telegram update, WhatsApp Cloud webhook, Discord message).
nlohmann::json ExtractAudioRefs(const nlohmann::json& body);
// 1.135: download non-audio inbound media refs (Telegram file ids, URLs) into media_dir; images up to 5 MB also get a
// base64 "dataUrl" so the turn can pass them to the model as image input. Each ref gains path/bytes or error.
nlohmann::json ResolveInboundMedia(const nlohmann::json& cfg, channels::InboundMessage& m, const std::string& media_dir);
}  // namespace openclaw::voice
