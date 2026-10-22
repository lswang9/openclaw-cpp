#pragma once
// NIP-04 encrypted DMs (secp256k1 ECDH + AES-256-CBC). Keys are 32-byte hex
// (or nsec1/npub1 bech32). Ciphertext form: base64(ct)?iv=base64(iv).
#include <optional>
#include <string>

namespace openclaw::channels::nostr {

// Decode hex or bech32 (nsec1/npub1) to 32-byte lowercase hex. Empty on failure.
std::string NormalizeKey(const std::string& in);
// secp256k1 pubkey x-only hex from private key hex.
std::string PubkeyHex(const std::string& priv_hex);
// Encrypt plaintext for peer_pub (x-only hex). Empty on failure.
std::string Encrypt(const std::string& priv_hex, const std::string& peer_pub_hex, const std::string& text);
// Decrypt NIP-04 ciphertext from peer. nullopt on failure.
std::optional<std::string> Decrypt(const std::string& priv_hex, const std::string& peer_pub_hex,
                                   const std::string& ciphertext);

}  // namespace openclaw::channels::nostr
