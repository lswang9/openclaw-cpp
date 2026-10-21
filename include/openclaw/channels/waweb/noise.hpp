#pragma once
// WhatsApp Web (Baileys-equivalent) building blocks — see docs/whatsapp-web-gap.md.
// Implemented here: Noise_XX_25519_AESGCM_SHA256 handshake state (WA prologue "WA" 6 3), transport framing
// (3-byte BE length, header on first frame), minimal protobuf HandshakeMessage codec, media encryption
// (HKDF "WhatsApp <Type> Keys", AES-256-CBC + HMAC-SHA256/10), and the pairing-QR payload.
// NOT implemented: live socket login, WABinary token dictionary, Signal (X3DH/Double Ratchet/sender keys),
// app-state sync, history sync.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::channels::waweb {

using Bytes = std::vector<std::uint8_t>;

struct KeyPair { Bytes pub, priv; };  // X25519, 32-byte raw keys
KeyPair GenerateKeyPair();
Bytes SharedKey(const Bytes& priv, const Bytes& peer_pub);
Bytes Sha256(const Bytes& data);
Bytes HmacSha256(const Bytes& key, const Bytes& data);
Bytes Hkdf(const Bytes& ikm, std::size_t len, const Bytes& salt = {}, const std::string& info = "");
Bytes RandomBytes(std::size_t n);
std::string Base64(const Bytes& b);
Bytes FromString(const std::string& s);

inline const Bytes kWaHeader = {'W', 'A', 6, 3};
inline constexpr const char* kNoiseMode = "Noise_XX_25519_AESGCM_SHA256\0\0\0\0";

// Symmetric + transport state of the WA Noise handshake (same for both roles).
class NoiseState {
public:
    explicit NoiseState(const Bytes& prologue = kWaHeader);
    void Authenticate(const Bytes& data);         // MixHash
    void MixIntoKey(const Bytes& ikm);            // MixKey (HKDF → salt|key, counters reset)
    Bytes Encrypt(const Bytes& plaintext);        // AES-256-GCM, iv = 0^8 || BE32(counter), AAD = h while handshaking
    std::optional<Bytes> Decrypt(const Bytes& ciphertext);
    void FinishInit();                            // Split: enc/dec keys; AAD dropped; counters reset
    bool finished() const { return finished_; }
    // Transport framing: 3-byte BE length (+ WA header before the very first outbound frame).
    Bytes EncodeFrame(const Bytes& payload);
    // Feed inbound bytes; returns complete (decrypted after FinishInit) frames.
    std::vector<Bytes> DecodeFrames(const Bytes& chunk);
    bool initiator = true;

private:
    Bytes hash_, salt_, enc_key_, dec_key_;
    std::uint32_t read_counter_ = 0, write_counter_ = 0;
    bool finished_ = false, sent_intro_ = false;
    Bytes inbuf_;
};

// Minimal protobuf for WAProto HandshakeMessage {clientHello=2, serverHello=3, clientFinish=4} each
// {ephemeral=1, static=2, payload=3}.
struct HelloParts { Bytes ephemeral, static_, payload; };
Bytes EncodeHandshake(int which /*2|3|4*/, const HelloParts& p);
std::optional<std::pair<int, HelloParts>> DecodeHandshake(const Bytes& b);

// Full in-process XX handshake helpers (client = Baileys side; server = test responder / WA server role).
struct ClientHandshake {
    NoiseState noise;
    KeyPair ephemeral, static_;
    Bytes ClientHello();                                                   // → frame payload
    // Process ServerHello; returns ClientFinish payload (send it with EncodeFrame, THEN noise.FinishInit()) or nullopt (bad MAC). server_cert_out = decrypted cert payload.
    std::optional<Bytes> OnServerHello(const Bytes& msg, const Bytes& client_payload, Bytes* server_static_out, Bytes* server_cert_out);
};
struct ServerHandshake {
    ServerHandshake() { noise.initiator = false; }
    NoiseState noise;
    KeyPair ephemeral, static_;
    Bytes client_ephemeral;
    std::optional<Bytes> OnClientHello(const Bytes& msg, const Bytes& cert_payload);  // → ServerHello
    std::optional<Bytes> OnClientFinish(const Bytes& msg, Bytes* client_static_out);   // → decrypted ClientPayload
};

