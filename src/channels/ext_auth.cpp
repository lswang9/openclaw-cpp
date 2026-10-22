#include "openclaw/channels/ext_auth.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

#include <chrono>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

#include "openclaw/channels/outbound.hpp"
#include "openclaw/channels/irc_native.hpp"

namespace openclaw::channels {
namespace {
using json = nlohmann::json;
size_t Wcb(char* p, size_t s, size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }
long HttpGet(const std::string& url, const std::vector<std::string>& hdrs, std::string* out, long timeout_s = 15, const std::string* post = nullptr) {
    CURL* c = curl_easy_init(); if (!c) return 0;
    curl_slist* h = nullptr; for (auto& x : hdrs) h = curl_slist_append(h, x.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str()); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Wcb); curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s); curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (post) { curl_easy_setopt(c, CURLOPT_POST, 1L); curl_easy_setopt(c, CURLOPT_POSTFIELDS, post->c_str()); }
    long code = 0; if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h); curl_easy_cleanup(c); return code;
}
std::string B64UrlDecode(std::string s) {
    for (auto& ch : s) { if (ch == '-') ch = '+'; else if (ch == '_') ch = '/'; }
    while (s.size() % 4) s += '=';
    std::string out(s.size() / 4 * 3, '\0');
    int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()), reinterpret_cast<const unsigned char*>(s.data()), static_cast<int>(s.size()));
    if (n < 0) return {};
    size_t pad = 0; while (pad < 2 && s.size() > pad && s[s.size() - 1 - pad] == '=') ++pad;
    out.resize(static_cast<size_t>(n) - pad); return out;
}
long long Now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::mutex g_jwks_mu;
std::map<std::string, std::pair<long long, json>> g_jwks;  // url → (fetchedAt, keys)
json Jwks(const std::string& url, bool force) {
    std::lock_guard<std::mutex> l(g_jwks_mu);
    auto it = g_jwks.find(url);
    if (!force && it != g_jwks.end() && Now() - it->second.first < 3600) return it->second.second;
    std::string body; if (HttpGet(url, {"Accept: application/json"}, &body) != 200) return it != g_jwks.end() ? it->second.second : json();
    auto j = json::parse(body, nullptr, false);
    json keys = j.is_object() && j.contains("keys") ? j["keys"] : json::array();
    g_jwks[url] = {Now(), keys};
    return keys;
}
EVP_PKEY* RsaFromJwk(const json& k) {
    const auto n = B64UrlDecode(k.value("n", "")), e = B64UrlDecode(k.value("e", ""));
    if (n.empty() || e.empty()) return nullptr;
    BIGNUM* bn = BN_bin2bn(reinterpret_cast<const unsigned char*>(n.data()), static_cast<int>(n.size()), nullptr);
    BIGNUM* be = BN_bin2bn(reinterpret_cast<const unsigned char*>(e.data()), static_cast<int>(e.size()), nullptr);
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn); OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, be);
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    EVP_PKEY* pkey = nullptr;
    if (ctx && EVP_PKEY_fromdata_init(ctx) > 0) EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);
    EVP_PKEY_CTX_free(ctx); OSSL_PARAM_free(params); OSSL_PARAM_BLD_free(bld); BN_free(bn); BN_free(be);
    return pkey;
}
bool VerifySig(EVP_PKEY* k, const std::string& data, const std::string& sig) {
    EVP_MD_CTX* m = EVP_MD_CTX_new(); bool ok = false;
    if (EVP_DigestVerifyInit(m, nullptr, EVP_sha256(), nullptr, k) == 1)
        ok = EVP_DigestVerify(m, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(), reinterpret_cast<const unsigned char*>(data.data()), data.size()) == 1;
    EVP_MD_CTX_free(m); return ok;
}
std::string Cfg(const json& cfg, const std::string& ch, const char* k, const char* env = nullptr) { return ChannelCred(cfg, ch, k, env ? env : ""); }
}  // namespace

