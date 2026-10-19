#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

namespace openclaw {

inline std::string EnvOr(const char* key, std::string_view fallback) {
    if (const char* v = std::getenv(key); v && *v) {
        return std::string{v};
    }
    return std::string{fallback};
}

}  // namespace openclaw
