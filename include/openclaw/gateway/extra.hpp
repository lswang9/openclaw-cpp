#pragma once

#include "openclaw/gateway/server.hpp"

#include <optional>
#include <string>

namespace openclaw::gateway {

struct ExtraResult {
    bool handled = false;
    bool ok = true;
    nlohmann::json payload;
    std::string error;
};

ExtraResult TryExtraDispatch(ServerDeps& deps, const std::string& method,
                             const nlohmann::json& params);

}  // namespace openclaw::gateway
