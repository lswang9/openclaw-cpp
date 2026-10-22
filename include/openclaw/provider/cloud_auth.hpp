#pragma once
// Offline-testable cloud auth for Amazon Bedrock (AWS SigV4 / bearer) and Google Vertex (ADC).
#include <optional>
#include <string>
#include <vector>

namespace openclaw::provider::cloud_auth {

struct AwsCreds {
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;  // optional STS
    std::string region;         // AWS_REGION / AWS_DEFAULT_REGION / default us-east-1
    std::string bearer_token;   // AWS_BEARER_TOKEN_BEDROCK (skips SigV4 when set)
};

// Env chain only (access key + secret, optional session, optional bearer). No instance-role IMDS.
AwsCreds ResolveAwsFromEnv();

// SigV4 headers for a POST (Authorization, X-Amz-Date, X-Amz-Content-Sha256, optional X-Amz-Security-Token).
// host is the Host header value (no scheme); canonical_uri is the path (must start with /); query may be empty.
std::vector<std::string> SignAwsV4(const AwsCreds& c, const std::string& method, const std::string& host,
                                   const std::string& canonical_uri, const std::string& canonical_query,
                                   const std::string& payload, const std::string& service = "bedrock");

struct GoogleToken {
    std::string access_token;
    long long expires_at_ms = 0;
};

// Resolve GOOGLE_APPLICATION_CREDENTIALS (or ~/.config/gcloud/application_default_credentials.json).
// authorized_user → refresh_token grant against OPENCLAW_GOOGLE_TOKEN_URL or https://oauth2.googleapis.com/token.
// service_account → JWT bearer grant (RS256) against the same token URL (or credential token_uri).
// Cached until ~60s before expiry. why is set on failure.
std::optional<GoogleToken> ResolveGoogleAdcAccessToken(std::string* why = nullptr);

// Test helper: clear the in-process ADC token cache.
void ResetGoogleAdcCacheForTest();

}  // namespace openclaw::provider::cloud_auth
