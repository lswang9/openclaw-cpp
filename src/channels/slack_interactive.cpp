#include "openclaw/channels/slack_interactive.hpp"

#include <regex>

namespace openclaw::channels::slack_interactive {
namespace {
std::string Trim(std::string s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(Trim(cur)); cur.clear(); } else cur += c;
    }
    out.push_back(Trim(cur));
    return out;
}
nlohmann::json PlainText(const std::string& t) { return {{"type", "plain_text"}, {"text", t.substr(0, 75)}}; }
std::string Slug(const std::string& s) {
    std::string o;
    for (char c : s) o += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(c)) : '_';
    return o.empty() ? "field" : o;
}
}  // namespace

nlohmann::json BuildModalView(const std::string& title, const std::vector<std::string>& fields,
                              const std::string& callback_id) {
    nlohmann::json blocks = nlohmann::json::array();
    for (auto& f : fields) {
        if (f.empty()) continue;
        blocks.push_back({{"type", "input"},
                          {"block_id", Slug(f)},
                          {"label", PlainText(f)},
                          {"element", {{"type", "plain_text_input"}, {"action_id", "value"}}}});
    }
    return {{"type", "modal"},
            {"callback_id", callback_id},
            {"private_metadata", title},
            {"title", PlainText(title.substr(0, 24))},
            {"submit", PlainText("Submit")},
            {"close", PlainText("Cancel")},
            {"blocks", blocks}};
}

nlohmann::json RenderReply(const std::string& reply) {
    static const std::regex re(R"(\[\[(buttons|select|modal):\s*([^\]]*)\]\])", std::regex::icase);
    nlohmann::json blocks = nlohmann::json::array();
    nlohmann::json actions = nlohmann::json::array();
    std::string text;
    auto begin = std::sregex_iterator(reply.begin(), reply.end(), re);
    std::size_t last = 0;
    int n = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        text += reply.substr(last, it->position() - last);
        last = it->position() + it->length();
        std::string kind = (*it)[1];
        for (auto& c : kind) c = static_cast<char>(std::tolower(c));
        const std::string spec = (*it)[2];
        if (kind == "buttons") {
            for (auto& item : Split(spec, ',')) {
                if (item.empty()) continue;
                auto bar = item.find('|');
                std::string label = Trim(item.substr(0, bar));
                std::string value = bar == std::string::npos ? label : Trim(item.substr(bar + 1));
                actions.push_back({{"type", "button"}, {"text", PlainText(label)},
                                   {"action_id", "openclaw_btn_" + std::to_string(n++)}, {"value", value}});
            }
        } else if (kind == "select") {
            auto bar = spec.find('|');
            std::string ph = Trim(spec.substr(0, bar));
            nlohmann::json opts = nlohmann::json::array();
            if (bar != std::string::npos)
                for (auto& o : Split(spec.substr(bar + 1), ','))
                    if (!o.empty()) opts.push_back({{"text", PlainText(o)}, {"value", o}});
            actions.push_back({{"type", "static_select"}, {"placeholder", PlainText(ph)},
                               {"action_id", "openclaw_select_" + std::to_string(n++)}, {"options", opts}});
        } else {  // modal
            auto bar = spec.find('|');
            std::string title = Trim(spec.substr(0, bar));
            std::vector<std::string> fields;
            if (bar != std::string::npos) fields = Split(spec.substr(bar + 1), ',');
            actions.push_back({{"type", "button"}, {"text", PlainText(title)}, {"action_id", "openclaw_modal"},
                               {"value", nlohmann::json{{"title", title}, {"fields", fields}}.dump()}});
        }
    }
    text += reply.substr(last);
    text = Trim(text);
    nlohmann::json out{{"text", text}};
    if (!actions.empty()) {
        if (!text.empty()) blocks.push_back({{"type", "section"}, {"text", {{"type", "mrkdwn"}, {"text", text}}}});
        blocks.push_back({{"type", "actions"}, {"elements", actions}});
        out["blocks"] = blocks;
    }
    return out;
}

