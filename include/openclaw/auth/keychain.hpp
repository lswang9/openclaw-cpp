#pragma once
// OS keychain for auth-profile secrets. Linux: libsecret (Secret Service over D-Bus), loaded with dlopen so the
// build has no hard dependency. Unavailable (no lib / no session bus / locked) → callers keep secrets in the 0600 file.
// OPENCLAW_KEYCHAIN=off forces the file backend; =libsecret makes a failed keychain write an error instead of a fallback.
#include <optional>
#include <string>

namespace openclaw::auth::keychain {

bool Available();            // libsecret loaded and a probe store/lookup round trip succeeded
std::string Backend();       // "libsecret" | "file"
std::string UnavailableReason();
bool Store(const std::string& profile_id, const std::string& field, const std::string& secret);
std::optional<std::string> Lookup(const std::string& profile_id, const std::string& field);
bool Clear(const std::string& profile_id, const std::string& field);

}  // namespace openclaw::auth::keychain
