#include "openclaw/channels/discord_commands.hpp"

#include <cctype>
#include <functional>
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
                {{{"type", 3}, {"name", "args"}, {"description", "Arguments"}, {"required", false}, {"autocomplete", true}}});
        out.push_back(c);
        if (out.size() >= 100) break;
    }
    // Message context menu (right-click → Apps → "Ask OpenClaw"): type 3, no description/options.
    out.push_back({{"name", kAskMenu}, {"type", 3}});
    // User context menu (right-click a member → Apps): type 2.
    out.push_back({{"name", kUserMenu}, {"type", 2}});
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
    if (d.value("type", 1) == 3) {  // MESSAGE context menu: the target message becomes the prompt
        const auto tid = d.value("target_id", std::string());
        const auto msgs = d.value("resolved", nlohmann::json::object()).value("messages", nlohmann::json::object());
        if (!msgs.contains(tid)) return "";
        const auto& m = msgs[tid];
        const auto author = m.value("author", nlohmann::json::object()).value("username", std::string("someone"));
        return "[Discord message from " + author + ", via \"" + d.value("name", std::string()) + "\"]\n" + m.value("content", std::string());
    }
    if (d.value("type", 1) == 2) {  // USER context menu: target user (+ guild member) becomes the subject
        const auto tid = d.value("target_id", std::string());
        const auto res = d.value("resolved", nlohmann::json::object());
        const auto users = res.value("users", nlohmann::json::object());
        if (!users.contains(tid)) return "";
        const auto& u = users[tid];
        std::string who = u.value("global_name", nlohmann::json()).is_string() ? u["global_name"].get<std::string>() : u.value("username", std::string("someone"));
        std::string extra = "username=" + u.value("username", std::string()) + ", id=" + tid;
        if (u.value("bot", false)) extra += ", bot";
        const auto members = res.value("members", nlohmann::json::object());
        if (members.contains(tid)) {
            const auto& m = members[tid];
            if (m.contains("nick") && m["nick"].is_string()) extra += ", nick=" + m["nick"].get<std::string>();
            if (m.contains("joined_at") && m["joined_at"].is_string()) extra += ", joined=" + m["joined_at"].get<std::string>();
            if (m.contains("roles") && m["roles"].is_array()) extra += ", roles=" + std::to_string(m["roles"].size());
        }
        return "[Discord user context menu \"" + d.value("name", std::string()) + "\" on " + who + " (" + extra + ")]\n"
               "Tell me what you know about this user from our conversations and memory.";
    }
    if (d.value("type", 1) != 1) return "";
    std::string text = "/" + d.value("name", "");
    if (text == "/") return "";
    CollectOptions(d.value("options", nlohmann::json::array()), text);
    return text;
}

std::optional<Focused> FocusedOption(const nlohmann::json& in) {
    if (in.value("type", 0) != 4 || !in.contains("data")) return std::nullopt;
    std::function<std::optional<Focused>(const nlohmann::json&)> walk = [&](const nlohmann::json& opts) -> std::optional<Focused> {
        if (!opts.is_array()) return std::nullopt;
        for (auto& o : opts) {
            if (o.value("focused", false)) {
                const auto& v = o.contains("value") ? o["value"] : nlohmann::json("");
                return Focused{in["data"].value("name", std::string()), o.value("name", std::string()), v.is_string() ? v.get<std::string>() : v.dump()};
            }
            if (o.contains("options")) if (auto f = walk(o["options"])) return f;
        }
        return std::nullopt;
    };
    return walk(in["data"].value("options", nlohmann::json::array()));
}

nlohmann::json Choices(const std::vector<std::string>& candidates, const std::string& prefix) {
    auto low = [](std::string x) { for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return x; };
    const auto p = low(prefix);
    nlohmann::json out = nlohmann::json::array();
    for (auto& c : candidates) {
        if (!p.empty() && low(c).find(p) == std::string::npos) continue;
        out.push_back({{"name", c.size() > 100 ? c.substr(0, 100) : c}, {"value", c.size() > 100 ? c.substr(0, 100) : c}});
        if (out.size() >= 25) break;  // Discord limit
    }
    return out;
}

}  // namespace openclaw::channels::discord_commands
