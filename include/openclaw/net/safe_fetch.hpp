#pragma once
// SSRF-safe HTTP(S) GET: resolves the host, refuses loopback/private/link-local/CGNAT/ULA/multicast addresses
// unless allow_private, pins the vetted address (CURLOPT_RESOLVE, no DNS rebinding), follows ≤3 redirects
// manually re-checking each hop, and aborts past max_bytes.
#include <string>

namespace openclaw::net {

struct FetchResult {
    bool ok = false;
    long status = 0;
    std::string body, content_type, final_url, error;
    bool truncated = false;
};
bool IsPrivateAddress(const std::string& ip);
FetchResult SafeFetch(const std::string& url, std::size_t max_bytes, bool allow_private, int timeout_sec = 20);

}  // namespace openclaw::net
