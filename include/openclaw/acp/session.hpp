#pragma once

#include "openclaw/acp/bindings.hpp"
#include "openclaw/acp/wire.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace openclaw::acp {

struct Session {
    std::string id;
    std::string agent_id;
    std::string status;
    std::vector<std::string> prompts;
};

struct ChannelInboundResult {
    bool handled = false;
    std::string reply;
    std::string acp_session_id;
    std::string channel_key;
    nlohmann::json outbound = nlohmann::json::object();
    nlohmann::json detail = nlohmann::json::object();
};

using OutboundFn = std::function<nlohmann::json(const std::string& channel, const std::string& chat_id,
                                                const std::string& text)>;

class Registry {
public:
    Registry();

    std::string Create(const std::string& agent_id);
    // Returns assistant text (empty on failure).
    std::string AppendPrompt(const std::string& id, const std::string& prompt);
    nlohmann::json ListJson() const;
    nlohmann::json StatusJson() const;
    nlohmann::json GetJson(const std::string& id) const;

    AgentEndpoint& Endpoint() { return *endpoint_; }
    const AgentEndpoint& Endpoint() const { return *endpoint_; }
    nlohmann::json WireHandle(const nlohmann::json& request);
    nlohmann::json AgentsListJson() const;

    BindingStore& Bindings() { return bindings_; }
    const BindingStore& Bindings() const { return bindings_; }

    nlohmann::json BindChannel(const std::string& channel, const std::string& chat_id,
                               const std::string& acp_session_id = "");
    bool UnbindChannel(const std::string& channel_key);

    ChannelInboundResult HandleChannelInbound(const channels::InboundMessage& msg,
                                              const OutboundFn& outbound = {});

private:
    std::string WireIdFor(const Session& s) const;
    bool AutoBindEnabled() const;
    bool IsBindableChannel(const std::string& channel) const;

    std::vector<Session> sessions_;
    int seq_ = 1;
    std::shared_ptr<AgentEndpoint> endpoint_;
    BindingStore bindings_;
};

}  // namespace openclaw::acp