bool ExtractPayload(const nlohmann::json& body, nlohmann::json* out) {
    nlohmann::json p;
    if (body.contains("payload")) {
        if (body["payload"].is_string()) {
            try { p = nlohmann::json::parse(body["payload"].get<std::string>()); } catch (...) { return false; }
        } else if (body["payload"].is_object()) p = body["payload"];
    } else p = body;
    if (!p.is_object()) return false;
    const auto t = p.value("type", "");
    if (t == "block_actions" || t == "view_submission" || t == "view_closed" || t == "shortcut" ||
        t == "message_action") {
        if (out) *out = p;
        return true;
    }
    return false;
}

Interaction Parse(const nlohmann::json& p) {
    Interaction r;
    r.kind = p.value("type", "");
    r.response_url = p.value("response_url", "");
    std::string user = p.contains("user") && p["user"].is_object() ? p["user"].value("id", "slack") : "slack";
    std::string ch;
    if (p.contains("channel") && p["channel"].is_object()) ch = p["channel"].value("id", "");
    std::string thread;
    if (p.contains("message") && p["message"].is_object()) thread = p["message"].value("thread_ts", "");
    auto mk = [&](std::string text) {
        InboundMessage m;
        m.channel = "slack";
        m.sender = user;
        m.text = std::move(text);
        m.conversation_id = ch;
        m.session_key = ch.empty() ? "slack-" + user : "slack-" + ch;
        if (ch.empty() || ch[0] == 'D') { m.chat_type = "direct"; m.peer_id = user; }
        else m.chat_type = "channel";
        m.thread_id = thread;
        return m;
    };
    if (r.kind == "block_actions" && p.contains("actions") && p["actions"].is_array() && !p["actions"].empty()) {
        const auto& a = p["actions"][0];
        const auto aid = a.value("action_id", "");
        if (aid == "openclaw_modal") {
            nlohmann::json spec;
            try { spec = nlohmann::json::parse(a.value("value", "{}")); } catch (...) { spec = nlohmann::json::object(); }
            std::vector<std::string> fields;
            for (auto& f : spec.value("fields", nlohmann::json::array())) if (f.is_string()) fields.push_back(f);
            auto view = BuildModalView(spec.value("title", std::string("Form")), fields);
            view["private_metadata"] = nlohmann::json{{"title", spec.value("title", std::string("Form"))}, {"channel", ch}}.dump();
            r.api_call = {{"method", "views.open"}, {"body", {{"trigger_id", p.value("trigger_id", "")}, {"view", view}}}};
            return r;
        }
        std::string label, value;
        if (a.value("type", "") == "static_select" && a.contains("selected_option")) {
            value = a["selected_option"].value("value", "");
            label = value;
        } else {
            value = a.value("value", "");
            label = a.contains("text") && a["text"].is_object() ? a["text"].value("text", value) : value;
        }
        r.inbound = mk("[Slack interaction] clicked \"" + label + "\" (value: " + value + ", action: " + aid + ")");
    } else if (r.kind == "view_submission" && p.contains("view")) {
        const auto& v = p["view"];
        std::string title = "form";
        try {
            auto md = nlohmann::json::parse(v.value("private_metadata", "{}"));
            title = md.value("title", title);
            ch = md.value("channel", ch);
        } catch (...) {
            if (!v.value("private_metadata", "").empty()) title = v.value("private_metadata", "");
        }
        std::string lines;
        if (v.contains("state") && v["state"].contains("values"))
            for (auto& [block, acts] : v["state"]["values"].items())
                for (auto& [aid, val] : acts.items()) {
                    std::string s = val.value("value", "");
                    if (val.contains("selected_option") && val["selected_option"].is_object())
                        s = val["selected_option"].value("value", s);
                    lines += "\n- " + block + ": " + s;
                }
        r.inbound = mk("[Slack modal submitted] " + title + lines);
        r.ack_clear_errors = true;
    } else if (r.kind == "shortcut" || r.kind == "message_action") {
        std::string txt = "[Slack shortcut] " + p.value("callback_id", "");
        if (p.contains("message") && p["message"].is_object()) txt += ": " + p["message"].value("text", "");
        r.inbound = mk(txt);
    }
    return r;
}

}  // namespace openclaw::channels::slack_interactive
