#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <string>
#include <vector>

namespace openclaw::gateway {

// Full upstream-advertised catalogs (OpenClaw Gateway).
std::vector<std::string> AdvertisedMethods();
nlohmann::json AdvertisedMethodsJson();
std::vector<std::string> AdvertisedEvents();
std::size_t AdvertisedMethodCount();
std::size_t AdvertisedEventCount();

// Methods we implement with real or structured stub responses.
std::vector<std::string> ImplementedMethods();
std::vector<std::string> DispatchableMethods();   // every method with a dispatch path (incl. honest not-implemented)
std::vector<std::string> UnimplementedMethods();  // upstream methods answering "not implemented" (generated)

}  // namespace openclaw::gateway
