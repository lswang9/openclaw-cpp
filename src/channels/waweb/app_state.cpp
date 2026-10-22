#include "openclaw/channels/waweb/app_state.hpp"

#include "openclaw/persist/state_db.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace openclaw::channels::waweb {
namespace {
using json = nlohmann::json;
std::string Hex(const Bytes& b) {
    static const char* x = "0123456789abcdef"; std::string o;
    for (auto c : b) { o += x[c >> 4]; o += x[c & 15]; } return o;
}
Bytes Unhex(const std::string& s) {
    Bytes b; for (std::size_t i = 0; i + 1 < s.size(); i += 2) b.push_back(static_cast<std::uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16))); return b;
}
void Pointwise(Bytes& base, const Bytes& input, bool subtract) {
    for (std::size_t i = 0; i + 1 < base.size() && i + 1 < input.size(); i += 2) {
        std::uint16_t x = static_cast<std::uint16_t>(base[i] | (base[i + 1] << 8));
        std::uint16_t y = static_cast<std::uint16_t>(input[i] | (input[i + 1] << 8));
        std::uint16_t r = subtract ? static_cast<std::uint16_t>(x - y) : static_cast<std::uint16_t>(x + y);
        base[i] = static_cast<std::uint8_t>(r & 0xff); base[i + 1] = static_cast<std::uint8_t>((r >> 8) & 0xff);
    }
}
Bytes ExpandIntegrity(const Bytes& item) { return Hkdf(item, 128, {}, "WhatsApp Patch Integrity"); }
void Var(Bytes& o, std::uint64_t v) { while (v >= 0x80) { o.push_back(static_cast<std::uint8_t>(v | 0x80)); v >>= 7; } o.push_back(static_cast<std::uint8_t>(v)); }
void FB(Bytes& o, int n, const Bytes& v) { Var(o, (std::uint64_t(n) << 3) | 2); Var(o, v.size()); o.insert(o.end(), v.begin(), v.end()); }
void FV(Bytes& o, int n, std::uint64_t v) { Var(o, std::uint64_t(n) << 3); Var(o, v); }
void FS(Bytes& o, int n, const std::string& s) { FB(o, n, Bytes(s.begin(), s.end())); }
Bytes AesCbc(bool enc, const Bytes& key, const Bytes& iv, const Bytes& in, bool& ok) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    Bytes out(in.size() + 32); int l = 0, f = 0;
    ok = EVP_CipherInit_ex(c, EVP_aes_256_cbc(), nullptr, key.data(), iv.data(), enc ? 1 : 0) == 1 &&
         EVP_CipherUpdate(c, out.data(), &l, in.data(), static_cast<int>(in.size())) == 1 &&
         EVP_CipherFinal_ex(c, out.data() + l, &f) == 1;
    EVP_CIPHER_CTX_free(c); out.resize(ok ? static_cast<std::size_t>(l + f) : 0); return out;
}
Bytes GenerateMac(SyncOp op, const Bytes& data, const Bytes& key_id, const Bytes& mac_key) {
    Bytes msg; msg.push_back(static_cast<std::uint8_t>(op));
    msg.insert(msg.end(), data.begin(), data.end());
    msg.insert(msg.end(), key_id.begin(), key_id.end());
    msg.push_back(static_cast<std::uint8_t>(key_id.size() & 0xff));
    auto m = HmacSha256(mac_key, msg); m.resize(32); return m;
}
Bytes EncodeSyncActionData(const SyncMutation& m) {
    // SyncActionData { index=1 bytes(JSON), value=2 SyncActionValue, padding=3, version=4 }
    json idx = m.index; std::string idx_s = idx.dump();
    Bytes value;
    if (m.action_type == "mute") {
        Bytes mute; FV(mute, 1, static_cast<std::uint64_t>(m.mute_end_ms));  // SyncActionValue.muteAction.muteEndTimestampMs approx as field 1 of muteAction nested
        // SyncActionValue { muteAction = 5 } — field 5 message
        Bytes ma; FV(ma, 1, m.bool_value ? 1 : 0); FV(ma, 2, static_cast<std::uint64_t>(m.mute_end_ms));
        FB(value, 5, ma);
    } else if (m.action_type == "pin") {
        Bytes pa; FV(pa, 1, m.bool_value ? 1 : 0); FB(value, 3, pa);  // pinAction = 3
    } else if (m.action_type == "archive") {
        Bytes aa; FV(aa, 1, m.bool_value ? 1 : 0); FB(value, 1, aa);  // archive = 1
    } else if (m.action_type == "star") {
        Bytes sa; FV(sa, 1, m.bool_value ? 1 : 0); FB(value, 8, sa);
    }
    Bytes data; FB(data, 1, Bytes(idx_s.begin(), idx_s.end())); FB(data, 2, value); FB(data, 3, {}); FV(data, 4, 2);
    return data;
}
std::optional<SyncMutation> DecodeSyncActionData(const Bytes& raw) {
    // minimal parse
    std::size_t i = 0;
    auto var = [&](std::uint64_t& v) { v = 0; for (int sh = 0; i < raw.size() && sh < 64; sh += 7) { auto c = raw[i++]; v |= std::uint64_t(c & 0x7f) << sh; if (!(c & 0x80)) return true; } return false; };
    Bytes index_b, value_b;
    while (i < raw.size()) {
        std::uint64_t k; if (!var(k)) return std::nullopt;
        int f = static_cast<int>(k >> 3), wt = static_cast<int>(k & 7);
        if (wt == 0) { std::uint64_t v; if (!var(v)) return std::nullopt; }
        else if (wt == 2) {
            std::uint64_t n; if (!var(n) || i + n > raw.size()) return std::nullopt;
            Bytes v(raw.begin() + i, raw.begin() + i + n); i += n;
            if (f == 1) index_b = v; else if (f == 2) value_b = v;
        } else return std::nullopt;
    }
    SyncMutation m;
    try {
        auto j = json::parse(std::string(index_b.begin(), index_b.end()));
        if (j.is_array()) for (auto& x : j) m.index.push_back(x.get<std::string>());
    } catch (...) { return std::nullopt; }
    // peek value for action type
    i = 0;
    auto var2 = [&](std::uint64_t& v) { v = 0; for (int sh = 0; i < value_b.size() && sh < 64; sh += 7) { auto c = value_b[i++]; v |= std::uint64_t(c & 0x7f) << sh; if (!(c & 0x80)) return true; } return false; };
    while (i < value_b.size()) {
        std::uint64_t k; if (!var2(k)) break;
        int f = static_cast<int>(k >> 3), wt = static_cast<int>(k & 7);
        if (wt == 0) { std::uint64_t v; if (!var2(v)) break; }
        else if (wt == 2) {
            std::uint64_t n; if (!var2(n) || i + n > value_b.size()) break;
            Bytes inner(value_b.begin() + i, value_b.begin() + i + n); i += n;
            auto read_bool_ts = [&](const Bytes& in, SyncMutation& out) {
                std::size_t j = 0;
                auto vv = [&](std::uint64_t& v) { v = 0; for (int sh = 0; j < in.size() && sh < 64; sh += 7) { auto c = in[j++]; v |= std::uint64_t(c & 0x7f) << sh; if (!(c & 0x80)) return true; } return false; };
                while (j < in.size()) {
                    std::uint64_t kk; if (!vv(kk)) break;
                    int ff = static_cast<int>(kk >> 3), w = static_cast<int>(kk & 7);
                    if (w == 0) { std::uint64_t v; if (!vv(v)) break; if (ff == 1) out.bool_value = v != 0; if (ff == 2) out.mute_end_ms = static_cast<std::int64_t>(v); }
                    else if (w == 2) { std::uint64_t nn; if (!vv(nn) || j + nn > in.size()) break; j += nn; }
                    else break;
                }
            };
            if (f == 5) { m.action_type = "mute"; read_bool_ts(inner, m); }
            else if (f == 3) { m.action_type = "pin"; read_bool_ts(inner, m); }
            else if (f == 1) { m.action_type = "archive"; read_bool_ts(inner, m); }
            else if (f == 8) { m.action_type = "star"; read_bool_ts(inner, m); }
        } else break;
    }
    return m;
}
std::filesystem::path KeysPath(const std::filesystem::path& dir) { return dir / "waweb-keys.json"; }
}  // namespace

