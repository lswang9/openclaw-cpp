// Test-only helper for offline DAVE e2e (scripts/fake-discord-voice.py --dave). Line-delimited JSON on stdin/stdout.
// Plays the voice server's MLS external sender (signed external Add proposals, commit/welcome split — same
// construction as libdave's own test ExternalSender) and one simulated remote participant running real libdave.
#include <dave/dave.h>
#include <mls/messages.h>
#include <mls/state.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Bytes = std::vector<uint8_t>;

static std::string Hex(const Bytes& b) {
    static const char* k = "0123456789abcdef";
    std::string s; for (auto c : b) { s += k[c >> 4]; s += k[c & 15]; } return s;
}
static Bytes Unhex(const std::string& s) {
    Bytes b; for (size_t i = 0; i + 1 < s.size(); i += 2) b.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16))); return b;
}
static Bytes Take(uint8_t* p, size_t n) { Bytes b(p, p + n); if (p) daveFree(p); return b; }

int main() {
    daveSetLogSinkCallback([](DAVELoggingSeverity, const char* file, int line, const char* msg) {
        std::cerr << "(" << (file ? file : "") << ":" << line << ") " << (msg ? msg : "") << "\n";
    });
    const mlspp::CipherSuite suite{mlspp::CipherSuite::ID::P256_AES128GCM_SHA256_P256};
    std::shared_ptr<mlspp::SignaturePrivateKey> sig;
    mlspp::ExternalSender ext;
    mlspp::bytes_ns::bytes group_id;
    uint64_t group = 0; uint16_t version = 1;
    DAVESessionHandle peer = nullptr; DAVEEncryptorHandle enc = nullptr; DAVEDecryptorHandle dec = nullptr;
    std::string peer_user, failure;
    std::string line;
    while (std::getline(std::cin, line)) {
        json in, out{{"ok", true}};
        try {
            in = json::parse(line);
            const std::string cmd = in.value("cmd", "");
            if (cmd == "init") {
                group = in.value("group", uint64_t{0}); version = in.value("version", 1);
                group_id = mlspp::bytes_ns::bytes(std::vector<uint8_t>(8));
                for (int i = 0; i < 8; ++i) group_id.at(i) = static_cast<uint8_t>(group >> ((7 - i) * 8));
                sig = std::make_shared<mlspp::SignaturePrivateKey>(mlspp::SignaturePrivateKey::generate(suite));
                ext.signature_key = sig->public_key;
                ext.credential = mlspp::Credential::basic({0x00, 0x01, 0x01, 0x00});
                out["external_sender"] = Hex(mlspp::tls::marshal(ext));
            } else if (cmd == "propose_add") {
                std::vector<mlspp::MLSMessage> msgs;
                for (auto& kp : in["key_packages"]) {
                    const auto bytes = mlspp::bytes_ns::bytes(Unhex(kp.get<std::string>()));
                    auto proposal = mlspp::Proposal{mlspp::Add{mlspp::tls::get<mlspp::KeyPackage>(bytes)}};
                    msgs.push_back(mlspp::external_proposal(suite, group_id, in.value("epoch", 0), proposal, 0, *sig));
                }
                mlspp::tls::ostream o; o << false; o << msgs;
                out["proposals"] = Hex(o.bytes());
            } else if (cmd == "split") {
                auto b = mlspp::bytes_ns::bytes(Unhex(in["commit_welcome"].get<std::string>()));
                mlspp::tls::istream is(b);
                mlspp::MLSMessage commit; is >> commit;
                mlspp::tls::ostream co; co << commit; out["commit"] = Hex(co.bytes());
                if (!is.empty()) { mlspp::Welcome w; is >> w; mlspp::tls::ostream wo; wo << w; out["welcome"] = Hex(wo.bytes()); }
            } else if (cmd == "peer_create") {
                peer_user = in.value("user", "peer");
                peer = daveSessionCreate(nullptr, "", [](const char* s, const char* r, void* u) { *static_cast<std::string*>(u) = std::string(s) + ": " + r; }, &failure);
                daveSessionInit(peer, version, group, peer_user.c_str());
                const auto es = mlspp::tls::marshal(ext);
                daveSessionSetExternalSender(peer, es.data(), es.size());
                uint8_t* kp = nullptr; size_t n = 0;
                daveSessionGetMarshalledKeyPackage(peer, &kp, &n);
                out["key_package"] = Hex(Take(kp, n));
            } else if (cmd == "peer_welcome") {
                std::vector<std::string> keep; std::vector<const char*> ids;
                for (auto& r : in["recognized"]) keep.push_back(r);
                for (auto& k : keep) ids.push_back(k.c_str());
                auto w = Unhex(in["welcome"].get<std::string>());
                auto r = daveSessionProcessWelcome(peer, w.data(), w.size(), ids.data(), ids.size());
                if (!r) throw std::runtime_error("welcome rejected: " + failure);
                daveWelcomeResultDestroy(r);
                enc = daveEncryptorCreate();
                auto kr = daveSessionGetKeyRatchet(peer, peer_user.c_str());
                daveEncryptorSetKeyRatchet(enc, kr); daveKeyRatchetDestroy(kr);
                dec = daveDecryptorCreate();
                auto kr2 = daveSessionGetKeyRatchet(peer, in.value("other", std::string()).c_str());
                daveDecryptorTransitionToKeyRatchet(dec, kr2); daveKeyRatchetDestroy(kr2);
            } else if (cmd == "peer_encrypt") {
                auto f = Unhex(in["frame"].get<std::string>());
                const uint32_t ssrc = in.value("ssrc", 1u);
                daveEncryptorAssignSsrcToCodec(enc, ssrc, DAVE_CODEC_OPUS);
                Bytes o(daveEncryptorGetMaxCiphertextByteSize(enc, DAVE_MEDIA_TYPE_AUDIO, f.size())); size_t n = 0;
                if (daveEncryptorEncrypt(enc, DAVE_MEDIA_TYPE_AUDIO, ssrc, f.data(), f.size(), o.data(), o.size(), &n) != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS)
                    throw std::runtime_error("encrypt failed");
                o.resize(n); out["frame"] = Hex(o);
            } else if (cmd == "peer_decrypt") {
                auto f = Unhex(in["frame"].get<std::string>());
                Bytes o(daveDecryptorGetMaxPlaintextByteSize(dec, DAVE_MEDIA_TYPE_AUDIO, f.size())); size_t n = 0;
                auto rc = daveDecryptorDecrypt(dec, DAVE_MEDIA_TYPE_AUDIO, f.data(), f.size(), o.data(), o.size(), &n);
                out["ok"] = rc == DAVE_DECRYPTOR_RESULT_CODE_SUCCESS;
                o.resize(n); out["frame"] = Hex(o);
            } else {
                out = {{"ok", false}, {"error", "unknown cmd " + cmd}};
            }
        } catch (const std::exception& e) {
            out = {{"ok", false}, {"error", e.what()}};
        }
        std::cout << out.dump() << std::endl;
    }
    return 0;
}
