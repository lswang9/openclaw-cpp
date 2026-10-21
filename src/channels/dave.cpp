#include "openclaw/channels/dave.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>

#ifdef OPENCLAW_HAVE_DAVE
#include <dave/dave.h>
#endif

namespace openclaw::channels::dave {

#ifdef OPENCLAW_HAVE_DAVE
bool Available() { return true; }
std::uint16_t MaxProtocolVersion() { return daveMaxSupportedProtocolVersion(); }

struct Session::Impl {
    DAVESessionHandle session = nullptr;
    DAVEEncryptorHandle encryptor = nullptr;
    std::map<std::string, DAVEDecryptorHandle> decryptors;
    std::string failure;
    ~Impl() {
        for (auto& [k, d] : decryptors) daveDecryptorDestroy(d);
        if (encryptor) daveEncryptorDestroy(encryptor);
        if (session) daveSessionDestroy(session);
    }
};

namespace {
void OnFailure(const char* source, const char* reason, void* user) {
    static_cast<std::string*>(user)->assign(std::string(source ? source : "") + ": " + (reason ? reason : ""));
}
Bytes Take(std::uint8_t* p, std::size_t n) { Bytes b(p, p + n); if (p) daveFree(p); return b; }
std::vector<const char*> Ids(const std::set<std::string>& s, std::vector<std::string>& keep) {
    keep.assign(s.begin(), s.end());
    std::vector<const char*> v; for (auto& k : keep) v.push_back(k.c_str());
    return v;
}
}  // namespace

Session::Session(std::string self, std::uint64_t channel) : impl_(std::make_unique<Impl>()), self_(std::move(self)), group_(channel) {
    static const bool sink = [] {  // libdave logs to stdout by default; keep CLI/JSON output clean
        daveSetLogSinkCallback([](DAVELoggingSeverity sev, const char* file, int line, const char* msg) {
            if (sev >= DAVE_LOGGING_SEVERITY_WARNING || std::getenv("OPENCLAW_DAVE_DEBUG"))
                std::fprintf(stderr, "[dave] (%s:%d) %s\n", file ? file : "", line, msg ? msg : "");
        });
        return true;
    }();
    (void)sink;
    impl_->session = daveSessionCreate(nullptr, "", OnFailure, &impl_->failure);
    impl_->encryptor = daveEncryptorCreate();
    daveEncryptorSetPassthroughMode(impl_->encryptor, true);
    roster_.insert(self_);
}
Session::~Session() = default;

std::vector<Outgoing> Session::Start(std::uint16_t v) {
    std::vector<Outgoing> out;
    version_ = v;
    if (v == 0) { daveEncryptorSetPassthroughMode(impl_->encryptor, true); return out; }
    daveSessionInit(impl_->session, v, group_, self_.c_str());
    if (!external_sender_.empty()) daveSessionSetExternalSender(impl_->session, external_sender_.data(), external_sender_.size());
    std::uint8_t* kp = nullptr; std::size_t n = 0;
    daveSessionGetMarshalledKeyPackage(impl_->session, &kp, &n);
    out.push_back({true, 26, Take(kp, n), {}});
    return out;
}

void Session::ApplyRatchets() {
    if (version_ == 0) { daveEncryptorSetPassthroughMode(impl_->encryptor, true); return; }
    if (auto kr = daveSessionGetKeyRatchet(impl_->session, self_.c_str())) {
        daveEncryptorSetKeyRatchet(impl_->encryptor, kr);
        daveKeyRatchetDestroy(kr);
        daveEncryptorSetPassthroughMode(impl_->encryptor, false);
    }
    for (auto& id : roster_) {
        if (id == self_) continue;
        auto& d = impl_->decryptors[id];
        if (!d) d = daveDecryptorCreate();
        if (auto kr = daveSessionGetKeyRatchet(impl_->session, id.c_str())) { daveDecryptorTransitionToKeyRatchet(d, kr); daveKeyRatchetDestroy(kr); }
    }
}

std::vector<Outgoing> Session::Execute(int transition_id) {
    (void)transition_id;
    if (pending_version_ != version_) version_ = pending_version_;
    ApplyRatchets();
    pending_transition_ = -1;
    return {};
}

std::vector<Outgoing> Session::OnJson(int op, const nlohmann::json& d) {
    std::vector<Outgoing> out;
    if (op == 11) { for (auto& u : d.value("user_ids", nlohmann::json::array())) if (u.is_string()) roster_.insert(u.get<std::string>()); }
    else if (op == 13) { auto u = d.value("user_id", std::string()); roster_.erase(u); }
    else if (op == 21) {  // prepare transition (e.g. downgrade to 0)
        pending_version_ = d.value("protocol_version", version_);
        pending_transition_ = d.value("transition_id", 0);
        if (pending_transition_ == 0) return Execute(0);
        out.push_back({false, 23, {}, {{"op", 23}, {"d", {{"transition_id", pending_transition_}}}}});
    } else if (op == 22) {
        if (d.value("transition_id", -1) == pending_transition_ || pending_transition_ < 0) return Execute(d.value("transition_id", 0));
    } else if (op == 24) {  // prepare epoch: epoch 1 = new MLS group
        const auto v = static_cast<std::uint16_t>(d.value("protocol_version", 1));
        pending_version_ = v;
        if (d.value("epoch", 0) == 1) return Start(v);
    }
    return out;
}

std::vector<Outgoing> Session::OnBinary(int op, const Bytes& p) {
    std::vector<Outgoing> out;
    if (op == 25) {  // may arrive before Session Description: keep it and apply on (re)init too
        external_sender_ = p;
        if (version_ > 0) daveSessionSetExternalSender(impl_->session, p.data(), p.size());
        return out;
    }
    if (version_ == 0) return out;
    if (pending_version_ == 0) pending_version_ = version_;
    if (op == 27) {
        std::vector<std::string> keep; auto ids = Ids(roster_, keep);
        std::uint8_t* cw = nullptr; std::size_t n = 0;
        daveSessionProcessProposals(impl_->session, p.data(), p.size(), ids.data(), ids.size(), &cw, &n);
        if (cw && n) out.push_back({true, 28, Take(cw, n), {}});
        return out;
    }
    if (op == 29 || op == 30) {
        if (p.size() < 2) { last_error_ = "short transition message"; return out; }
        const int tid = (p[0] << 8) | p[1];
        bool ok = false;
        if (op == 29) {
            auto r = daveSessionProcessCommit(impl_->session, p.data() + 2, p.size() - 2);
            ok = r && !daveCommitResultIsFailed(r);
            const bool ignored = r && daveCommitResultIsIgnored(r);
            if (r) daveCommitResultDestroy(r);
            if (ignored) return out;
        } else {
            std::vector<std::string> keep; auto ids = Ids(roster_, keep);
            auto r = daveSessionProcessWelcome(impl_->session, p.data() + 2, p.size() - 2, ids.data(), ids.size());
            ok = r != nullptr;
            if (r) daveWelcomeResultDestroy(r);
        }
        if (!ok) {
            last_error_ = impl_->failure.empty() ? "MLS commit/welcome rejected" : impl_->failure;
            out.push_back({false, 31, {}, {{"op", 31}, {"d", {{"transition_id", tid}}}}});
            auto again = Start(version_);  // re-join with a fresh key package
            out.insert(out.end(), again.begin(), again.end());
            return out;
        }
        pending_transition_ = tid;
        if (tid == 0) { Execute(0); return out; }
        // Ratchets for the new epoch are prepared now; the media switch waits for op 22.
        out.push_back({false, 23, {}, {{"op", 23}, {"d", {{"transition_id", tid}}}}});
    }
    return out;
}

bool Session::Ready() const { return version_ == 0 || daveEncryptorHasKeyRatchet(impl_->encryptor); }

bool Session::Encrypt(std::uint32_t ssrc, const Bytes& frame, Bytes* out) {
    if (version_ == 0) { *out = frame; return true; }
    daveEncryptorAssignSsrcToCodec(impl_->encryptor, ssrc, DAVE_CODEC_OPUS);
    out->resize(daveEncryptorGetMaxCiphertextByteSize(impl_->encryptor, DAVE_MEDIA_TYPE_AUDIO, frame.size()));
    std::size_t n = 0;
    auto rc = daveEncryptorEncrypt(impl_->encryptor, DAVE_MEDIA_TYPE_AUDIO, ssrc, frame.data(), frame.size(), out->data(), out->size(), &n);
    if (rc != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS) { last_error_ = "encrypt failed rc=" + std::to_string(rc); return false; }
    out->resize(n); ++encrypted_;
    return true;
}

bool Session::Decrypt(const std::string& user, const Bytes& frame, Bytes* out) {
    if (version_ == 0) { *out = frame; return true; }
    auto it = impl_->decryptors.find(user);
    if (it == impl_->decryptors.end()) { ++decrypt_failed_; last_error_ = "no decryptor for user " + user; return false; }
    out->resize(daveDecryptorGetMaxPlaintextByteSize(it->second, DAVE_MEDIA_TYPE_AUDIO, frame.size()));
    std::size_t n = 0;
    auto rc = daveDecryptorDecrypt(it->second, DAVE_MEDIA_TYPE_AUDIO, frame.data(), frame.size(), out->data(), out->size(), &n);
    if (rc != DAVE_DECRYPTOR_RESULT_CODE_SUCCESS) { ++decrypt_failed_; last_error_ = "decrypt failed rc=" + std::to_string(rc); return false; }
    out->resize(n); ++decrypted_;
    return true;
}

nlohmann::json Session::Stats() const {
    return {{"protocol_version", version_}, {"ready", Ready()}, {"frames_encrypted", encrypted_}, {"frames_decrypted", decrypted_},
            {"decrypt_failures", decrypt_failed_}, {"roster", roster_.size()}};
}

#else  // !OPENCLAW_HAVE_DAVE

bool Available() { return false; }
std::uint16_t MaxProtocolVersion() { return 0; }
struct Session::Impl {};
Session::Session(std::string self, std::uint64_t channel) : impl_(std::make_unique<Impl>()), self_(std::move(self)), group_(channel) {}
Session::~Session() = default;
std::vector<Outgoing> Session::Start(std::uint16_t v) { version_ = v; if (v) last_error_ = "built without libdave"; return {}; }
std::vector<Outgoing> Session::OnJson(int, const nlohmann::json&) { return {}; }
std::vector<Outgoing> Session::OnBinary(int, const Bytes&) { return {}; }
bool Session::Ready() const { return version_ == 0; }
bool Session::Encrypt(std::uint32_t, const Bytes& f, Bytes* o) { if (version_) return false; *o = f; return true; }
bool Session::Decrypt(const std::string&, const Bytes& f, Bytes* o) { if (version_) return false; *o = f; return true; }
nlohmann::json Session::Stats() const { return {{"protocol_version", version_}, {"available", false}}; }
std::vector<Outgoing> Session::Execute(int) { return {}; }
void Session::ApplyRatchets() {}
#endif

}  // namespace openclaw::channels::dave