Bytes LtHashSubtractThenAdd(const Bytes& base, const std::vector<Bytes>& subtract, const std::vector<Bytes>& add) {
    Bytes out = base.empty() ? Bytes(128, 0) : base;
    if (out.size() != 128) out.assign(128, 0);
    for (auto& s : subtract) Pointwise(out, ExpandIntegrity(s), true);
    for (auto& a : add) Pointwise(out, ExpandIntegrity(a), false);
    return out;
}

MutationKeys ExpandMutationKeys(const Bytes& key_data) {
    auto e = Hkdf(key_data, 160, {}, "WhatsApp Mutation Keys");
    MutationKeys k;
    k.index_key.assign(e.begin(), e.begin() + 32);
    k.value_encryption.assign(e.begin() + 32, e.begin() + 64);
    k.value_mac.assign(e.begin() + 64, e.begin() + 96);
    k.snapshot_mac.assign(e.begin() + 96, e.begin() + 128);
    k.patch_mac.assign(e.begin() + 128, e.begin() + 160);
    return k;
}

bool SaveAppStateKey(const std::filesystem::path& dir, const AppStateSyncKey& k) {
    auto keys = json::parse(persist::StateRead(KeysPath(dir)).value_or("{}"), nullptr, false);
    if (!keys.is_object()) keys = json::object();
    keys["app-state-sync-key"][k.id_b64] = {{"keyData", Hex(k.key_data)}, {"timestamp", k.timestamp_ms}};
    return persist::StateWrite(KeysPath(dir), keys.dump(2));
}
std::optional<AppStateSyncKey> LoadAppStateKey(const std::filesystem::path& dir, const std::string& id_b64) {
    auto keys = json::parse(persist::StateRead(KeysPath(dir)).value_or("{}"), nullptr, false);
    if (!keys.is_object() || !keys.contains("app-state-sync-key") || !keys["app-state-sync-key"].contains(id_b64)) return std::nullopt;
    const auto& j = keys["app-state-sync-key"][id_b64];
    return AppStateSyncKey{id_b64, Unhex(j.value("keyData", "")), j.value("timestamp", 0ULL)};
}
bool SaveAppStateVersion(const std::filesystem::path& dir, const std::string& collection, const LtHashState& s) {
    auto keys = json::parse(persist::StateRead(KeysPath(dir)).value_or("{}"), nullptr, false);
    if (!keys.is_object()) keys = json::object();
    json map = json::object();
    for (auto& [k, v] : s.index_value_map) map[k] = Hex(v);
    keys["app-state-sync-version"][collection] = {{"version", s.version}, {"hash", Hex(s.hash)}, {"indexValueMap", map}};
    return persist::StateWrite(KeysPath(dir), keys.dump(2));
}
std::optional<LtHashState> LoadAppStateVersion(const std::filesystem::path& dir, const std::string& collection) {
    auto keys = json::parse(persist::StateRead(KeysPath(dir)).value_or("{}"), nullptr, false);
    if (!keys.is_object() || !keys.contains("app-state-sync-version") || !keys["app-state-sync-version"].contains(collection)) return std::nullopt;
    const auto& j = keys["app-state-sync-version"][collection];
    LtHashState s; s.version = j.value("version", 0ULL); s.hash = Unhex(j.value("hash", ""));
    for (auto& [k, v] : j.value("indexValueMap", json::object()).items()) s.index_value_map[k] = Unhex(v.get<std::string>());
    return s;
}