bool VerifyJwtRs256(const std::string& jwt, const std::string& jwks_url, const std::vector<std::string>& issuers, const std::string& aud,
                    std::string* why, json* claims) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    const auto d1 = jwt.find('.'), d2 = jwt.find('.', d1 == std::string::npos ? 0 : d1 + 1);
    if (d1 == std::string::npos || d2 == std::string::npos) return fail("malformed jwt");
    auto hdr = json::parse(B64UrlDecode(jwt.substr(0, d1)), nullptr, false), pl = json::parse(B64UrlDecode(jwt.substr(d1 + 1, d2 - d1 - 1)), nullptr, false);
    if (!hdr.is_object() || !pl.is_object()) return fail("malformed jwt");
    if (hdr.value("alg", "") != "RS256") return fail("unsupported jwt alg " + hdr.value("alg", ""));
    const auto kid = hdr.value("kid", std::string()), sig = B64UrlDecode(jwt.substr(d2 + 1)), data = jwt.substr(0, d2);
    bool verified = false;
    for (int attempt = 0; attempt < 2 && !verified; ++attempt) {  // refetch once on unknown kid (key rotation)
        bool found = false;
        for (auto& k : Jwks(jwks_url, attempt == 1)) {
            if (!kid.empty() && k.value("kid", "") != kid) continue;
            found = true;
            if (EVP_PKEY* p = RsaFromJwk(k)) { verified = VerifySig(p, data, sig); EVP_PKEY_free(p); }
            if (verified) break;
        }
        if (found && !verified) break;
    }
    if (!verified) return fail("jwt signature not verified");
    const long long now = Now(), skew = 300;
    if (pl.contains("exp") && pl["exp"].is_number() && now > pl["exp"].get<long long>() + skew) return fail("jwt expired");
    if (pl.contains("nbf") && pl["nbf"].is_number() && now + skew < pl["nbf"].get<long long>()) return fail("jwt not yet valid");
    if (!issuers.empty() && std::find(issuers.begin(), issuers.end(), pl.value("iss", "")) == issuers.end()) return fail("jwt issuer " + pl.value("iss", "") + " not trusted");
    if (!aud.empty()) {
        bool ok = pl.contains("aud") && ((pl["aud"].is_string() && pl["aud"] == aud) ||
                                         (pl["aud"].is_array() && std::find(pl["aud"].begin(), pl["aud"].end(), json(aud)) != pl["aud"].end()));
        if (!ok) return fail("jwt audience mismatch");
    }
    if (claims) *claims = pl;
    return true;
}

namespace {
std::string TeamsJwksUrl(const json& cfg) {
    auto meta = Cfg(cfg, "msteams", "openIdMetadataUrl", "MSTEAMS_OPENID_METADATA_URL");
    if (meta.empty()) meta = "https://login.botframework.com/v1/.well-known/openidconfiguration";
    static std::mutex mu; static std::map<std::string, std::string> cache;
    std::lock_guard<std::mutex> l(mu);
    if (cache.count(meta)) return cache[meta];
    std::string body; if (HttpGet(meta, {}, &body) != 200) return {};
    auto j = json::parse(body, nullptr, false);
    const auto u = j.is_object() ? j.value("jwks_uri", std::string()) : std::string();
    if (!u.empty()) cache[meta] = u;
    return u;
}
std::string Bearer(const HeaderFn& h) {
    auto a = h("Authorization");
    return a.rfind("Bearer ", 0) == 0 ? a.substr(7) : std::string();
}
bool ConstEq(const std::string& a, const std::string& b) {
    unsigned d = a.size() ^ b.size();
    for (size_t i = 0; i < b.size(); ++i) d |= static_cast<unsigned char>(b[i]) ^ static_cast<unsigned char>(i < a.size() ? a[i] : 0);
    return d == 0;
}
}  // namespace