// Media crypto (upload/download of encrypted attachments).
enum class MediaType { Image, Video, Audio, Document, Sticker };
struct MediaKeys { Bytes iv, cipher_key, mac_key, ref_key; };
MediaKeys ExpandMediaKey(const Bytes& media_key, MediaType t);
Bytes EncryptMedia(const Bytes& plain, const Bytes& media_key, MediaType t);           // enc || mac10
std::optional<Bytes> DecryptMedia(const Bytes& file, const Bytes& media_key, MediaType t);

// Pairing QR content: "<ref>,<b64 noisePub>,<b64 identityPub>,<b64 advSecret>".
std::string QrPayload(const std::string& ref, const Bytes& noise_pub, const Bytes& identity_pub, const Bytes& adv_secret);

// ---- Offline stubs / codecs toward a Baileys-equivalent client (no live socket) ----

// ServerHello payload is a CertChain protobuf: {leaf=1, intermediate=2} of NoiseCertificate{details=1, signature=2}.
struct CertDetails {
    std::uint32_t serial = 0, issuer_serial = 0;
    Bytes key;
    std::uint64_t not_before = 0, not_after = 0;
};
struct CertChainInfo {
    Bytes leaf_details;
    Bytes leaf_signature;
    Bytes intermediate_details;
    Bytes intermediate_signature;
    bool structurally_ok = false;
    bool signature_checked = false;
    bool signature_valid = false;
    std::string error;
    CertDetails intermediate, leaf;
};
std::optional<CertChainInfo> DecodeCertChain(const Bytes& payload);
std::optional<CertDetails> DecodeCertDetails(const Bytes& raw);
const Bytes& WaRootPublicKey();  // Curve25519 (Montgomery) WA noise root, issuer serial 0
// Signal-style XEdDSA verify with a Montgomery public key (libsignal curve25519_verify semantics).
bool VerifyXEdDsa(const Bytes& mont_pub, const Bytes& msg, const Bytes& sig64);
// whatsmeow verifyServerCert equivalent: intermediate signed by root (XEdDSA), issuerSerial 0, leaf signed by the
// intermediate key, leaf issuerSerial == intermediate serial, optional leaf key == decrypted server static, optional
// validity window at now_s (unix seconds; 0 = skip). Empty root → WaRootPublicKey().
CertChainInfo VerifyCertChain(const Bytes& payload, const Bytes& wa_root_pub = {}, const Bytes& server_static = {},
                              std::uint64_t now_s = 0);

// WABinary codec, wire-compatible with Baileys encodeBinaryNode/decodeDecompressedBinaryNode (token dictionary v3,
// nibble/hex packing, JID_PAIR/AD_JID/FB_JID/INTEROP_JID). with_frame_flag adds/strips the leading flags byte;
// zlib-compressed frames (flag & 2) are rejected (no zlib in the build).
struct BinaryNode {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    Bytes content;                          // raw bytes (or UTF-8 when content_is_string)
    std::vector<BinaryNode> children;       // when content is a list
    bool has_content = false;
    bool content_is_string = false;
};
Bytes EncodeBinaryNode(const BinaryNode& n, bool with_frame_flag = true);
std::optional<BinaryNode> DecodeBinaryNode(const Bytes& b, bool with_frame_flag = true, std::string* error = nullptr);

// Signal protocol session stubs (X3DH / Double Ratchet). No cryptographic ratchet yet — stores identity
// and records that a session was "established" for offline wiring tests.
struct SignalIdentity {
    KeyPair identity;          // X25519 identity keypair (IK)
    KeyPair signed_prekey;     // X25519 SPK
    Bytes spk_signature;       // placeholder (Ed25519(IK, SPK.pub) — empty until Ed25519 wired)
    std::uint32_t registration_id = 0;
};
SignalIdentity GenerateSignalIdentity();
struct SignalSession {
    Bytes peer_identity;
    Bytes root_key;
    Bytes send_chain;
    Bytes recv_chain;
    std::uint32_t send_n = 0, recv_n = 0;
    bool established = false;
};
// Stub X3DH: HKDF(IK||EK||SPK material) → root; not interoperable with WhatsApp.
SignalSession SignalX3dhInitiate(const SignalIdentity& self, const Bytes& peer_ik, const Bytes& peer_spk,
                                 const Bytes& peer_opk = {});
// Stub ratchet step: HKDF(chain || msg) → new chain + 32-byte message key. Not the real Double Ratchet.
Bytes SignalRatchetEncrypt(SignalSession& s, const Bytes& plaintext);
std::optional<Bytes> SignalRatchetDecrypt(SignalSession& s, const Bytes& ciphertext);

}  // namespace openclaw::channels::waweb