EncodedMutation EncodeMutation(const SyncMutation& m, const MutationKeys& keys, const Bytes& key_id) {
    auto data = EncodeSyncActionData(m);
    Bytes iv(16, 0); bool ok; auto enc = AesCbc(true, keys.value_encryption, iv, data, ok);
    auto value_mac = GenerateMac(m.op, enc, key_id, keys.value_mac);
    json idx = m.index; auto index_s = idx.dump();
    auto index_mac = HmacSha256(keys.index_key, Bytes(index_s.begin(), index_s.end()));
    EncodedMutation e; e.index_mac = index_mac; e.value_mac = value_mac;
    e.value_blob = enc; e.value_blob.insert(e.value_blob.end(), value_mac.begin(), value_mac.end());
    return e;
}

std::optional<SyncMutation> DecodeMutation(const EncodedMutation& e, const MutationKeys& keys, const Bytes& key_id,
                                           SyncOp op, std::string* err) {
    if (e.value_blob.size() < 32) { if (err) *err = "short blob"; return std::nullopt; }
    Bytes enc(e.value_blob.begin(), e.value_blob.end() - 32), mac(e.value_blob.end() - 32, e.value_blob.end());
    auto want = GenerateMac(op, enc, key_id, keys.value_mac);
    if (want != mac) { if (err) *err = "value mac mismatch"; return std::nullopt; }
    Bytes iv(16, 0); bool ok; auto pt = AesCbc(false, keys.value_encryption, iv, enc, ok);
    if (!ok) { if (err) *err = "aes failed"; return std::nullopt; }
    auto m = DecodeSyncActionData(pt);
    if (!m) { if (err) *err = "bad SyncActionData"; return std::nullopt; }
    m->op = op;
    return m;
}

bool ApplyMutation(LtHashState& state, const EncodedMutation& e, SyncOp op) {
    if (state.hash.size() != 128) state.hash.assign(128, 0);
    std::string idx = Base64(e.index_mac);
    std::vector<Bytes> sub, add;
    if (op == SyncOp::Set) {
        if (auto it = state.index_value_map.find(idx); it != state.index_value_map.end()) sub.push_back(it->second);
        add.push_back(e.value_mac);
        state.index_value_map[idx] = e.value_mac;
    } else {
        auto it = state.index_value_map.find(idx);
        if (it == state.index_value_map.end()) return false;
        sub.push_back(it->second);
        state.index_value_map.erase(it);
    }
    state.hash = LtHashSubtractThenAdd(state.hash, sub, add);
    ++state.version;
    return true;
}

Bytes SnapshotMac(const LtHashState& state, const std::string& collection, const MutationKeys& keys) {
    Bytes msg = state.hash;
    // version as 8-byte BE + collection name
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>((state.version >> (8 * i)) & 0xff));
    msg.insert(msg.end(), collection.begin(), collection.end());
    return HmacSha256(keys.snapshot_mac, msg);
}

}  // namespace openclaw::channels::waweb
