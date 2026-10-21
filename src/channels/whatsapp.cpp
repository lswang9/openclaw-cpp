#include "openclaw/channels/whatsapp.hpp"

#include <cstdlib>

namespace openclaw::channels {
namespace {

std::string Env(const char* k) {
    if (const char* v = std::getenv(k); v && *v) return v;
    return {};
}

bool ForceMock() {
    const auto v = Env("WHATSAPP_FORCE_MOCK");
    if (v == "1" || v == "true" || v == "yes") return true;
    const auto c = Env("WHATSAPP_CLOUD_FORCE_MOCK");
    return c == "1" || c == "true" || c == "yes";
}

}  // namespace

WhatsAppCloudAdapter::WhatsAppCloudAdapter()
    : OutboundRecordAdapter("whatsapp",
                            "WhatsApp Cloud API (not Baileys); WHATSAPP_TOKEN + PHONE_NUMBER_ID",
                            "/v21.0/{phone_number_id}/messages") {
    SetEnabled(true);
}

bool WhatsAppCloudAdapter::VerifyTokenValue(const std::string& got) const {
    const auto expect = Env("WHATSAPP_VERIFY_TOKEN");
    if (expect.empty()) return true;  // dev
    return got == expect;
}

bool WhatsAppCloudAdapter::ExtractCloudMessage(const nlohmann::json& body, InboundMessage& out,
                                               std::string& note) {
    out.channel = "whatsapp";
    // Flat / test shapes
    if (body.contains("text") && body["text"].is_string()) {
        out.text = body.value("text", "");
        out.sender = body.value("from", body.value("sender", "whatsapp"));
        out.session_key = body.value("session_key", "whatsapp-" + out.sender);
        out.explicit_key = body.contains("session_key");
        out.chat_type = body.value("chat_type", out.sender.find("@g.us") != std::string::npos ? "group" : "direct");
        if (out.chat_type == "group") out.conversation_id = body.value("group_id", out.sender);
        return !out.text.empty();
    }

    if (!body.contains("entry") || !body["entry"].is_array() || body["entry"].empty()) {
        note = "no entry";
        return false;
    }
    try {
        const auto& e0 = body["entry"][0];
        if (!e0.contains("changes") || !e0["changes"].is_array() || e0["changes"].empty()) {
            note = "no changes";
            return false;
        }
        const auto value = e0["changes"][0].value("value", nlohmann::json::object());
        // Status-only webhooks
        if (value.contains("statuses") && (!value.contains("messages") || value["messages"].empty())) {
            note = "status webhook";
            return false;
        }
        if (!value.contains("messages") || !value["messages"].is_array() || value["messages"].empty()) {
            note = "no messages";
            return false;
        }
        const auto& m0 = value["messages"][0];
        out.sender = m0.value("from", "whatsapp");
        if (value.contains("contacts") && value["contacts"].is_array() && !value["contacts"].empty()) {
            out.sender = value["contacts"][0].value("wa_id", out.sender);
        }
        const auto typ = m0.value("type", "text");
        if (typ == "text" && m0.contains("text")) {
            out.text = m0["text"].value("body", "");
        } else if (typ == "button" && m0.contains("button")) {
            out.text = m0["button"].value("text", m0["button"].value("payload", ""));
        } else if (typ == "interactive" && m0.contains("interactive")) {
            auto& inter = m0["interactive"];
            if (inter.contains("button_reply"))
                out.text = inter["button_reply"].value("title", inter["button_reply"].value("id", ""));
            else if (inter.contains("list_reply"))
                out.text = inter["list_reply"].value("title", inter["list_reply"].value("id", ""));
        } else if (typ == "image" || typ == "audio" || typ == "video" || typ == "document") {
            out.text = "[" + typ + " id=" + m0.value(typ, nlohmann::json::object()).value("id", "") + "]";
            if (m0.contains(typ) && m0[typ].contains("caption"))
                out.text += " " + m0[typ].value("caption", "");
        } else {
            note = "unsupported type " + typ;
            return false;
        }
        out.session_key = "whatsapp-" + out.sender;
        out.chat_type = "direct";  // Cloud API delivers 1:1 conversations
        return !out.text.empty();
    } catch (...) {
        note = "parse error";
        return false;
    }
}

nlohmann::json WhatsAppCloudAdapter::Status() const {
    auto s = OutboundRecordAdapter::Status();
    s["type"] = "whatsapp_cloud";
    s["inbound"] = inbound_;
    s["fed"] = fed_;
    s["inbound_path"] = "/api/channels/whatsapp/inbound";
    s["verify_token_set"] = !Env("WHATSAPP_VERIFY_TOKEN").empty();
    s["token_set"] = !Env("WHATSAPP_TOKEN").empty() || !Env("WHATSAPP_ACCESS_TOKEN").empty();
    s["phone_number_id_set"] = !Env("WHATSAPP_PHONE_NUMBER_ID").empty();
    s["force_mock"] = ForceMock();
    s["baileys"] = false;
    s["gaps"] = nlohmann::json::array({"no Baileys / multi-device",
                                       "no media download/upload pipeline",
                                       "no template message builder UI",
                                       "text (+ button/list caption markers) only"});
    s["impl"] = (Env("WHATSAPP_TOKEN").empty() && Env("WHATSAPP_ACCESS_TOKEN").empty()) ? "honest"
                                                                                        : "real";
    return s;
}

nlohmann::json WhatsAppCloudAdapter::HandleVerifyGet(const std::string& verify_token,
                                                     const std::string& challenge) const {
    if (!VerifyTokenValue(verify_token)) {
        return {{"ok", false}, {"error", "verify token mismatch"}, {"impl", "real"}};
    }
    return {{"ok", true},
            {"challenge", challenge},
            {"mode", "url_verification"},
            {"impl", "real"},
            {"raw_challenge", true}};
}

nlohmann::json WhatsAppCloudAdapter::HandleHttpInbound(const nlohmann::json& body,
                                                       const DeliverFn& deliver) {
    if (!Enabled()) return {{"ok", false}, {"error", "adapter disabled"}};

    // Challenge in JSON body (tests / some proxies)
    if (body.contains("hub.challenge") ||
        (body.contains("challenge") && body.value("hub.mode", body.value("mode", "")) == "subscribe")) {
        auto token = body.value("hub.verify_token", body.value("verify_token", ""));
        auto ch = body.value("hub.challenge", body.value("challenge", ""));
        return HandleVerifyGet(token, ch);
    }
    if (body.contains("challenge") && body.contains("hub.verify_token")) {
        return HandleVerifyGet(body.value("hub.verify_token", ""), body.value("challenge", ""));
    }

    InboundMessage msg;
    std::string note;
    if (!ExtractCloudMessage(body, msg, note)) {
        return {{"ok", true}, {"ignored", true}, {"note", note.empty() ? "no message text" : note},
                {"impl", "real"}};
    }

    ++inbound_;
    std::string reply = deliver ? deliver(msg) : "";

    // Auto-reply outbound when tokens present and not FORCE_MOCK
    nlohmann::json outbound;
    if (!ForceMock() && !reply.empty() && !msg.sender.empty()) {
        outbound = SendText(msg.sender, reply, {{"via", "whatsapp-cloud-inbound"}});
    } else if (!reply.empty()) {
        outbound = {{"recorded", true}, {"mode", "mock_or_no_token"}, {"to", msg.sender}, {"text", reply}};
    }

    return {{"ok", true},
            {"channel", "whatsapp"},
            {"session_key", msg.session_key},
            {"sender", msg.sender},
            {"reply", reply},
            {"outbound", outbound},
            {"inbound_total", inbound_},
            {"impl", "real"}};
}

nlohmann::json WhatsAppCloudAdapter::FeedPayload(const nlohmann::json& payload,
                                                 const DeliverFn& deliver) {
    ++fed_;
    return HandleHttpInbound(payload, deliver);
}

nlohmann::json WhatsAppCloudAdapter::RunMockFrames(const std::vector<nlohmann::json>& frames,
                                                   const DeliverFn& deliver) {
    nlohmann::json results = nlohmann::json::array();
    for (const auto& f : frames) results.push_back(FeedPayload(f, deliver));
    return {{"ok", true},
            {"impl", "real-sim"},
            {"mode", "mock"},
            {"frames", frames.size()},
            {"results", results},
            {"status", Status()}};
}

}  // namespace openclaw::channels
