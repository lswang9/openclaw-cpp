#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::secrets {

class Store {
public:
    // If passphrase non-empty, values encrypted at rest (AES-256-GCM, OpenSSL).
    explicit Store(std::filesystem::path path, std::string passphrase = {});

    void SetPassphrase(std::string passphrase);
    bool Encrypted() const { return !passphrase_.empty(); }

    void Set(const std::string& key, const std::string& value);
    std::optional<std::string> Get(const std::string& key) const;
    bool Delete(const std::string& key);
    std::vector<std::string> ListKeys() const;
    nlohmann::json ListJson() const;  // keys + encrypted flag only

private:
    std::filesystem::path path_;
    std::string passphrase_;
    nlohmann::json data_ = nlohmann::json::object();  // key -> ciphertext blob or plain
    void Load();
    void Save() const;
    std::string Encrypt(const std::string& plain) const;
    std::string Decrypt(const std::string& blob) const;
};

}  // namespace openclaw::secrets
