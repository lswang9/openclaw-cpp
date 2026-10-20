#include "openclaw/channels/feishu.hpp"

#include <cstdlib>

namespace openclaw::channels {
namespace {
std::string Env(const char* k) {
    if (const char* v = std::getenv(k); v && *v) return v;
    return {};
}
}  // namespace

FeishuAdapter::FeishuAdapter()
    : OutboundRecordAdapter("feishu",
                            "Feishu outbound + webhook inbound + long-connection WS",
                            "/open-apis/im/v1/messages") {
    SetEnabled(true);
}

bool FeishuAdapter::VerifyToken(const nlohmann::json& body) const {
    const auto expect = Env("FEISHU_VERIFY_TOKEN");
    if (expect.empty()) return true;  // no token configured → accept (dev)
    auto got = body.value("token", "");
    if (got.empty() && body.contains("header")) got = body["header"].value("token", "");
    return got == expect;
}

std::string FeishuAdapter::ExtractText(const nlohmann::json& body) {
    // url_verification has no text
    if (body.value("type", "") == "url_verification") return {};
    // flat
    if (body.contains("text")) return body.value("text", "");
    // event v2: event.message.content is JSON string {"text":"..."}
    const nlohmann::json* ev = nullptr;
    if (body.contains("event") && body["event"].is_object()) ev = &body["event"];
    if (!ev) return {};
    if (ev->contains("message") && (*ev)["message"].is_object()) {
        auto content = (*ev)["message"].value("content", "");
        if (!content.empty()) {
            try {
                auto c = nlohmann::json::parse(content);
                if (c.contains("text")) return c.value("text", "");
            } catch (...) {
                return content;
            }
        }
        if ((*ev)["message"].contains("text")) return (*ev)["message"].value("text", "");
    }
    // interactive card / legacy
    if (ev->contains("text")) return (*ev).value("text", "");
    return {};
}

nlohmann::json FeishuAdapter::Status() const {
    auto s = OutboundRecordAdapter::Status();
    s["type"] = "feishu";
    s["inbound"] = inbound_;
    s["inbound_path"] = "/api/channels/feishu/inbound";
    s["verify_token_set"] = !Env("FEISHU_VERIFY_TOKEN").empty();
    s["app_id_set"] = !Env("FEISHU_APP_ID").empty();
    s["longconn"] = longconn_.Status();
    return s;
}

void FeishuAdapter::Start(DeliverFn deliver) {
    SetEnabled(true);
    (void)longconn_.Start(std::move(deliver));
}

void FeishuAdapter::Stop() { longconn_.Stop(); }

nlohmann::json FeishuAdapter::HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) {
    if (!Enabled()) return {{"ok", false}, {"error", "adapter disabled"}};
    if (!VerifyToken(body)) return {{"ok", false}, {"error", "verify token mismatch"}, {"impl", "real"}};

    // Feishu URL verification challenge
    if (body.value("type", "") == "url_verification" || body.contains("challenge")) {
        return {{"ok", true},
                {"challenge", body.value("challenge", "")},
                {"impl", "real"},
                {"mode", "url_verification"}};
    }

    std::string text = ExtractText(body);
    if (text.empty()) {
        return {{"ok", true},
                {"ignored", true},
                {"note", "no message text (ack only)"},
                {"impl", "real"}};
    }

    InboundMessage msg;
    msg.channel = "feishu";
    msg.text = text;
    msg.sender = "feishu";
    if (body.contains("event") && body["event"].contains("sender")) {
        auto& s = body["event"]["sender"];
        if (s.contains("sender_id")) msg.sender = s["sender_id"].value("open_id", msg.sender);
    }
    msg.session_key = body.value("session_key", "feishu-default");
    if (body.contains("event") && body["event"].contains("message")) {
        auto chat = body["event"]["message"].value("chat_id", "");
        if (!chat.empty()) msg.session_key = "feishu-" + chat;
    }

    ++inbound_;
    std::string reply;
    if (deliver) reply = deliver(msg);
    return {{"ok", true},
            {"channel", "feishu"},
            {"session_key", msg.session_key},
            {"sender", msg.sender},
            {"reply", reply},
            {"inbound_total", inbound_},
            {"impl", "real"}};
}

}  // namespace openclaw::channels
