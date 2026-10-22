#pragma once
// 1.165: WhatsApp app-state sync primitives (Baileys/whatsmeow-shaped): LTHash, mutation key expansion,
// app-state-sync-key + collection version persistence, and offline SET mute mutation encode/decode.
#include "openclaw/channels/waweb/noise.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::channels::waweb {

// 128-byte LTHash: per input HKDF(item, salt="", info="WhatsApp Patch Integrity", 128) then pointwise
// uint16 LE add/sub with wraparound. SubtractThenAdd matches whatsmeow WAPatchIntegrity.
Bytes LtHashSubtractThenAdd(const Bytes& base /*128 or empty→zeros*/, const std::vector<Bytes>& subtract,
                            const std::vector<Bytes>& add);

struct MutationKeys {
    Bytes index_key, value_encryption, value_mac, snapshot_mac, patch_mac;  // 32 each
};
MutationKeys ExpandMutationKeys(const Bytes& key_data /*32*/);

struct AppStateSyncKey {
    std::string id_b64;  // key id as base64
    Bytes key_data;      // 32
    std::uint64_t timestamp_ms = 0;
};

struct LtHashState {
    std::uint64_t version = 0;
    Bytes hash;  // 128
    std::map<std::string, Bytes> index_value_map;  // indexMac b64 → valueMac
};

bool SaveAppStateKey(const std::filesystem::path& dir, const AppStateSyncKey& k);
std::optional<AppStateSyncKey> LoadAppStateKey(const std::filesystem::path& dir, const std::string& id_b64);
bool SaveAppStateVersion(const std::filesystem::path& dir, const std::string& collection, const LtHashState& s);
std::optional<LtHashState> LoadAppStateVersion(const std::filesystem::path& dir, const std::string& collection);

enum class SyncOp : std::uint8_t { Set = 0x01, Remove = 0x02 };

struct SyncMutation {
    SyncOp op = SyncOp::Set;
    std::vector<std::string> index;  // e.g. {"mute","jid@s.whatsapp.net"}
    std::string action_type;         // "mute" | "pin" | "archive" | "star"
    bool bool_value = false;
    std::int64_t mute_end_ms = 0;
};

struct EncodedMutation {
    Bytes index_mac, value_blob /* enc || valueMac */, value_mac;
};
// AES-256-CBC with zero IV (offline self-consistent; WA may use a different IV scheme on the wire).
EncodedMutation EncodeMutation(const SyncMutation& m, const MutationKeys& keys, const Bytes& key_id);
std::optional<SyncMutation> DecodeMutation(const EncodedMutation& e, const MutationKeys& keys, const Bytes& key_id,
                                           SyncOp op, std::string* err = nullptr);

// Apply SET/REMOVE to LTHash state (mixes valueMacs). Returns false on REMOVE of unknown index.
bool ApplyMutation(LtHashState& state, const EncodedMutation& e, SyncOp op);
Bytes SnapshotMac(const LtHashState& state, const std::string& collection, const MutationKeys& keys);

}  // namespace openclaw::channels::waweb