bool VerifyExtInbound(const std::string& id, const json& cfg, const HeaderFn& h, const std::string& query, std::string* why) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    if (id == "msteams") {
        const auto app = Cfg(cfg, "msteams", "appId", "MSTEAMS_APP_ID");
        auto v = config::GetPath(cfg, "channels.msteams.verifyJwt");
        if (app.empty() || (v && v->is_boolean() && !v->get<bool>())) return true;
        const auto tok = Bearer(h); if (tok.empty()) return fail("missing Bot Framework bearer token");
        const auto jwks = TeamsJwksUrl(cfg); if (jwks.empty()) return fail("Bot Framework OpenID metadata unavailable");
        return VerifyJwtRs256(tok, jwks, {"https://api.botframework.com"}, app, why);
    }
    if (id == "googlechat") {
        const auto aud = Cfg(cfg, "googlechat", "audience", "GOOGLE_CHAT_AUDIENCE");
        if (aud.empty()) return true;
        const auto tok = Bearer(h); if (tok.empty()) return fail("missing Google Chat bearer token");
        auto jwks = Cfg(cfg, "googlechat", "jwksUrl", "GOOGLE_CHAT_JWKS_URL");
        if (jwks.empty()) jwks = "https://www.googleapis.com/service_accounts/v1/jwk/chat@system.gserviceaccount.com";
        return VerifyJwtRs256(tok, jwks, {"chat@system.gserviceaccount.com"}, aud, why);
    }
    if (id == "matrix") {
        const auto hs = Cfg(cfg, "matrix", "hsToken", "MATRIX_HS_TOKEN");
        if (hs.empty()) return true;
        std::string got = Bearer(h);
        if (got.empty()) { auto p = query.find("access_token="); if (p != std::string::npos) { got = query.substr(p + 13); got = got.substr(0, got.find('&')); } }
        return ConstEq(got, hs) ? true : fail("matrix app-service hs_token mismatch");
    }
    if (id == "signal") {
        const auto sec = Cfg(cfg, "signal", "webhookSecret", "SIGNAL_WEBHOOK_SECRET");
        if (sec.empty()) return true;
        return ConstEq(h("X-Webhook-Secret"), sec) ? true : fail("signal webhook secret mismatch");
    }
    return true;
}

namespace {
void Nap(const std::atomic<bool>& stop, int ms) { for (int i = 0; i < ms / 50 && !stop.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
std::string Esc(const std::string& s) { CURL* c = curl_easy_init(); char* e = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size())); std::string r = e ? e : ""; curl_free(e); curl_easy_cleanup(c); return r; }
std::string Trim(std::string s) { while (!s.empty() && s.back() == '/') s.pop_back(); return s; }
int EnvInt(const char* k, int d) { const char* v = std::getenv(k); return v && *v ? std::atoi(v) : d; }
std::string Mode(const json& cfg, const std::string& ch, const char* key) { auto v = config::GetPath(cfg, "channels." + ch + "." + key); return v && v->is_string() ? v->get<std::string>() : std::string(); }

// 1.143: signal-cli JSON-RPC daemon — Server-Sent Events from {httpUrl}/api/v1/events (each data line is a receive
// notification {envelope, account} or a full JSON-RPC {method:"receive", params:{envelope}}).
struct SseCtx { std::string buf; const std::atomic<bool>* stop; std::function<void(const std::string&)> on_data; };
size_t SseWrite(char* p, size_t s, size_t n, void* u) {
    auto* c = static_cast<SseCtx*>(u); c->buf.append(p, s * n);
    std::size_t pos;
    while ((pos = c->buf.find('\n')) != std::string::npos) {
        std::string line = c->buf.substr(0, pos); c->buf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data:", 0) == 0) { std::string d = line.substr(5); if (!d.empty() && d[0] == ' ') d.erase(0, 1); if (!d.empty()) c->on_data(d); }
    }
    return s * n;
}
int SseProgress(void* u, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return static_cast<SseCtx*>(u)->stop->load() ? 1 : 0; }

void SignalSseLoop(const std::string& rpc, const std::string& number, const std::atomic<bool>& stop, Adapter* ad, const DeliverFn& deliver) {
    std::cerr << "[gateway] signal: JSON-RPC daemon events (SSE) " << rpc << "\n";
    int backoff = 500;
    while (!stop.load()) {
        SseCtx ctx{{}, &stop, [&](const std::string& d) {
            auto j = json::parse(d, nullptr, false);
            if (!j.is_object()) return;
            json item = j.contains("params") && j["params"].is_object() ? j["params"] : j;
            if (!item.contains("envelope")) return;
            try { ad->HandleHttpInbound(item, deliver); } catch (const std::exception& e) { std::cerr << "[gateway] signal event: " << e.what() << "\n"; }
        }};
        CURL* c = curl_easy_init(); if (!c) return;
        std::string url = rpc + "/api/v1/events"; if (!number.empty()) url += "?account=" + Esc(number);
        curl_slist* h = curl_slist_append(nullptr, "Accept: text/event-stream");
        curl_easy_setopt(c, CURLOPT_URL, url.c_str()); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, SseWrite); curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L); curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, SseProgress); curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        const auto rc = curl_easy_perform(c);
        long st = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
        curl_slist_free_all(h); curl_easy_cleanup(c);
        if (stop.load()) break;
        if (rc == CURLE_OK && st == 200) backoff = 500; else backoff = std::min(backoff * 2, 30000);
        Nap(stop, backoff);
    }
}

