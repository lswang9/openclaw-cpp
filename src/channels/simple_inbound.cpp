#include "openclaw/channels/simple_inbound.hpp"
#include <cstdlib>

namespace openclaw::channels {
namespace {
std::string Env(const char* k) {
    if (const char* v = std::getenv(k); v && *v) return v;
    return {};
}
}  // namespace

SimpleInboundAdapter::SimpleInboundAdapter(std::string id, std::string verify_env, std::string note)
    : id_(std::move(id)), verify_env_(std::move(verify_env)), note_(std::move(note)) {}

bool SimpleInboundAdapter::Verify(const nlohmann::json& body) const {
    const auto expect = Env(verify_env_.c_str());
    if (expect.empty()) return true;
    auto got = body.value("token", body.value("verify_token", ""));
    if (got.empty()) got = body.value("hub.verify_token", "");
    return got == expect;
}

std::string SimpleInboundAdapter::ExtractText(const nlohmann::json& body) const {
    if (body.contains("text") && body["text"].is_string()) return body.value("text", "");
    if (body.contains("message")) {
        if (body["message"].is_string()) return body["message"].get<std::string>();
        if (body["message"].is_object()) {
            if (body["message"].contains("text")) {
                auto& t = body["message"]["text"];
                if (t.is_string()) return t.get<std::string>();
                if (t.is_object()) return t.value("body", t.value("text", ""));
            }
            return body["message"].value("body", "");
        }
    }
    // WhatsApp Cloud API entry.changes.value.messages[0].text.body
    if (body.contains("entry") && body["entry"].is_array() && !body["entry"].empty()) {
        auto& e0 = body["entry"][0];
        if (e0.contains("changes") && e0["changes"].is_array() && !e0["changes"].empty()) {
            nlohmann::json v = e0["changes"][0].value("value", nlohmann::json::object());
            if (v.contains("messages") && v["messages"].is_array() && !v["messages"].empty()) {
                auto& m0 = v["messages"][0];
                if (m0.contains("text")) return m0["text"].value("body", "");
                if (m0.contains("button")) return m0["button"].value("text", "");
            }
        }
    }
    if (body.contains("body") && body["body"].is_string()) return body.value("body", "");
    return {};
}

nlohmann::json SimpleInboundAdapter::Status() const {
    return {{"id", id_},
            {"enabled", enabled_},
            {"type", "simple_inbound"},
            {"note", note_},
            {"inbound", inbound_},
            {"inbound_path", "/api/channels/" + id_ + "/inbound"},
            {"verify_env", verify_env_},
            {"verify_token_set", !Env(verify_env_.c_str()).empty()}};
}

nlohmann::json SimpleInboundAdapter::HandleHttpInbound(const nlohmann::json& body,
                                                       const DeliverFn& deliver) {
    if (!enabled_) return {{"ok", false}, {"error", "adapter disabled"}};

    // Meta hub challenge (WhatsApp) GET-style fields sometimes POSTed in tests
    if (body.contains("hub.challenge") || body.contains("challenge")) {
        if (!Verify(body) && body.contains("hub.verify_token")) {
            return {{"ok", false}, {"error", "verify token mismatch"}, {"impl", "real"}};
        }
        auto ch = body.value("hub.challenge", body.value("challenge", ""));
        return {{"ok", true}, {"challenge", ch}, {"impl", "real"}, {"mode", "url_verification"}};
    }

    if (!Verify(body)) return {{"ok", false}, {"error", "verify token mismatch"}, {"impl", "real"}};

    std::string text = ExtractText(body);
    if (text.empty()) {
        return {{"ok", true}, {"ignored", true}, {"note", "no message text"}, {"impl", "real"}};
    }

    InboundMessage msg;
    msg.channel = id_;
    msg.text = text;
    msg.sender = body.value("from", body.value("sender", id_));
    // WhatsApp from field
    if (body.contains("entry")) {
        try {
            auto v = body["entry"][0]["changes"][0]["value"];
            if (v.contains("messages")) msg.sender = v["messages"][0].value("from", msg.sender);
            if (v.contains("contacts") && v["contacts"].is_array() && !v["contacts"].empty())
                msg.sender = v["contacts"][0].value("wa_id", msg.sender);
        } catch (...) {
        }
    }
    msg.session_key = body.value("session_key", id_ + "-" + msg.sender);
    ++inbound_;
    std::string reply = deliver ? deliver(msg) : "";
    return {{"ok", true},
            {"channel", id_},
            {"session_key", msg.session_key},
            {"sender", msg.sender},
            {"reply", reply},
            {"inbound_total", inbound_},
            {"impl", "real"}};
}

}  // namespace openclaw::channels
