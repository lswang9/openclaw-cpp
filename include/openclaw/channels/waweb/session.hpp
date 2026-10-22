#pragma once
// 1.163: live Noise session over TCP or WebSocket (Baileys wss://web.whatsapp.com/ws/chat shape). Offline fake uses
// the same framing; the client speaks curl WebSocket when the URL is ws:// or wss://.
#include "openclaw/channels/waweb/pairing.hpp"

#include <functional>
#include <optional>
#include <string>

namespace openclaw::channels::waweb {

struct LoginResult {
    bool ok = false;
    std::string error;
    std::string stage;  // hello|server-hello|finish|pair|transport
    bool registered = false;
    std::string me_jid, platform;
    int frames_out = 0, frames_in = 0;
};

// Run a companion Noise login against url ("tcp://host:port" or "ws://host:port"). When unregistered, sends a
// registration ClientPayload and expects a pair-success IQ; on success updates and saves creds. When already
// registered, sends a login ClientPayload and reads until idle_ms of silence (default 500) or max_frames.
LoginResult RunNoiseLogin(AuthCreds& creds, const std::filesystem::path& auth_dir, const std::string& url,
                          int idle_ms = 800, int max_frames = 8);

// Blocking fake WA Noise server (TCP). Speaks ServerHandshake, then sends a pair-success IQ for registration
// payloads and expects pair-device-sign. Writes a JSON line per event to log_path when non-empty. Stops after
// one successful session (or on error). Returns 0 on success.
int RunFakeNoiseServer(const std::string& bind_host, int port, const std::filesystem::path& log_path,
                       const std::string& pair_jid = "15550001234:1@s.whatsapp.net");

}  // namespace openclaw::channels::waweb