// 1.143: Mattermost WebSocket event stream (/api/v4/websocket): authentication_challenge, then "posted" events
// (data.post is a JSON string) for every channel the bot can see — DMs included, no channel list needed.
void MattermostWsLoop(const std::string& base, const std::string& tok, std::string me, const std::string& hook_token,
                      const std::atomic<bool>& stop, Adapter* ad, const DeliverFn& deliver) {
    std::string ws = base;
    if (ws.rfind("https://", 0) == 0) ws = "wss://" + ws.substr(8); else if (ws.rfind("http://", 0) == 0) ws = "ws://" + ws.substr(7);
    ws += "/api/v4/websocket";
    std::cerr << "[gateway] mattermost: websocket " << ws << "\n";
    int backoff = 500;
    while (!stop.load()) {
        CURL* c = curl_easy_init(); if (!c) return;
        curl_slist* h = curl_slist_append(nullptr, ("Authorization: Bearer " + tok).c_str());
        curl_easy_setopt(c, CURLOPT_URL, ws.c_str()); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L); curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        if (curl_easy_perform(c) != CURLE_OK) { curl_slist_free_all(h); curl_easy_cleanup(c); Nap(stop, backoff); backoff = std::min(backoff * 2, 30000); continue; }
        backoff = 500;
        { const auto a = json{{"seq", 1}, {"action", "authentication_challenge"}, {"data", {{"token", tok}}}}.dump(); size_t sent = 0; curl_ws_send(c, a.data(), a.size(), &sent, 0, CURLWS_TEXT); }
        std::string frame; char buf[65536];
        while (!stop.load()) {
            size_t n = 0; const curl_ws_frame* meta = nullptr;
            const auto rc = curl_ws_recv(c, buf, sizeof buf, &n, &meta);
            if (rc == CURLE_AGAIN) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
            if (rc != CURLE_OK || (meta && (meta->flags & CURLWS_CLOSE))) break;
            if (meta && (meta->flags & CURLWS_PING)) { size_t sent = 0; curl_ws_send(c, buf, n, &sent, 0, CURLWS_PONG); continue; }
            frame.append(buf, n);
            if (meta && meta->bytesleft > 0) continue;
            auto ev = json::parse(frame, nullptr, false); frame.clear();
            if (!ev.is_object()) continue;
            if (ev.value("event", "") == "hello" && me.empty() && ev.contains("broadcast")) me = ev["broadcast"].value("user_id", std::string());
            if (ev.value("event", "") != "posted" || !ev.contains("data")) continue;
            const auto& d = ev["data"];
            auto p = d.contains("post") && d["post"].is_string() ? json::parse(d["post"].get<std::string>(), nullptr, false) : d.value("post", json());
            if (!p.is_object() || p.value("user_id", "") == me || !p.value("type", std::string()).empty()) continue;
            json body{{"token", hook_token}, {"channel_id", p.value("channel_id", "")}, {"user_id", p.value("user_id", "")}, {"text", p.value("message", "")},
                      {"post_id", p.value("id", "")}, {"root_id", p.value("root_id", "")}, {"channel_type", d.value("channel_type", std::string("O"))}};
            try { ad->HandleHttpInbound(body, deliver); } catch (const std::exception& e) { std::cerr << "[gateway] mattermost ws post: " << e.what() << "\n"; }
        }
        curl_slist_free_all(h); curl_easy_cleanup(c);
        if (!stop.load()) Nap(stop, backoff);
    }
}

