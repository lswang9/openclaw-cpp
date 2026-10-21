#pragma once
// Discord voice (v8 voice gateway) offline-testable layers: voice WS handshake (Hello/Identify/Ready/Select
// Protocol/Session Description/Speaking/Heartbeat v8), UDP IP discovery (74-byte), RTP framing (0x80 0x78, seq,
// ts += 960, ssrc), transport encryption aead_aes256_gcm_rtpsize and aead_xchacha20_poly1305_rtpsize (32-bit nonce
// counter appended), Ogg Opus demux (packets from OpusHead/OpusTags streams) and 20 ms paced playback with 5 silence
// frames. Live use needs the endpoint/session/token from the main gateway VOICE_SERVER_UPDATE / VOICE_STATE_UPDATE.
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::channels::discord_voice {

using Bytes = std::vector<std::uint8_t>;

Bytes IpDiscoveryRequest(std::uint32_t ssrc);
struct Discovered { std::string ip; std::uint16_t port = 0; };
std::optional<Discovered> ParseIpDiscoveryResponse(const Bytes& b);

Bytes RtpHeader(std::uint16_t seq, std::uint32_t ts, std::uint32_t ssrc);
// mode: "aead_aes256_gcm_rtpsize" | "aead_xchacha20_poly1305_rtpsize". Packet = header || ciphertext+tag || nonce4.
std::optional<Bytes> EncryptRtp(const std::string& mode, const Bytes& key, const Bytes& header, const Bytes& opus, std::uint32_t nonce);
std::optional<Bytes> DecryptRtp(const std::string& mode, const Bytes& key, const Bytes& packet);
Bytes HChaCha20(const Bytes& key32, const Bytes& nonce16);  // exposed for test vectors

// Ogg Opus demux → audio packets (header pages skipped). Empty on malformed input.
std::vector<Bytes> OggOpusPackets(const Bytes& ogg, std::string* err = nullptr);
inline const Bytes kOpusSilence{0xF8, 0xFF, 0xFE};

// Decrypt + strip the (encrypted) RTP header-extension body → raw Opus payload.
std::optional<Bytes> OpusPayload(const std::string& mode, const Bytes& key, const Bytes& packet);
bool IsRtcp(const Bytes& packet);

// PCM helpers (s16 interleaved stereo 48 kHz).
using Pcm = std::vector<std::int16_t>;
bool HaveOpus();
std::vector<Bytes> EncodePcm(const Pcm& stereo48k, std::string* err = nullptr);  // 20 ms frames
Pcm DecodeOpus(const std::vector<Bytes>& packets, std::string* err = nullptr);
Bytes WavFromPcm(const Pcm& stereo48k);
// WAV (s16le, 48 kHz, mono/stereo) → stereo PCM; other formats via ffmpeg when on PATH. Empty on failure.
Pcm PcmFromAudioFile(const Bytes& data, std::string* err = nullptr);
// Ogg Opus → packets as-is; anything else → decode to PCM then Opus-encode.
std::vector<Bytes> PacketsFromAudio(const Bytes& data, std::string* err = nullptr);

struct PlayRequest {
    std::string endpoint;  // ws(s)://host[:port]/?v=8
    std::string server_id, user_id, session_id, token;
    std::vector<Bytes> packets;
    int frame_ms = 20;
    int listen_ms = 0;          // after playback, receive others' audio this long
    std::string receive_dir;    // when set, decoded per-SSRC WAVs land here
};
// Full session: connect, handshake, discovery, speaking, paced send, silence, speaking off. Returns a report.
nlohmann::json Play(const PlayRequest& req);

}  // namespace openclaw::channels::discord_voice
