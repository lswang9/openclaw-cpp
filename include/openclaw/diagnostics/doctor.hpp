#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace openclaw::diagnostics {

struct HealthSnapshot {
    bool ok = true;
    std::string version;
    std::string align_target;
    nlohmann::json checks = nlohmann::json::array();
};

HealthSnapshot RunDoctor(const nlohmann::json& ctx);
nlohmann::json RunDoctorJson(const nlohmann::json& ctx);

}  // namespace openclaw::diagnostics
