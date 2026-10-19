#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace openclaw::gateway {

// Subset of OpenClaw Gateway advertised methods we intend to implement.
std::vector<std::string> AdvertisedMethods();
nlohmann::json AdvertisedMethodsJson();

// Events we currently emit / plan (from upstream GATEWAY_EVENTS).
std::vector<std::string> AdvertisedEvents();

}  // namespace openclaw::gateway
