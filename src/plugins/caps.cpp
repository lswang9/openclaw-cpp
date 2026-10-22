#include "openclaw/plugins/caps.hpp"

namespace openclaw::plugins {

bool HasCapability(JsHost* js, const std::string& kind) {
    return js && js->Running() && js->HasRegistration(kind);
}

std::optional<nlohmann::json> InvokeRegistration(JsHost* js, const std::string& kind, const nlohmann::json& args,
                                                 const std::string& method, const std::string& id) {
    if (!HasCapability(js, kind)) return std::nullopt;
    try {
        nlohmann::json p{{"kind", kind}, {"method", method}, {"args", args.is_null() ? nlohmann::json::object() : args}};
        if (!id.empty()) p["id"] = id;
        auto r = js->Rpc("invoke_registration", p);
        if (r.is_object() && r.value("ok", false)) return r;
        return r;  // still return error payload so callers can log
    } catch (...) { return std::nullopt; }
}

void NotifyReload(JsHost* js, const nlohmann::json& detail) {
    if (!HasCapability(js, "registerReload")) return;
    InvokeRegistration(js, "registerReload", detail, "reload");
}

void NotifyLifecycle(JsHost* js, const std::string& phase) {
    if (!HasCapability(js, "registerRuntimeLifecycle")) return;
    InvokeRegistration(js, "registerRuntimeLifecycle", {{"phase", phase}}, phase);
}

}  // namespace openclaw::plugins
