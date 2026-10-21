#include "openclaw/channels/discord_commands.hpp"

#include <cctype>
#include <set>

namespace openclaw::channels::discord_commands {

std::vector<CommandDef> NativeCommands() {
    return {{"new", "Start a new session", false},
            {"reset", "Reset the current session", false},
            {"status", "Show session status", false},
            {"help", "Show available commands", false},
            {"compact", "Compact the session context", true},
            {"model", "Show or switch the model", true},
            {"stop", "Stop the current run", false}};
}

std::string SanitizeName(const std::string& n) {
    std::string o;
    for (char c : n) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) o += static_cast<char>(std::tolower(u));
        else if (c == '-' || c == '_') o += c;
        else if (c == ' ' || c == '.') o += '-';
    }
    if (o.size() > 32) o.resize(32);
    return o;
}

nlohmann::json BuildSpecs(const std::vector<CommandDef>& defs) {
    nlohmann::json out = nlohmann::json::array();
    std::set<std::string> seen;
    for (auto& d : defs) {
        auto name = SanitizeName(d.name);
        if (name.empty() || !seen.insert(name).second) continue;
        std::string desc = d.description.empty() ? ("Run /" + name) : d.description;
        if (desc.size() > 100) desc = desc.substr(0, 97) + "...";
        nlohmann::json c{{"name", name}, {"description", desc}, {"type", 1}};
        if (d.takes_args)
            c["options"] = nlohmann::json::array(
                {{{"type", 3}, {"name", "args"}, {"description", "Arguments"}, {"required", false}}});
        out.push_back(c);
        if (out.size() >= 100) break;
    }
    return out;
}

namespace {
void CollectOptions(const nlohmann::json& opts, std::string& out) {
    if (!opts.is_array()) return;
    for (auto& o : opts) {
        if (o.contains("options")) {  // subcommand / group
            out += " " + o.value("name", "");
            CollectOptions(o["options"], out);
        } else if (o.contains("value")) {
            const auto& v = o["value"];
            std::string s = v.is_string() ? v.get<std::string>() : v.dump();
            if (!s.empty()) out += " " + s;
        }
    }
}
}  // namespace

std::string InteractionText(const nlohmann::json& in) {
    if (in.value("type", 0) != 2 || !in.contains("data")) return "";
    const auto& d = in["data"];
    std::string text = "/" + d.value("name", "");
    if (text == "/") return "";
    CollectOptions(d.value("options", nlohmann::json::array()), text);
    return text;
}

}  // namespace openclaw::channels::discord_commands
