#include "openclaw/acp/session.hpp"

#include <cstdlib>

namespace openclaw::acp {
namespace {

bool EnvTruthy(const char* k, bool default_val = false) {
    const char* v = std::getenv(k);
    if (!v || !*v) return default_val;
    const std::string s(v);
    if (s == "0" || s == "false" || s == "no") return false;
    if (s == "1" || s == "true" || s == "yes") return true;
    return default_val;
}

}  // namespace

Registry::Registry() : endpoint_(std::make_shared<AgentEndpoint>()) {}

bool Registry::AutoBindEnabled() const {
    return EnvTruthy("OPENCLAW_ACP_AUTO_BIND", true);
}

bool Registry::IsBindableChannel(const std::string& channel) const {
    return channel == "slack" || channel == "feishu" || channel == "discord";
}

std::string Registry::WireIdFor(const Session& s) const {
    for (const auto& p : s.prompts) {
        if (p.rfind("wire:", 0) == 0) return p.substr(5);
    }
    return {};
}

std::string Registry::Create(const std::string& agent_id) {
    Session s;
    s.id = "acp-" + std::to_string(seq_++);
    s.agent_id = agent_id.empty() ? "openclaw" : agent_id;
    s.status = "idle";
    sessions_.push_back(s);
    auto wr = endpoint_->NewSession({{"cwd", "."}});
    if (wr.contains("sessionId")) {
        sessions_.back().prompts.push_back(std::string("wire:") + wr["sessionId"].get<std::string>());
    }
    return s.id;
}

std::string Registry::AppendPrompt(const std::string& id, const std::string& prompt) {
    for (auto& s : sessions_) {
        if (s.id != id) continue;
        s.prompts.push_back(prompt);
        std::string wire_id = WireIdFor(s);
        if (wire_id.empty()) {
            s.status = "idle";
            return {};
        }
        auto blocks = nlohmann::json::array({{{"type", "text"}, {"text", prompt}}});
        auto r = endpoint_->Prompt(wire_id, blocks);
        if (r.contains("error")) {
            s.status = "error";
            return std::string("error: ") + r.value("error", "prompt failed");
        }
        s.status = "idle";
        if (r.contains("text") && r["text"].is_string()) return r["text"].get<std::string>();
        if (r.contains("_meta") && r["_meta"].contains("openclawCpp")) {
            return r["_meta"]["openclawCpp"].value("text", "");
        }
        return {};
    }
    return {};
}

nlohmann::json Registry::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : sessions_) {
        arr.push_back({{"id", s.id},
                       {"agentId", s.agent_id},
                       {"status", s.status},
                       {"prompts", s.prompts.size()}});
    }
    return arr;
}

nlohmann::json Registry::GetJson(const std::string& id) const {
    for (const auto& s : sessions_) {
        if (s.id == id) {
            return {{"id", s.id},
                    {"agentId", s.agent_id},
                    {"status", s.status},
                    {"prompts", s.prompts}};
        }
    }
    return {{"error", "not found"}, {"id", id}};
}

nlohmann::json Registry::StatusJson() const {
    auto st = endpoint_->StatusJson();
    st["skeleton"] = false;
    st["implemented"] = true;
    st["gateway_sessions"] = ListJson();
    st["wire_sessions"] = endpoint_->ListSessionsJson();
    st["bindings"] = bindings_.StatusJson();
    st["channel_bound"] = true;
    st["auto_bind"] = AutoBindEnabled();
    return st;
}

nlohmann::json Registry::WireHandle(const nlohmann::json& request) {
    return endpoint_->Handle(request);
}

nlohmann::json Registry::AgentsListJson() const {
    return nlohmann::json::array({
        {{"id", "openclaw-cpp"},
         {"name", "openclaw-cpp"},
         {"title", "openclaw-cpp ACP agent"},
         {"protocolVersion", kAcpProtocolVersion},
         {"transport", nlohmann::json::array({"in-process", "stdio", "channel-bound"})},
         {"impl", "real"},
         {"note", "session/prompt → AgentLoop; channel bindings for Slack/Feishu/Discord"}},
    });
}

nlohmann::json Registry::BindChannel(const std::string& channel, const std::string& chat_id,
                                     const std::string& acp_session_id) {
    if (channel.empty() || chat_id.empty()) {
        return {{"ok", false}, {"error", "channel and chat_id required"}};
    }
    std::string sid = acp_session_id;
    if (sid.empty()) {
        sid = Create("openclaw");
    } else {
        bool found = false;
        for (const auto& s : sessions_) {
            if (s.id == sid) {
                found = true;
                break;
            }
        }
        if (!found) return {{"ok", false}, {"error", "acp session not found"}, {"id", sid}};
    }
    std::string wire_id;
    for (const auto& s : sessions_) {
        if (s.id == sid) {
            wire_id = WireIdFor(s);
            break;
        }
    }
    ChannelBinding b;
    b.channel = channel;
    b.chat_id = chat_id;
    b.channel_key = BindingStore::MakeKey(channel, chat_id);
    b.acp_session_id = sid;
    b.wire_session_id = wire_id;
    b.auto_created = false;
    bindings_.Bind(b);
    return {{"ok", true},
            {"impl", "real"},
            {"binding",
             {{"channel_key", b.channel_key},
              {"channel", b.channel},
              {"chat_id", b.chat_id},
              {"acp_session_id", b.acp_session_id},
              {"wire_session_id", b.wire_session_id},
              {"auto_created", false}}}};
}

bool Registry::UnbindChannel(const std::string& channel_key) {
    return bindings_.Unbind(channel_key);
}

ChannelInboundResult Registry::HandleChannelInbound(const channels::InboundMessage& msg,
                                                    const OutboundFn& outbound) {
    ChannelInboundResult out;
    std::string channel, chat_id, channel_key;
    if (!BindingStore::ParseInbound(msg, channel, chat_id, channel_key)) return out;
    if (!IsBindableChannel(channel)) return out;

    auto existing = bindings_.Find(channel_key);
    if (!existing) {
        if (!AutoBindEnabled()) return out;
        auto created = BindChannel(channel, chat_id, "");
        if (!created.value("ok", false)) {
            out.detail = created;
            return out;
        }
        ChannelBinding b;
        b.channel = channel;
        b.chat_id = chat_id;
        b.channel_key = channel_key;
        b.acp_session_id = created["binding"].value("acp_session_id", "");
        b.wire_session_id = created["binding"].value("wire_session_id", "");
        b.auto_created = true;
        bindings_.Bind(b);
        existing = b;
    }

    out.handled = true;
    out.channel_key = channel_key;
    out.acp_session_id = existing->acp_session_id;

    std::string prompt = msg.text;
    if (!msg.sender.empty()) prompt = "[" + msg.sender + "] " + msg.text;

    out.reply = AppendPrompt(existing->acp_session_id, prompt);
    if (out.reply.empty()) out.reply = "(empty acp reply)";

    if (outbound) {
        out.outbound = outbound(channel, chat_id, out.reply);
    }
    out.detail = {{"acp_session_id", out.acp_session_id},
                  {"channel_key", channel_key},
                  {"wire_session_id", existing->wire_session_id},
                  {"auto_created", existing->auto_created},
                  {"impl", "real"}};
    return out;
}

}  // namespace openclaw::acp
