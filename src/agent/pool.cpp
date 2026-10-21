#include "openclaw/agent/pool.hpp"

#include "openclaw/workspace/embeddings.hpp"
#include "openclaw/workspace/workspace.hpp"

#include <cstdlib>

namespace openclaw::agent {
namespace fs = std::filesystem;

std::filesystem::path AgentsRoot() {
    if (const char* p = std::getenv("OPENCLAW_AGENTS_DIR"); p && *p) return p;
    if (const char* p = std::getenv("OPENCLAW_STATE_DIR"); p && *p) return fs::path(p) / "agents";
    if (const char* p = std::getenv("OPENCLAW_DATA_DIR"); p && *p) return fs::path(p) / "agents";
    if (const char* p = std::getenv("OPENCLAW_SESSION_DIR"); p && *p) {
        // sibling of the legacy flat sessions dir
        return fs::path(p).parent_path() / "agents";
    }
    const char* h = std::getenv("HOME");
    return fs::path(h ? h : "/tmp") / ".openclaw" / "agents";
}

namespace {
std::string Low(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
void ReadToolPolicy(const nlohmann::json& tools, ToolPolicy& out) {
    if (!tools.is_object()) return;
    if (tools.contains("profile") && tools["profile"].is_string()) out.profile = tools["profile"];
    auto arr = [&](const char* k, std::vector<std::string>& dst) {
        if (!tools.contains(k) || !tools[k].is_array()) return;
        for (auto& x : tools[k]) if (x.is_string()) dst.push_back(x.get<std::string>());
    };
    arr("allow", out.allow);
    arr("deny", out.deny);
}
bool MatchPat(const std::string& pat, const std::string& name) {
    if (pat.empty()) return false;
    if (pat.back() == '*') return name.rfind(pat.substr(0, pat.size() - 1), 0) == 0;
    return Low(pat) == Low(name);
}
std::vector<std::string> ProfileAllow(const std::string& profile) {
    const auto p = Low(profile);
    if (p == "minimal") return {"read", "memory_search", "memory_get", "memory_set"};
    if (p == "messaging") return {"read", "memory_search", "memory_get", "memory_set", "http_get"};
    if (p == "coding")
        return {"read", "write", "edit", "exec", "shell", "memory_search", "memory_get", "memory_set", "http_get",
                "browser_*"};
    return {};  // full / unknown → unrestricted before deny
}
}  // namespace

AgentIsolation ResolveIsolation(const nlohmann::json& raw, const std::string& agent_id_in) {
    const std::string id = Low(agent_id_in.empty() ? "main" : agent_id_in);
    AgentIsolation iso;
    auto agents = routing::ListAgents(raw);
    if (auto* ad = routing::FindAgent(agents, id)) iso.def = *ad;
    else iso.def = {id, id, {}, "", "", id == routing::DefaultAgentId(raw)};

    // Global tools.* then agents.list[].tools / agents.entries.<id>.tools
    if (raw.contains("tools") && raw["tools"].is_object()) ReadToolPolicy(raw["tools"], iso.tools);
    if (raw.contains("agents") && raw["agents"].is_object()) {
        const auto& ag = raw["agents"];
        if (ag.contains("defaults") && ag["defaults"].is_object()) {
            if (ag["defaults"].contains("tools")) ReadToolPolicy(ag["defaults"]["tools"], iso.tools);
            if (ag["defaults"].contains("sandbox") && ag["defaults"]["sandbox"].is_object())
                iso.sandbox = ag["defaults"]["sandbox"];
        }
        auto apply = [&](const nlohmann::json& a) {
            if (a.contains("tools")) ReadToolPolicy(a["tools"], iso.tools);
            if (a.contains("sandbox") && a["sandbox"].is_object())
                for (auto& [k, v] : a["sandbox"].items()) iso.sandbox[k] = v;
        };
        if (ag.contains("list") && ag["list"].is_array())
            for (auto& a : ag["list"]) if (a.is_object() && Low(a.value("id", "")) == id) apply(a);
        if (ag.contains("entries") && ag["entries"].is_object() && ag["entries"].contains(id)) apply(ag["entries"][id]);
    }

    const auto root = AgentsRoot() / id;
    // main keeps OPENCLAW_SESSION_DIR when set (smoke / legacy); other agents always under AgentsRoot.
    if (id == "main") {
        if (const char* p = std::getenv("OPENCLAW_SESSION_DIR"); p && *p) iso.sessions_root = p;
        else iso.sessions_root = root / "sessions";
    } else iso.sessions_root = root / "sessions";

    if (!iso.def.workspace.empty()) iso.workspace_root = iso.def.workspace;
    else if (id == "main") iso.workspace_root = workspace::Workspace::DefaultRoot();
    else iso.workspace_root = root / "workspace";

    if (id == "main") {
        if (const char* d = std::getenv("OPENCLAW_DATA_DIR"); d && *d)
            iso.memory_index = fs::path(d) / "memory" / "main.index.json";
        else iso.memory_index = root / "memory" / "index.json";
    } else iso.memory_index = root / "memory" / "index.json";

    iso.agent_files_root = root / "agent";
    return iso;
}

bool ToolAllowed(const ToolPolicy& p, const std::string& name) {
    for (auto& d : p.deny) if (MatchPat(d, name)) return false;
    auto base = ProfileAllow(p.profile);
    // coding profile uses browser_* sentinel
    auto in_list = [&](const std::vector<std::string>& list) {
        if (list.empty()) return true;
        for (auto& a : list) {
            if (a == "browser_*" && name.rfind("browser_", 0) == 0) return true;
            if (MatchPat(a, name)) return true;
        }
        return false;
    };
    if (!base.empty() && !in_list(base)) return false;
    if (!p.allow.empty() && !in_list(p.allow)) return false;
    return true;
}

tools::Registry FilterTools(const tools::Registry& base, const ToolPolicy& p) {
    tools::Registry out;
    for (auto& n : base.Names()) {
        if (!ToolAllowed(p, n)) continue;
        if (auto* t = base.Get(n)) out.Register(*t);
    }
    return out;
}

Pool::Pool(tools::Registry* base_tools, approvals::Store* approvals) : base_(base_tools), approvals_(approvals) {}

void Pool::SetConfig(nlohmann::json raw) {
    std::lock_guard l(mu_);
    raw_ = std::move(raw);
    // Drop tool registries so the next Get() rebuilds with the new policy/workspace; keep session stores.
    for (auto& [id, rt] : runtimes_) {
        rt->iso = ResolveIsolation(raw_, id);
        rt->tools.reset();
    }
}

AgentRuntime& Pool::Get(const std::string& agent_id_in) {
    const std::string id = Low(agent_id_in.empty() ? "main" : agent_id_in);
    std::lock_guard l(mu_);
    auto it = runtimes_.find(id);
    if (it == runtimes_.end()) {
        auto rt = std::make_unique<AgentRuntime>();
        rt->iso = ResolveIsolation(raw_, id);
        std::error_code ec;
        fs::create_directories(rt->iso.sessions_root, ec);
        fs::create_directories(rt->iso.workspace_root, ec);
        fs::create_directories(rt->iso.memory_index.parent_path(), ec);
        fs::create_directories(rt->iso.agent_files_root, ec);
        rt->sessions = std::make_unique<session::Store>(rt->iso.sessions_root);
        it = runtimes_.emplace(id, std::move(rt)).first;
    }
    auto& rt = *it->second;
    if (!rt.tools) {
        tools::RegistryOptions o;
        o.approvals = approvals_;
        o.workspace_root = rt.iso.workspace_root.string();
        o.memory_index_file = rt.iso.memory_index.string();
        o.embedder_from_config = true;
        // Build a fresh registry bound to this agent's workspace + memory index, then apply allow/deny.
        auto full = tools::MakeDefaultRegistry(o);
        if (base_) {
            // Keep any extra tools (MCP/JS) from the shared base that aren't file/memory/exec.
            for (auto& n : base_->Names()) {
                if (full.Has(n)) continue;
                if (auto* t = base_->Get(n)) full.Register(*t);
            }
        }
        rt.tools = std::make_shared<tools::Registry>(FilterTools(full, rt.iso.tools));
        // Re-bind exec/shell to this agent's workspace (MakeDefaultRegistry already did via opts).
    }
    return rt;
}

std::shared_ptr<tools::Registry> Pool::Tools(const std::string& agent_id) {
    return Get(agent_id).tools;
}
session::Store& Pool::Sessions(const std::string& agent_id) {
    return *Get(agent_id).sessions;
}

std::vector<std::string> Pool::Ids() const {
    std::lock_guard l(mu_);
    std::vector<std::string> out;
    for (auto& [id, _] : runtimes_) out.push_back(id);
    return out;
}

}  // namespace openclaw::agent
