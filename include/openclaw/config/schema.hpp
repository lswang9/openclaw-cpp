#pragma once
// Upstream OpenClaw v2026.10.5 base config schema (JSON Schema + uiHints), generated from upstream's
// computeBaseConfigSchemaResponse() by scripts/gen-config-schema.sh and embedded at build time.
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace openclaw::config {
// {schema, uiHints, version, generatedAt}
const nlohmann::json& UpstreamSchemaResponse();
// Path lookup ("gateway.port", "agents.list.*.id", "." = root): {path, schema, hint?, hintPath?, children[]}; null if not found.
nlohmann::json LookupSchema(const std::string& path);
// Number of leaf+branch property paths in the schema.
std::size_t SchemaPathCount();
// Validate a raw config document against the schema: unknown keys where the schema is closed, type
// mismatches, enum violations, numeric bounds. Each issue: {path, code, message}.
nlohmann::json ValidateAgainstSchema(const nlohmann::json& raw, std::size_t max_issues = 200);
}  // namespace openclaw::config
