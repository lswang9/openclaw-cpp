#pragma once
// Discord application (slash) commands: spec building, interaction parsing, sync.
#include <nlohmann/json.hpp>
#include <optional>
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
inline constexpr const char* kAskMenu = "Ask OpenClaw";
inline constexpr const char* kUserMenu = "Ask OpenClaw about user";  // USER context menu (type 2), <=32 chars
// APPLICATION_COMMAND_AUTOCOMPLETE (type 4): the focused option and its partial value.
struct Focused { std::string command, option, value; };
std::optional<Focused> FocusedOption(const nlohmann::json& interaction);
// Up to 25 {name,value} choices containing `prefix` (case-insensitive).
nlohmann::json Choices(const std::vector<std::string>& candidates, const std::string& prefix);

}  // namespace openclaw::channels::discord_commands
