#include "openclaw/channels/slack.hpp"
#include "openclaw/channels/slack_interactive.hpp"
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
    socket_.SetInteractiveHandler([this](const nlohmann::json& p, const DeliverFn& d) { return HandleInteractive(p, d); });
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

    if (nlohmann::json ip; slack_interactive::ExtractPayload(body, &ip)) return HandleInteractive(ip, deliver);

    std::string text;
    std::string sender = "slack";
    std::string session = "slack-default";
    std::string conv, ctype, thread;
    if (typ == "event_callback" && body.contains("event")) {
        auto& ev = body["event"];
        auto et = ev.value("type", "");
        if (et == "message" || et == "app_mention") {
            text = ev.value("text", "");
            sender = ev.value("user", sender);
            auto ch = ev.value("channel", "");
            if (!ch.empty()) session = "slack-" + ch;
            conv = ch; ctype = ev.value("channel_type", "");
            auto tts = ev.value("thread_ts", ""); if (!tts.empty() && tts != ev.value("ts", "")) thread = tts;
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
    msg.explicit_key = body.contains("session_key");
    msg.chat_type = ctype == "im" ? "direct" : (ctype == "mpim" || ctype == "group") ? "group" : ctype == "channel" ? "channel" : "";
    msg.conversation_id = conv; msg.thread_id = thread;
    ++inbound_;
    std::string reply = deliver ? deliver(msg) : "";
    auto rendered = slack_interactive::RenderReply(reply);
    if (rendered.contains("blocks")) reply = rendered.value("text", reply);
    return {{"ok", true},
            {"message", rendered},
            {"channel", "slack"},
            {"session_key", msg.session_key},
            {"sender", msg.sender},
            {"reply", reply},
            {"inbound_total", inbound_},
            {"impl", "real"}};
}

nlohmann::json SlackAdapter::CallApi(const std::string& method, const nlohmann::json& body) {
    nlohmann::json entry{{"api", method}, {"body", body}, {"mode", "recorded"}};
    const auto token = Env("SLACK_BOT_TOKEN");
    if (!token.empty()) {
        auto base = Env("SLACK_API_BASE");
        if (base.empty()) base = "https://slack.com/api";
        auto r = HttpPostJson(base + "/" + method, body, {"Authorization: Bearer " + token});
        entry["mode"] = "live_http";
        entry["http"] = r;
        entry["delivered"] = r.value("http", 0) == 200 && r.contains("json") && r["json"].is_object() &&
                             r["json"].value("ok", false);
    }
    Record(entry);
    return entry;
}

nlohmann::json SlackAdapter::HandleInteractive(const nlohmann::json& payload, const DeliverFn& deliver) {
    auto it = slack_interactive::Parse(payload);
    nlohmann::json out{{"ok", true}, {"channel", "slack"}, {"interactive", it.kind}, {"impl", "real"}};
    if (!it.api_call.is_null()) out["api"] = CallApi(it.api_call["method"], it.api_call["body"]);
    if (it.inbound) {
        ++inbound_;
        std::string reply = deliver ? deliver(*it.inbound) : "";
        auto rendered = slack_interactive::RenderReply(reply);
        out["session_key"] = it.inbound->session_key;
        out["inbound_text"] = it.inbound->text;
        out["reply"] = rendered.value("text", reply);
        out["message"] = rendered;
        if (!reply.empty() && !it.inbound->conversation_id.empty()) {
            nlohmann::json extra{{"via", "slack-interactive"}};
            if (rendered.contains("blocks")) extra["blocks"] = rendered["blocks"];
            if (!it.inbound->thread_id.empty()) extra["thread_ts"] = it.inbound->thread_id;
            out["posted"] = SendText(it.inbound->conversation_id, rendered.value("text", reply), extra);
        }
    }
    if (it.ack_clear_errors) out["response_action"] = "clear";
    if (!it.inbound && it.api_call.is_null()) out["ignored"] = true;
    return out;
}

}  // namespace openclaw::channels
