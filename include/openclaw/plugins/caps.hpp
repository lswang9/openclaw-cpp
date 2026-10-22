#pragma once
// 1.139: consume previously inert api.register* registrations via the JS host.
#include "openclaw/plugins/js_host.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace openclaw::plugins {

// Invoke a captured registration (kind like "registerEmbeddingProvider"). Returns nullopt when none / host missing.
std::optional<nlohmann::json> InvokeRegistration(JsHost* js, const std::string& kind, const nlohmann::json& args = {},
                                                 const std::string& method = "execute", const std::string& id = {});

// True when at least one registration of that kind is loaded.
bool HasCapability(JsHost* js, const std::string& kind);

// Notify reload / lifecycle registrations (best-effort).
void NotifyReload(JsHost* js, const nlohmann::json& detail = {});
void NotifyLifecycle(JsHost* js, const std::string& phase);  // "start" | "stop"

}  // namespace openclaw::plugins
