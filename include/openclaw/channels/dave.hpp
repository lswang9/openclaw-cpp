#pragma once
// Discord DAVE (audio/video E2EE) client session on top of Discord's libdave (C API) + Cisco mlspp.
// Voice-gateway DAVE opcodes handled here: 21 prepare_transition, 22 execute_transition, 23 transition_ready (out),
// 24 prepare_epoch, 25 mls_external_sender (bin), 26 mls_key_package (bin, out), 27 mls_proposals (bin),
// 28 mls_commit_welcome (bin, out), 29 mls_announce_commit_transition (bin), 30 mls_welcome (bin),
// 31 mls_invalid_commit_welcome (out); 11/13 clients connect/disconnect maintain the recognized roster.
// Built only when libdave is available (scripts/build-dave.sh → OPENCLAW_HAVE_DAVE); otherwise Available()=false.
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace openclaw::channels::dave {

using Bytes = std::vector<std::uint8_t>;

bool Available();
std::uint16_t MaxProtocolVersion();  // 0 when unavailable

struct Outgoing {
    bool binary = false;
    int op = 0;
    Bytes payload;         // binary: sent as [op u8][payload]
    nlohmann::json json;   // text: full {"op":..,"d":..}
};

class Session {
public:
    Session(std::string self_user_id, std::uint64_t channel_id);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // op 4 session description / op 24 prepare_epoch(epoch=1): (re)initialise; returns the key package to send.
    std::vector<Outgoing> Start(std::uint16_t protocol_version);
    std::vector<Outgoing> OnJson(int op, const nlohmann::json& d);
    std::vector<Outgoing> OnBinary(int op, const Bytes& payload);  // payload after the seq+opcode header

    std::uint16_t ProtocolVersion() const { return version_; }
    bool Ready() const;  // encryptor has a key ratchet (or protocol 0 → passthrough)
    // Media: Opus frame ↔ DAVE frame. Passthrough when protocol version is 0.
    bool Encrypt(std::uint32_t ssrc, const Bytes& frame, Bytes* out);
    bool Decrypt(const std::string& user_id, const Bytes& frame, Bytes* out);
    void UserConnected(const std::string& id) { roster_.insert(id); }
    void UserDisconnected(const std::string& id) { roster_.erase(id); }
    nlohmann::json Stats() const;
    std::string LastError() const { return last_error_; }

private:
    std::vector<Outgoing> Execute(int transition_id);
    void ApplyRatchets();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string self_;
    std::uint64_t group_ = 0;
    std::uint16_t version_ = 0, pending_version_ = 0;
    std::set<std::string> roster_;
    std::string last_error_;
    Bytes external_sender_;
    std::uint64_t encrypted_ = 0, decrypted_ = 0, decrypt_failed_ = 0;
    int pending_transition_ = -1;
};

}  // namespace openclaw::channels::dave
