#pragma once
// 1.144: SQLite backing for the gateway's JSON state documents (cron, pairing, nodes, board, device links,
// subagent registry, auth order, and every subsystem store that goes through ReadJsonFile/WriteJsonFile).
// When enabled, `<dir>/<name>.json` lives as row `name.json` in `<dir>/openclaw-state.sqlite`
// (table state_documents). Existing files are imported once on first read and left in place.
#include <filesystem>
#include <optional>
#include <string>

namespace openclaw::persist {
// "sqlite" when OPENCLAW_STATE_STORE=sqlite, or (unset) when the session backend is sqlite.
bool StateDbEnabled();
// Returns the document text; std::nullopt if neither the DB nor a legacy file has it.
std::optional<std::string> StateRead(const std::filesystem::path& file);
// Writes the document (DB when enabled, else atomic file write). Returns false on error.
bool StateWrite(const std::filesystem::path& file, const std::string& text);
std::filesystem::path StateDbPath(const std::filesystem::path& file);
}  // namespace openclaw::persist
