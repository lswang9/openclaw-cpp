#pragma once
// Discord application (slash) commands: spec building, interaction parsing, sync.
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace openclaw::channels::discord_commands {

struct CommandDef {
    std::string name;
    std::string description;
    bool takes_args = true;
};

// Native chat commands mirrored from upstream (/new, /reset, /status, /help, /compact, /model, /stop).
std::vector<CommandDef> NativeCommands();
// Discord-safe name: lowercase, [a-z0-9_-], 1..32 chars ("" if impossible).
std::string SanitizeName(const std::string& n);
// CHAT_INPUT command JSON array (dedup by sanitized name, max 100, description <=100).
nlohmann::json BuildSpecs(const std::vector<CommandDef>& defs);
// Interaction (type 2 APPLICATION_COMMAND) -> "/name opt1 opt2" text; "" if not a command.
std::string InteractionText(const nlohmann::json& interaction);

}  // namespace openclaw::channels::discord_commands
