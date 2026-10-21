#include "openclaw/context/compaction.hpp"
#include "openclaw/context/assemble.hpp"
#include <cctype>
#include <sstream>

namespace openclaw::context {

std::size_t EstimateTokens(const std::vector<agent::Message>& msgs) {
    std::size_t chars = 0;
    for (const auto& m : msgs) {
        chars += m.content.size() + 16;
        if (!m.tool_calls.is_null()) chars += m.tool_calls.dump().size();
    }
    return chars / 4;
}

CompactionPolicy CompactionFromConfig(const nlohmann::json& raw) {
    CompactionPolicy p;
    try {
        if (!raw.is_object() || !raw.contains("agents") || !raw["agents"].contains("defaults")) return p;
        const auto& d = raw["agents"]["defaults"];
        if (d.contains("contextTokens") && d["contextTokens"].is_number_unsigned()) p.context_tokens = d["contextTokens"];
        if (d.contains("compaction") && d["compaction"].is_object()) {
            const auto& c = d["compaction"];
            p.mode = c.value("mode", p.mode);
            if (c.contains("reserveTokens") && c["reserveTokens"].is_number_unsigned()) p.reserve_tokens = c["reserveTokens"];
            if (c.contains("keepRecent") && c["keepRecent"].is_number_unsigned()) p.keep_recent = c["keepRecent"];
        }
    } catch (...) {}
    return p;
}

bool NeedsCompaction(const CompactionPolicy& p, const std::vector<agent::Message>& msgs) {
    if (p.mode == "off") return false;
    std::size_t limit = p.context_tokens > p.reserve_tokens ? p.context_tokens - p.reserve_tokens : p.context_tokens / 2;
    return EstimateTokens(msgs) > limit && msgs.size() > p.keep_recent + 1;
}

CompactionResult CompactWithSummary(provider::Provider& prov, const std::vector<agent::Message>& history,
                                    std::size_t keep_recent, const std::string& instructions) {
    CompactionResult r;
    r.before_tokens = EstimateTokens(history);
    std::size_t start = (!history.empty() && history.front().role == "system") ? 1 : 0;
    if (history.size() <= start + keep_recent) { r.messages = history; r.after_tokens = r.before_tokens; return r; }
    std::size_t cut = history.size() - keep_recent;
    // Don't start the kept tail on tool results (their assistant tool_call would be summarized away).
    while (cut > start && history[cut].role == "tool") --cut;
    if (cut <= start) { r.messages = history; r.after_tokens = r.before_tokens; return r; }

    std::ostringstream transcript;
    for (std::size_t i = start; i < cut; ++i) {
        const auto& m = history[i];
        std::string c = m.content;
        if (m.role == "tool" && c.size() > 2000) c = c.substr(0, 2000) + "…";
        if (c.rfind(kSummaryPrefix, 0) == 0) c = c.substr(std::string(kSummaryPrefix).size());
        transcript << m.role << ": " << c << "\n";
    }
    std::string prompt =
        "Summarize the conversation below so it can replace the original messages. Preserve decisions, open tasks, "
        "facts about the user, file paths, identifiers and anything needed to continue. Be concise.";
    if (!instructions.empty()) prompt += "\nAdditional focus: " + instructions;
    std::vector<agent::Message> req = {{.role = "system", .content = prompt},
                                       {.role = "user", .content = transcript.str()}};
    std::string summary;
    try {
        auto res = prov.Complete(req, nlohmann::json::array());
        if (res.ok && !res.message.content.empty()) summary = res.message.content;
    } catch (...) {}

    if (start == 1) r.messages.push_back(history.front());
    if (!summary.empty()) {
        r.summarized = true;
        r.summary = summary;
        r.messages.push_back({.role = "user", .content = std::string(kSummaryPrefix) + summary});
    }
    for (std::size_t i = cut; i < history.size(); ++i) r.messages.push_back(history[i]);
    r.compacted = true;
    r.dropped = cut - start;
    r.after_tokens = EstimateTokens(r.messages);
    return r;
}

MemoryFlushPolicy MemoryFlushFromConfig(const nlohmann::json& raw) {
    MemoryFlushPolicy p;
    try {
        const auto& f = raw.at("agents").at("defaults").at("compaction").at("memoryFlush");
        if (f.is_object()) {
            p.enabled = f.value("enabled", p.enabled);
            p.prompt = f.value("prompt", p.prompt);
            p.system_prompt = f.value("systemPrompt", p.system_prompt);
        }
    } catch (...) {}
    return p;
}

bool ParseCompactCommand(const std::string& text, std::string& instructions) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (text.compare(i, 8, "/compact") != 0) return false;
    std::size_t j = i + 8;
    if (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j]))) return false;
    auto s = text.find_first_not_of(" \t\r\n", j);
    instructions = s == std::string::npos ? "" : text.substr(s);
    return true;
}

}  // namespace openclaw::context
