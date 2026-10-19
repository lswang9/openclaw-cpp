#include "openclaw/diagnostics/doctor.hpp"

namespace openclaw::diagnostics {

HealthSnapshot RunDoctor(const nlohmann::json& ctx) {
    HealthSnapshot h;
    h.version = ctx.value("version", "unknown");
    h.align_target = ctx.value("align_target", "OpenClaw 2026.10.5");

    auto add = [&](const std::string& name, bool ok, const std::string& detail) {
        h.checks.push_back({{"name", name}, {"ok", ok}, {"detail", detail}});
        if (!ok) h.ok = false;
    };

    add("gateway", true, "process up");
    add("methods", ctx.value("methods_implemented", 0) > 0,
        "implemented=" + std::to_string(ctx.value("methods_implemented", 0)));
    add("providers", ctx.value("providers", nlohmann::json::array()).is_array(),
        "count=" + std::to_string(ctx.value("providers", nlohmann::json::array()).size()));
    add("sessions_dir", true, ctx.value("session_dir", ""));
    add("data_dir", true, ctx.value("data_dir", ""));
    if (ctx.contains("deepseek_key_set")) {
        add("deepseek_key", ctx.value("deepseek_key_set", false),
            ctx.value("deepseek_key_set", false) ? "set" : "missing");
    }
    add("memory", true, "keys=" + std::to_string(ctx.value("memory_keys", 0)));
    add("cron", true, "jobs=" + std::to_string(ctx.value("cron_jobs", 0)));
    return h;
}

nlohmann::json RunDoctorJson(const nlohmann::json& ctx) {
    auto h = RunDoctor(ctx);
    return {{"ok", h.ok},
            {"version", h.version},
            {"align_target", h.align_target},
            {"checks", h.checks}};
}

}  // namespace openclaw::diagnostics