void MatrixLoop(const json& cfg, const std::filesystem::path& state, const std::atomic<bool>& stop, Adapter* ad, const DeliverFn& deliver) {
    const auto hs = Trim(Cfg(cfg, "matrix", "homeserver", "MATRIX_HOMESERVER")), tok = Cfg(cfg, "matrix", "accessToken", "MATRIX_ACCESS_TOKEN");
    const auto mode = Mode(cfg, "matrix", "mode");
    if (hs.empty() || tok.empty() || mode == "appservice" || mode == "webhook" || mode == "off") return;
    const std::vector<std::string> auth{"Authorization: Bearer " + tok};
    const auto sf = state / "matrix" / "sync.json";
    std::string since; { std::ifstream f(sf); if (f) { auto j = json::parse(f, nullptr, false); if (j.is_object()) since = j.value("since", ""); } }
    auto v = config::GetPath(cfg, "channels.matrix.autoJoin");
    const bool auto_join = !(v && v->is_boolean() && !v->get<bool>());
    const int tmo = EnvInt("OPENCLAW_MATRIX_SYNC_TIMEOUT_MS", 30000);
    std::cerr << "[gateway] matrix: /sync long polling" << (since.empty() ? " (initial, backlog skipped)" : "") << "\n";
    int backoff = 500;
    while (!stop.load()) {
        std::string url = hs + "/_matrix/client/v3/sync?timeout=" + std::to_string(since.empty() ? 0 : tmo);
        if (since.empty()) url += "&filter=" + Esc(R"({"room":{"timeline":{"limit":0}}})"); else url += "&since=" + Esc(since);
        std::string body; const long st = HttpGet(url, auth, &body, tmo / 1000 + 15);
        auto j = json::parse(body, nullptr, false);
        if (st != 200 || !j.is_object() || !j.contains("next_batch")) { Nap(stop, backoff); backoff = std::min(backoff * 2, 30000); continue; }
        backoff = 500;
        const bool initial = since.empty();
        since = j["next_batch"].get<std::string>();
        { std::error_code ec; std::filesystem::create_directories(sf.parent_path(), ec); std::ofstream(sf) << json{{"since", since}}.dump(); }
        const json rooms = j.value("rooms", json::object());
        if (auto_join && rooms.contains("invite"))
            for (auto& [room, _] : rooms["invite"].items()) { std::string b, e = "{}"; HttpGet(hs + "/_matrix/client/v3/join/" + Esc(room), {auth[0], "Content-Type: application/json"}, &b, 15, &e); }
        if (initial || !rooms.contains("join")) continue;
        for (auto& [room, r] : rooms["join"].items()) {
            for (auto ev : r.value("timeline", json::object()).value("events", json::array())) {
                if (ev.value("type", "") != "m.room.message") continue;
                ev["room_id"] = room;
                try { ad->HandleHttpInbound({{"events", json::array({ev})}}, deliver); } catch (const std::exception& e) { std::cerr << "[gateway] matrix event: " << e.what() << "\n"; }
            }
        }
    }
}

void SignalLoop(const json& cfg, const std::atomic<bool>& stop, Adapter* ad, const DeliverFn& deliver) {
    const auto base = Trim(Cfg(cfg, "signal", "apiBase", "SIGNAL_API_BASE")), number = Cfg(cfg, "signal", "number", "SIGNAL_NUMBER");
    const auto mode = Mode(cfg, "signal", "receiveMode");
    if (mode == "webhook" || mode == "off") return;
    if (const auto rpc = Trim(Cfg(cfg, "signal", "httpUrl", "SIGNAL_HTTP_URL")); !rpc.empty()) return SignalSseLoop(rpc, number, stop, ad, deliver);
    if (base.empty() || number.empty()) return;
    const int tmo = EnvInt("OPENCLAW_SIGNAL_RECEIVE_TIMEOUT", 10);
    std::cerr << "[gateway] signal: polling /v1/receive\n";
    while (!stop.load()) {
        std::string body; const long st = HttpGet(base + "/v1/receive/" + Esc(number) + "?timeout=" + std::to_string(tmo), {}, &body, tmo + 15);
        auto j = json::parse(body, nullptr, false);
        if (st != 200 || !j.is_array()) { Nap(stop, 2000); continue; }
        for (auto& item : j) try { ad->HandleHttpInbound(item, deliver); } catch (const std::exception& e) { std::cerr << "[gateway] signal envelope: " << e.what() << "\n"; }
        if (j.empty() && tmo == 0) Nap(stop, 500);
    }
}

