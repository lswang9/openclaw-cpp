#include "openclaw/agent/routing.hpp"

#include <cctype>

namespace openclaw::agent::routing {
namespace {
std::string Low(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string Str(const nlohmann::json& j, const char* k) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : "";
}
std::filesystem::path ExpandHome(const std::string& p) {
    if (!p.empty() && p[0] == '~') {
        const char* h = std::getenv("HOME");
        return std::filesystem::path(h ? h : "/tmp") / p.substr(p.size() > 1 && p[1] == '/' ? 2 : 1);
    }
    return p;
}
}  // namespace

std::vector<AgentDef> ListAgents(const nlohmann::json& raw) {
    std::vector<AgentDef> out;
    const nlohmann::json* list = nullptr;
    if (raw.contains("agents") && raw["agents"].is_object() && raw["agents"].contains("list") && raw["agents"]["list"].is_array())
        list = &raw["agents"]["list"];
    if (list) {
        for (auto& a : *list) {
            if (!a.is_object() || Str(a, "id").empty()) continue;
            AgentDef d;
            d.id = Low(Str(a, "id"));
            d.name = Str(a, "name").empty() ? d.id : Str(a, "name");
            if (!Str(a, "workspace").empty()) d.workspace = ExpandHome(Str(a, "workspace"));
            if (a.contains("model")) {
                if (a["model"].is_string()) d.model = a["model"];
                else d.model = Str(a["model"], "primary");
            }
            if (a.contains("identity") && a["identity"].is_object()) {
                auto nm = Str(a["identity"], "name");
                if (!nm.empty()) d.system_prompt = "Your name is " + nm + ".";
            }
            if (!Str(a, "systemPrompt").empty()) d.system_prompt += (d.system_prompt.empty() ? "" : "\n") + Str(a, "systemPrompt");
            d.is_default = a.value("default", false);
            out.push_back(std::move(d));
        }
    }
    if (out.empty()) out.push_back({"main", "main", {}, "", "", true});
    return out;
}

std::string DefaultAgentId(const nlohmann::json& raw) {
    auto agents = ListAgents(raw);
    for (auto& a : agents) if (a.is_default) return a.id;
    return agents.front().id;
}

const AgentDef* FindAgent(const std::vector<AgentDef>& agents, const std::string& id) {
    for (auto& a : agents) if (a.id == Low(id)) return &a;
    return nullptr;
}

RouteResult ResolveRoute(const nlohmann::json& raw, const RouteInput& in) {
    RouteResult best{DefaultAgentId(raw), "default", -1};
    if (!raw.contains("bindings") || !raw["bindings"].is_array()) return best;
    const auto agents = ListAgents(raw);
    int best_score = -1;
    int idx = -1;
    for (auto& b : raw["bindings"]) {
        ++idx;
        if (!b.is_object()) continue;
        const auto aid = Low(Str(b, "agentId"));
        if (aid.empty() || !FindAgent(agents, aid)) continue;
        const auto m = b.value("match", nlohmann::json::object());
        const auto ch = Low(Str(m, "channel"));
        if (!ch.empty() && ch != "*" && ch != Low(in.channel)) continue;
        int score = 0;
        std::string by = "binding.channel";
        const auto acc = Str(m, "accountId");
        if (!acc.empty() && acc != "*") {
            if (acc != in.account_id) continue;
            score = 1; by = "binding.account";
        } else if (acc.empty() && !in.account_id.empty() && in.account_id != "default") {
            continue;  // upstream: missing accountId matches only the default account
        }
        if (auto team = Str(m, "teamId"); !team.empty()) {
            if (team != in.team_id) continue;
            score = 2; by = "binding.team";
        }
        if (auto g = Str(m, "guildId"); !g.empty()) {
            if (g != in.guild_id) continue;
            score = 3; by = "binding.guild";
        }
        if (m.contains("peer") && m["peer"].is_object()) {
            auto kind = Low(Str(m["peer"], "kind")), id = Str(m["peer"], "id");
            if (kind == "dm") kind = "direct";
            if (!kind.empty() && kind != in.peer_kind) continue;
            if (!id.empty() && id != in.peer_id) continue;
            score = 4; by = "binding.peer";
        }
        if (score > best_score) { best_score = score; best = {aid, by, idx}; }
    }
    return best;
}

std::string AgentFromSessionKey(const std::string& key) {
    if (key.rfind("agent:", 0) != 0) return "";
    auto end = key.find(':', 6);
    return Low(key.substr(6, end == std::string::npos ? std::string::npos : end - 6));
}

}  // namespace openclaw::agent::routing
