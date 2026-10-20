#include "openclaw/channels/slack.hpp"
#include <cstdlib>

namespace openclaw::channels {
namespace {
std::string Env(const char* k) {
    if (const char* v = std::getenv(k); v && *v) return v;
    return {};
}
}  // namespace

SlackAdapter::SlackAdapter()
    : OutboundRecordAdapter("slack",
                            "Slack Events API + Socket Mode + chat.postMessage when SLACK_BOT_TOKEN set",
                            "/api/chat.postMessage") {
    SetEnabled(true);
}

bool SlackAdapter::Verify(const nlohmann::json& body) const {
    const auto expect = Env("SLACK_VERIFY_TOKEN");
    if (expect.empty()) return true;
    auto got = body.value("token", "");
    return got == expect;
}

nlohmann::json SlackAdapter::Status() const {
    auto s = OutboundRecordAdapter::Status();
    s["type"] = "slack";
    s["inbound"] = inbound_;
    s["inbound_path"] = "/api/channels/slack/inbound";
    s["verify_token_set"] = !Env("SLACK_VERIFY_TOKEN").empty();
    s["socket_mode"] = socket_.Status();
    s["bot_token_set"] = !Env("SLACK_BOT_TOKEN").empty();
    return s;
}

void SlackAdapter::Start(DeliverFn deliver) {
    SetEnabled(true);
    (void)socket_.Start(std::move(deliver));
}

void SlackAdapter::Stop() { socket_.Stop(); }

nlohmann::json SlackAdapter::HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) {
    if (!Enabled()) return {{"ok", false}, {"error", "adapter disabled"}};
    if (!Verify(body)) return {{"ok", false}, {"error", "verify token mismatch"}, {"impl", "real"}};

    const auto typ = body.value("type", "");
    if (typ == "url_verification" || body.contains("challenge")) {
        return {{"ok", true},
                {"challenge", body.value("challenge", "")},
                {"impl", "real"},
                {"mode", "url_verification"}};
    }

    std::string text;
    std::string sender = "slack";
    std::string session = "slack-default";
    if (typ == "event_callback" && body.contains("event")) {
        auto& ev = body["event"];
        auto et = ev.value("type", "");
        if (et == "message" || et == "app_mention") {
            text = ev.value("text", "");
            sender = ev.value("user", sender);
            auto ch = ev.value("channel", "");
            if (!ch.empty()) session = "slack-" + ch;
        }
    }
    if (text.empty()) text = body.value("text", body.value("message", ""));
    if (text.empty()) {
        return {{"ok", true}, {"ignored", true}, {"note", "no message text"}, {"impl", "real"}};
    }

    InboundMessage msg;
    msg.channel = "slack";
    msg.text = text;
    msg.sender = sender;
    msg.session_key = body.value("session_key", session);
    ++inbound_;
    std::string reply = deliver ? deliver(msg) : "";
    return {{"ok", true},
            {"channel", "slack"},
            {"session_key", msg.session_key},
            {"sender", msg.sender},
            {"reply", reply},
            {"inbound_total", inbound_},
            {"impl", "real"}};
}

}  // namespace openclaw::channels
