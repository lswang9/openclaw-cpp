#pragma once
// Inbound webhook authenticity: Slack signing secret (HMAC-SHA256 v0) and Discord Ed25519; form bodies.
#include <nlohmann/json.hpp>
#include <string>

namespace openclaw::channels::signing {

// v0=hex(HMAC_SHA256(secret, "v0:" + ts + ":" + body)); rejects |now-ts| > 300s.
bool VerifySlack(const std::string& secret, const std::string& timestamp, const std::string& body,
                 const std::string& signature, long long now_sec, std::string* why = nullptr);
std::string SlackSignature(const std::string& secret, const std::string& timestamp, const std::string& body);

// Ed25519 over timestamp + body with the application's hex public key.
bool VerifyDiscord(const std::string& public_key_hex, const std::string& timestamp, const std::string& body,
                   const std::string& signature_hex, std::string* why = nullptr);

// application/x-www-form-urlencoded → {"k": "v"}.
nlohmann::json ParseForm(const std::string& body);

}  // namespace openclaw::channels::signing
