#include "openclaw/context/system_prompt.hpp"
#include <fstream>
#include <sstream>

namespace openclaw::context {
namespace fs = std::filesystem;

const std::vector<std::string>& BootstrapFileNames() {
    static const std::vector<std::string> k = {"AGENTS.md", "SOUL.md", "TOOLS.md", "IDENTITY.md",
                                               "USER.md", "HEARTBEAT.md", "BOOTSTRAP.md", "MEMORY.md"};
    return k;
}

namespace {
bool ReadCapped(const fs::path& p, std::size_t cap, std::string& out, bool& truncated, std::size_t& size) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    out = ss.str(); size = out.size(); truncated = false;
    if (out.size() > cap) {
        // keep head 70% + tail 20% (upstream trims middle with a marker)
        std::size_t head = cap * 7 / 10, tail = cap * 2 / 10;
        out = out.substr(0, head) + "\n…[truncated " + std::to_string(size - head - tail) + " chars]…\n" +
              out.substr(size - tail);
        truncated = true;
    }
    return true;
}
}  // namespace

std::string BuildSystemPrompt(const SystemPromptOptions& o) {
    std::ostringstream s;
    s << (o.base.empty() ? "You are a personal assistant running inside OpenClaw." : o.base) << "\n";
    if (!o.tool_names.empty()) {
        s << "\n## Tooling\nAvailable tools:";
        for (auto& t : o.tool_names) s << " " << t;
        s << "\n";
    }
    auto block = skills::FormatSkillsForPrompt(o.skills);
    if (!block.empty()) {
        s << "\n## Skills\nBefore replying, scan <available_skills>. If one clearly applies, read its SKILL.md at "
             "<location> with the `read` tool, then follow it. Never read more than one skill up front.\n"
          << block << "\n";
    }
    if (!o.workspace.empty()) {
        s << "\n## Workspace\nYour working directory is: " << o.workspace.string() << "\n";
    }
    if (o.include_bootstrap && !o.workspace.empty()) {
        bool header = false;
        for (const auto& n : BootstrapFileNames()) {
            std::string txt; bool tr; std::size_t sz;
            if (!ReadCapped(o.workspace / n, o.bootstrap_max_chars, txt, tr, sz)) continue;
            if (!header) { s << "\n# Project Context\n"; header = true; }
            s << "\n## " << n << "\n" << txt << "\n";
        }
    }
    return s.str();
}

nlohmann::json DescribeBootstrap(const fs::path& ws, std::size_t cap) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& n : BootstrapFileNames()) {
        std::string txt; bool tr = false; std::size_t sz = 0;
        bool ok = ReadCapped(ws / n, cap, txt, tr, sz);
        arr.push_back({{"name", n}, {"present", ok}, {"rawChars", sz}, {"injectedChars", ok ? txt.size() : 0},
                       {"truncated", tr}});
    }
    return {{"workspace", ws.string()}, {"files", arr}};
}

}  // namespace openclaw::context
