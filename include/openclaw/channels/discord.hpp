#pragma once
#include <functional>
#include "openclaw/channels/discord_gateway.hpp"
#include "openclaw/channels/outbound.hpp"

namespace openclaw::channels {

// Discord: outbound (DISCORD_BOT_TOKEN) + inbound webhook/interactions + Gateway WS.
class DiscordAdapter : public OutboundRecordAdapter {
public:
    DiscordAdapter();
    nlohmann::json Status() const override;
    bool SupportsHttpInbound() const override { return true; }
    nlohmann::json HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) override;

    void Start(DeliverFn deliver) override;
    void Stop() override;

    // Slash commands. Specs are CHAT_INPUT JSON (discord_commands::BuildSpecs).
    void SetCommandSpecs(nlohmann::json specs) { specs_ = std::move(specs); }
    const nlohmann::json& CommandSpecs() const { return specs_; }
    // PUT /applications/{app}/commands (or guilds/{g}/commands when DISCORD_GUILD_ID).
    // Live when DISCORD_BOT_TOKEN + DISCORD_APPLICATION_ID; base via DISCORD_API_BASE; else recorded.
    nlohmann::json SyncCommands();
    // APPLICATION_COMMAND / component interaction. via_gateway → deferred ack (type 5) + PATCH @original.
    nlohmann::json HandleInteraction(const nlohmann::json& in, const DeliverFn& deliver, bool via_gateway);
    // Autocomplete candidates for (command, option, partial). Set by the gateway (models, skills, ...).
    using CompleteFn = std::function<std::vector<std::string>(const std::string&, const std::string&, const std::string&)>;
    void SetAutocomplete(CompleteFn fn) { complete_ = std::move(fn); }
    nlohmann::json Api(const std::string& method, const std::string& path, const nlohmann::json& body);

    // Voice ops: action join | leave | play | status. play: {guild_id, channel_id, audio_path | audio_b64 | text,
    // listen_ms?, transcribe?, leave?}. Endpoint/token/session come from the live gateway (op 4 → VOICE_*_UPDATE).
    nlohmann::json Voice(const nlohmann::json& params);

    DiscordGateway& Gateway() { return gateway_; }
    const DiscordGateway& Gateway() const { return gateway_; }

private:
    std::uint64_t inbound_ = 0;
    bool Verify(const nlohmann::json& body) const;
    static std::string ExtractText(const nlohmann::json& body);
    DiscordGateway gateway_;
    nlohmann::json specs_ = nlohmann::json::array();
    CompleteFn complete_;
};

}  // namespace openclaw::channels
