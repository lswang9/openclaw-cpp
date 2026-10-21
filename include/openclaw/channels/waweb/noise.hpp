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

// ServerHello payload is a CertChain protobuf. We decode the outer shape and verify the leaf
// signature against a caller-supplied root when provided; without a root we only structural-check.
struct CertChainInfo {
    Bytes leaf_details;     // raw NoiseCertificate.details
    Bytes leaf_signature;
    Bytes intermediate_details;
    Bytes intermediate_signature;
    bool structurally_ok = false;
    bool signature_checked = false;
    bool signature_valid = false;
};
// Decode CertChain {leaf=1:{details=1,signature=2}, intermediate=2:{...}}. Empty optional on hard fail.
std::optional<CertChainInfo> DecodeCertChain(const Bytes& payload);
// Verify leaf_signature = Ed25519(intermediate_pub, leaf_details) when intermediate_details encodes a key;
// if `wa_root_pub` (32 bytes) is set, also check intermediate against the WA root. Stub: returns structural
// result and sets signature_checked=false when keys cannot be extracted (full X.509/Ed25519 path missing).
CertChainInfo VerifyCertChain(const Bytes& payload, const Bytes& wa_root_pub = {});

// Minimal WABinary frame codec (token dictionary NOT included — tags are numeric / raw strings only).
// Documented wire form: [list size][tag][attrs...][content]. Used for offline unit tests of the shape.
struct BinaryNode {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    Bytes content;                          // raw bytes content
    std::vector<BinaryNode> children;       // when content is a list
};
Bytes EncodeBinaryNode(const BinaryNode& n);
std::optional<BinaryNode> DecodeBinaryNode(const Bytes& b);

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