void MattermostLoop(const json& cfg, const std::atomic<bool>& stop, Adapter* ad, const DeliverFn& deliver) {
    const auto base = Trim(Cfg(cfg, "mattermost", "baseUrl", "MATTERMOST_URL")), tok = Cfg(cfg, "mattermost", "botToken", "MATTERMOST_BOT_TOKEN");
    auto chans = config::GetPath(cfg, "channels.mattermost.channels");
    const auto mmode = Mode(cfg, "mattermost", "mode");
    if (base.empty() || tok.empty() || mmode == "webhook" || mmode == "off") return;
    const std::vector<std::string> auth{"Authorization: Bearer " + tok};
    std::string me = Cfg(cfg, "mattermost", "botUserId", "MATTERMOST_BOT_USER_ID");
    if (me.empty()) { std::string b; if (HttpGet(base + "/api/v4/users/me", auth, &b) == 200) { auto j = json::parse(b, nullptr, false); if (j.is_object()) me = j.value("id", ""); } }
    // 1.143: WebSocket by default (mode "websocket", or no channel list); "poll" keeps the 1.132 per-channel polling.
    if (mmode == "websocket" || (mmode != "poll" && (!chans || !chans->is_array() || chans->empty())))
        return MattermostWsLoop(base, tok, me, Cfg(cfg, "mattermost", "webhookToken", "MATTERMOST_WEBHOOK_TOKEN"), stop, ad, deliver);
    if (!chans || !chans->is_array() || chans->empty()) return;
    std::map<std::string, long long> since; std::map<std::string, std::string> ctype;
    const long long start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for (auto& c : *chans) if (c.is_string()) {
        since[c.get<std::string>()] = start;
        std::string b; if (HttpGet(base + "/api/v4/channels/" + Esc(c.get<std::string>()), auth, &b) == 200) { auto j = json::parse(b, nullptr, false); if (j.is_object()) ctype[c.get<std::string>()] = j.value("type", "O"); }
    }
    const auto hook_token = Cfg(cfg, "mattermost", "webhookToken", "MATTERMOST_WEBHOOK_TOKEN");
    const int every = EnvInt("OPENCLAW_MATTERMOST_POLL_MS", 2000);
    std::cerr << "[gateway] mattermost: polling " << since.size() << " channel(s)\n";
    while (!stop.load()) {
        for (auto& [ch, last] : since) {
            std::string b; if (HttpGet(base + "/api/v4/channels/" + Esc(ch) + "/posts?since=" + std::to_string(last), auth, &b) != 200) continue;
            auto j = json::parse(b, nullptr, false); if (!j.is_object()) continue;
            std::vector<json> posts; const json pm = j.value("posts", json::object()); for (auto& [id, p] : pm.items()) posts.push_back(p);
            std::sort(posts.begin(), posts.end(), [](const json& a, const json& b) { return a.value("create_at", 0LL) < b.value("create_at", 0LL); });
            for (auto& p : posts) {
                const long long at = p.value("create_at", 0LL);
                if (at <= last) continue;
                last = std::max(last, at);
                if (p.value("user_id", "") == me || !p.value("type", std::string()).empty() || p.value("delete_at", 0LL) != 0) continue;
                json body{{"token", hook_token}, {"channel_id", ch}, {"user_id", p.value("user_id", "")}, {"text", p.value("message", "")},
                          {"post_id", p.value("id", "")}, {"root_id", p.value("root_id", "")}, {"channel_type", ctype.count(ch) ? ctype[ch] : "O"}};
                try { ad->HandleHttpInbound(body, deliver); } catch (const std::exception& e) { std::cerr << "[gateway] mattermost post: " << e.what() << "\n"; }
            }
        }
        Nap(stop, every);
    }
}
}  // namespace

void RunExtListeners(const json& cfg, const std::filesystem::path& state, const std::atomic<bool>& stop,
                     const std::function<Adapter*(const std::string&)>& adapter, const DeliverFn& deliver) {
    std::vector<std::thread> ts;
    if (auto* a = adapter("matrix")) ts.emplace_back([&, a] { MatrixLoop(cfg, state, stop, a, deliver); });
    if (auto* a = adapter("signal")) ts.emplace_back([&, a] { SignalLoop(cfg, stop, a, deliver); });
    if (auto* a = adapter("mattermost")) ts.emplace_back([&, a] { MattermostLoop(cfg, stop, a, deliver); });
    if (auto* a = adapter("irc")) ts.emplace_back([&, a] { RunIrcClient("irc", cfg, stop, a, deliver); });
    if (auto* a = adapter("twitch")) ts.emplace_back([&, a] { RunIrcClient("twitch", cfg, stop, a, deliver); });
    for (auto& t : ts) t.join();
}
}  // namespace openclaw::channels
