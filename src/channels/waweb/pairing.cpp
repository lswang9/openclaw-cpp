#include <cctype>
#include "openclaw/channels/waweb/pairing.hpp"

#include <openssl/evp.h>
#include <openssl/md5.h>

#include <cstring>

namespace openclaw::channels::waweb {
namespace {
Bytes Cat(std::initializer_list<Bytes> parts) {
    Bytes o; for (auto& p : parts) o.insert(o.end(), p.begin(), p.end()); return o;
}
void PutVarint(Bytes& o, std::uint64_t v) {
    while (v >= 0x80) { o.push_back(static_cast<std::uint8_t>(v | 0x80)); v >>= 7; }
    o.push_back(static_cast<std::uint8_t>(v));
}
void PutTag(Bytes& o, int field, int wt) { PutVarint(o, (static_cast<std::uint64_t>(field) << 3) | wt); }
void PutBytes(Bytes& o, int field, const Bytes& b) { PutTag(o, field, 2); PutVarint(o, b.size()); o.insert(o.end(), b.begin(), b.end()); }
void PutString(Bytes& o, int field, const std::string& s) { PutBytes(o, field, Bytes(s.begin(), s.end())); }
void PutU32(Bytes& o, int field, std::uint32_t v) { PutTag(o, field, 0); PutVarint(o, v); }
void PutU64(Bytes& o, int field, std::uint64_t v) { PutTag(o, field, 0); PutVarint(o, v); }
void PutBool(Bytes& o, int field, bool v) { if (v) PutU32(o, field, 1); }
void PutMsg(Bytes& o, int field, const Bytes& inner) { PutBytes(o, field, inner); }
Bytes Be(std::uint64_t v, int n) { Bytes b; for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); return b; }

bool ReadVarint(const Bytes& b, std::size_t& i, std::uint64_t& v) {
    v = 0;
    for (int sh = 0; i < b.size() && sh < 64; sh += 7) {
        auto c = b[i++]; v |= std::uint64_t(c & 0x7F) << sh;
        if (!(c & 0x80)) return true;
    }
    return false;
}
bool Fields(const Bytes& b, std::vector<std::pair<int, Bytes>>& out) {
    std::size_t i = 0;
    while (i < b.size()) {
        std::uint64_t key; if (!ReadVarint(b, i, key)) return false;
        int field = static_cast<int>(key >> 3), wt = static_cast<int>(key & 7);
        if (wt == 0) { std::uint64_t v; if (!ReadVarint(b, i, v)) return false; Bytes raw; PutVarint(raw, v); out.emplace_back(field, raw); }
        else if (wt == 2) {
            std::uint64_t n; if (!ReadVarint(b, i, n) || i + n > b.size()) return false;
            out.emplace_back(field, Bytes(b.begin() + static_cast<std::ptrdiff_t>(i), b.begin() + static_cast<std::ptrdiff_t>(i + n)));
            i += n;
        } else if (wt == 5) { if (i + 4 > b.size()) return false; out.emplace_back(field, Bytes(b.begin() + i, b.begin() + i + 4)); i += 4; }
        else if (wt == 1) { if (i + 8 > b.size()) return false; out.emplace_back(field, Bytes(b.begin() + i, b.begin() + i + 8)); i += 8; }
        else return false;
    }
    return true;
}
std::uint64_t AsVarint(const Bytes& b) {
    std::size_t i = 0; std::uint64_t v = 0; ReadVarint(b, i, v); return v;
}
Bytes ChildContent(const BinaryNode& n, const std::string& tag) {
    for (auto& c : n.children) if (c.tag == tag) return c.content;
    return {};
}
BinaryNode* Child(BinaryNode& n, const std::string& tag) {
    for (auto& c : n.children) if (c.tag == tag) return &c;
    return nullptr;
}
const BinaryNode* Child(const BinaryNode& n, const std::string& tag) {
    for (auto& c : n.children) if (c.tag == tag) return &c;
    return nullptr;
}
std::string Attr(const BinaryNode& n, const std::string& k) {
    for (auto& [a, v] : n.attrs) if (a == k) return v;
    return {};
}
Bytes Md5(const std::string& s) {
    Bytes o(16); EVP_Digest(s.data(), s.size(), o.data(), nullptr, EVP_md5(), nullptr); return o;
}
Bytes EncodeAdvDeviceIdentity(std::uint32_t raw_id, std::uint64_t ts, std::uint32_t key_index, bool hosted) {
    Bytes o; PutU32(o, 1, raw_id); PutU64(o, 2, ts); PutU32(o, 3, key_index);
    if (hosted) { PutU32(o, 4, 1); PutU32(o, 5, 1); }
    return o;
}
Bytes EncodeAdvSignedDeviceIdentity(const AdvAccount& a, bool include_key) {
    Bytes o; PutBytes(o, 1, a.details);
    if (include_key && !a.account_signature_key.empty()) PutBytes(o, 2, a.account_signature_key);
    PutBytes(o, 3, a.account_signature);
    if (!a.device_signature.empty()) PutBytes(o, 4, a.device_signature);
    return o;
}
Bytes EncodeAdvSignedDeviceIdentityHmac(const Bytes& details, const Bytes& hmac, bool hosted) {
    Bytes o; PutBytes(o, 1, details); PutBytes(o, 2, hmac); if (hosted) PutU32(o, 3, 1); return o;
}
std::optional<AdvAccount> DecodeAdvSignedDeviceIdentity(const Bytes& raw) {
    std::vector<std::pair<int, Bytes>> f; if (!Fields(raw, f)) return std::nullopt;
    AdvAccount a;
    for (auto& [n, v] : f) {
        if (n == 1) a.details = v; else if (n == 2) a.account_signature_key = v;
        else if (n == 3) a.account_signature = v; else if (n == 4) a.device_signature = v;
    }
    if (a.details.empty() || a.account_signature.empty()) return std::nullopt;
    std::vector<std::pair<int, Bytes>> d; if (!Fields(a.details, d)) return std::nullopt;
    for (auto& [n, v] : d) {
        if (n == 3) a.key_index = static_cast<std::uint32_t>(AsVarint(v));
        if (n == 5 && AsVarint(v) == 1) a.hosted = true;
    }
    return a;
}

Bytes EncodeUserAgent(const ClientPayloadOpts& o) {
    Bytes ver; PutU32(ver, 1, static_cast<std::uint32_t>(o.version[0])); PutU32(ver, 2, static_cast<std::uint32_t>(o.version[1])); PutU32(ver, 3, static_cast<std::uint32_t>(o.version[2]));
    Bytes ua; PutU32(ua, 1, 14);  // WEB
    PutMsg(ua, 2, ver);
    PutString(ua, 3, "000"); PutString(ua, 4, "000");
    PutString(ua, 5, "0.1"); PutString(ua, 7, "Desktop"); PutString(ua, 8, "0.1");
    PutU32(ua, 10, 0);  // RELEASE
    PutString(ua, 11, "en"); PutString(ua, 12, o.country);
    return ua;
}
Bytes EncodeWebInfo(const ClientPayloadOpts& o) {
    Bytes wi; PutU32(wi, 4, 0);  // WEB_BROWSER; Darwin/WinHybrid only when syncFullHistory + Desktop
    (void)o; return wi;
}
Bytes EncodeDeviceProps(const ClientPayloadOpts& o) {
    Bytes ver; PutU32(ver, 1, 10); PutU32(ver, 2, 15); PutU32(ver, 3, 7);
    Bytes hist; PutU32(hist, 3, 10240); PutBool(hist, 4, true); PutBool(hist, 7, true); PutBool(hist, 8, true);
    PutBool(hist, 9, true); PutBool(hist, 10, true); PutBool(hist, 11, true); PutBool(hist, 12, true); PutBool(hist, 14, true);
    int platform = 1;  // CHROME
    std::string bn = o.browser_name; for (auto& c : bn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (bn.find("firefox") != std::string::npos) platform = 2;
    else if (bn.find("safari") != std::string::npos) platform = 5;
    else if (bn.find("edge") != std::string::npos) platform = 6;
    else if (bn.find("desktop") != std::string::npos) platform = 7;
    Bytes dp; PutString(dp, 1, o.browser_os); PutMsg(dp, 2, ver); PutU32(dp, 3, static_cast<std::uint32_t>(platform));
    if (o.sync_full_history) PutBool(dp, 4, true);
    PutMsg(dp, 5, hist);
    return dp;
}
}  // namespace

Bytes EncodeClientPayloadLogin(std::uint64_t username, std::uint32_t device, const ClientPayloadOpts& o) {
    Bytes p;
    PutU64(p, 1, username);
    PutBool(p, 3, true);  // passive
    PutMsg(p, 5, EncodeUserAgent(o));
    PutMsg(p, 6, EncodeWebInfo(o));
    if (!o.push_name.empty()) PutString(p, 7, o.push_name);
    PutU32(p, 12, 1);  // WIFI_UNKNOWN
    PutU32(p, 13, 1);  // USER_ACTIVATED
    PutU32(p, 18, device);
    PutBool(p, 33, true);  // pull
    PutBool(p, 41, false); // lidDbMigrated
    return p;
}

Bytes EncodeClientPayloadRegistration(const AuthCreds& c, const ClientPayloadOpts& o) {
    Bytes pair;
    PutBytes(pair, 1, Be(c.signal.registration_id, 4));
    PutBytes(pair, 2, Bytes{0x05});
    PutBytes(pair, 3, c.signal.identity.pub);
    PutBytes(pair, 4, Be(c.signal.signed_prekey_id, 3));
    PutBytes(pair, 5, c.signal.signed_prekey.pub);
    PutBytes(pair, 6, c.signed_prekey_sig);
    std::string ver = std::to_string(o.version[0]) + "." + std::to_string(o.version[1]) + "." + std::to_string(o.version[2]);
    PutBytes(pair, 7, Md5(ver));
    PutBytes(pair, 8, EncodeDeviceProps(o));
    Bytes p;
    PutBool(p, 3, false);
    PutMsg(p, 5, EncodeUserAgent(o));
    PutMsg(p, 6, EncodeWebInfo(o));
    if (!o.push_name.empty()) PutString(p, 7, o.push_name);
    PutU32(p, 12, 1); PutU32(p, 13, 1);
    PutMsg(p, 19, pair);
    PutBool(p, 33, false);
    return p;
}

std::optional<ClientPayloadInfo> DecodeClientPayload(const Bytes& raw) {
    std::vector<std::pair<int, Bytes>> f; if (!Fields(raw, f)) return std::nullopt;
    ClientPayloadInfo info;
    for (auto& [n, v] : f) {
        if (n == 1) info.username = AsVarint(v);
        else if (n == 3) info.passive = AsVarint(v) != 0;
        else if (n == 18) info.device = static_cast<std::uint32_t>(AsVarint(v));
        else if (n == 33) info.pull = AsVarint(v) != 0;
        else if (n == 19) {
            info.has_pairing = true;
            std::vector<std::pair<int, Bytes>> pf; if (!Fields(v, pf)) return std::nullopt;
            for (auto& [pn, pv] : pf) {
                if (pn == 1 && pv.size() >= 4) info.registration_id = (pv[0] << 24) | (pv[1] << 16) | (pv[2] << 8) | pv[3];
                else if (pn == 3) info.e_ident = pv;
                else if (pn == 4 && pv.size() >= 3) info.signed_prekey_id = (pv[0] << 16) | (pv[1] << 8) | pv[2];
                else if (pn == 5) info.e_skey_val = pv;
                else if (pn == 6) info.e_skey_sig = pv;
                else if (pn == 7) info.build_hash = pv;
                else if (pn == 8) info.device_props = pv;
            }
        }
    }
    return info;
}

PairSuccessResult ConfigureSuccessfulPairing(const BinaryNode& iq, AuthCreds& creds) {
    PairSuccessResult r;
    const auto* ps = Child(iq, "pair-success");
    if (!ps) { r.error = "missing pair-success"; return r; }
    const auto* di = Child(*ps, "device-identity");
    const auto* device = Child(*ps, "device");
    if (!di || !device) { r.error = "missing device-identity or device"; return r; }
    r.me_jid = Attr(*device, "jid"); r.me_lid = Attr(*device, "lid");
    if (const auto* plat = Child(*ps, "platform")) r.platform = Attr(*plat, "name");
    if (const auto* biz = Child(*ps, "biz")) r.biz_name = Attr(*biz, "name");

    std::vector<std::pair<int, Bytes>> top; if (!Fields(di->content, top)) { r.error = "undecodable ADVSignedDeviceIdentityHMAC"; return r; }
    Bytes details, hmac; bool hosted = false;
    for (auto& [n, v] : top) {
        if (n == 1) details = v; else if (n == 2) hmac = v; else if (n == 3 && AsVarint(v) == 1) hosted = true;
    }
    if (details.empty() || hmac.size() != 32) { r.error = "missing details/hmac"; return r; }
    Bytes hmac_in = hosted ? Cat({kAdvHostedAccountSigPrefix, details}) : details;
    auto want = HmacSha256(creds.adv_secret, hmac_in);
    if (want != hmac) { r.error = "Invalid account signature"; return r; }

    auto account = DecodeAdvSignedDeviceIdentity(details);
    if (!account) { r.error = "undecodable ADVSignedDeviceIdentity"; return r; }
    account->hosted = hosted || account->hosted;
    const Bytes& acct_prefix = account->hosted ? kAdvHostedAccountSigPrefix : kAdvAccountSigPrefix;
    Bytes account_msg = Cat({acct_prefix, account->details, creds.signal.identity.pub});
    if (!VerifyXEdDsa(account->account_signature_key, account_msg, account->account_signature)) {
        r.error = "Failed to verify account signature"; return r;
    }
    Bytes device_msg = Cat({kAdvDeviceSigPrefix, account->details, creds.signal.identity.pub, account->account_signature_key});
    account->device_signature = SignXEdDsa(creds.signal.identity.priv, device_msg);
    r.account = *account;

    BinaryNode reply; reply.tag = "iq"; reply.has_content = true;
    reply.attrs = {{"to", "s.whatsapp.net"}, {"type", "result"}, {"id", Attr(iq, "id")}};
    BinaryNode pds; pds.tag = "pair-device-sign"; pds.has_content = true;
    BinaryNode di_out; di_out.tag = "device-identity"; di_out.has_content = true;
    di_out.attrs = {{"key-index", std::to_string(account->key_index)}};
    di_out.content = EncodeAdvSignedDeviceIdentity(*account, false);
    di_out.content_is_string = false;
    pds.children.push_back(di_out);
    reply.children.push_back(pds);
    r.reply = reply;

    creds.me_jid = r.me_jid; creds.platform = r.platform; creds.registered = true;
    r.ok = true;
    return r;
}

BinaryNode BuildPairSuccessIq(const AuthCreds& companion, const KeyPair& account_identity, const std::string& jid,
                              const std::string& lid, const std::string& platform, const std::string& iq_id, bool hosted) {
    auto details = EncodeAdvDeviceIdentity(1, 1700000000, 1, hosted);
    const Bytes& acct_prefix = hosted ? kAdvHostedAccountSigPrefix : kAdvAccountSigPrefix;
    Bytes account_msg = Cat({acct_prefix, details, companion.signal.identity.pub});
    AdvAccount a; a.details = details; a.account_signature_key = account_identity.pub;
    a.account_signature = SignXEdDsa(account_identity.priv, account_msg); a.key_index = 1; a.hosted = hosted;
    Bytes signed_id = EncodeAdvSignedDeviceIdentity(a, true);
    Bytes hmac_in = hosted ? Cat({kAdvHostedAccountSigPrefix, signed_id}) : signed_id;
    Bytes hmac = HmacSha256(companion.adv_secret, hmac_in);
    Bytes hmac_msg = EncodeAdvSignedDeviceIdentityHmac(signed_id, hmac, hosted);

    BinaryNode iq; iq.tag = "iq"; iq.has_content = true;
    iq.attrs = {{"from", "s.whatsapp.net"}, {"type", "result"}, {"id", iq_id}};
    BinaryNode ps; ps.tag = "pair-success"; ps.has_content = true;
    BinaryNode di; di.tag = "device-identity"; di.has_content = true; di.content = hmac_msg;
    BinaryNode device; device.tag = "device"; device.attrs = {{"jid", jid}}; if (!lid.empty()) device.attrs.emplace_back("lid", lid);
    BinaryNode plat; plat.tag = "platform"; plat.attrs = {{"name", platform}};
    ps.children = {di, device, plat};
    iq.children.push_back(ps);
    return iq;
}

}  // namespace openclaw::channels::waweb
