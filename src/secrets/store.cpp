#include "openclaw/secrets/store.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <fstream>
#include <stdexcept>
#include <vector>

namespace openclaw::secrets {
namespace fs = std::filesystem;

namespace {

std::string B64Encode(const unsigned char* data, size_t len) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; ++i) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(T[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(T[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::vector<unsigned char> B64Decode(const std::string& s) {
    std::vector<int> dec(256, -1);
    const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; tbl[i]; ++i) dec[static_cast<unsigned char>(tbl[i])] = i;
    std::vector<unsigned char> out;
    int val = 0, valb = -8;
    for (unsigned char c : s) {
        if (dec[c] == -1) break;
        val = (val << 6) + dec[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

}  // namespace

Store::Store(fs::path path, std::string passphrase)
    : path_(std::move(path)), passphrase_(std::move(passphrase)) {
    if (path_.has_parent_path()) fs::create_directories(path_.parent_path());
    Load();
}

void Store::SetPassphrase(std::string passphrase) {
    passphrase_ = std::move(passphrase);
}

void Store::Load() {
    if (!fs::exists(path_)) {
        data_ = nlohmann::json::object();
        return;
    }
    std::ifstream in(path_);
    in >> data_;
    if (!data_.is_object()) data_ = nlohmann::json::object();
}

void Store::Save() const {
    std::ofstream out(path_);
    out << data_.dump(2);
}

std::string Store::Encrypt(const std::string& plain) const {
    if (passphrase_.empty()) return plain;
    unsigned char key[32];
    SHA256(reinterpret_cast<const unsigned char*>(passphrase_.data()), passphrase_.size(), key);
    unsigned char iv[12];
    if (RAND_bytes(iv, sizeof(iv)) != 1) throw std::runtime_error("RAND_bytes failed");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    std::vector<unsigned char> ct(plain.size() + 16);
    int len = 0, ct_len = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, iv);
    EVP_EncryptUpdate(ctx, ct.data(), &len,
                      reinterpret_cast<const unsigned char*>(plain.data()),
                      static_cast<int>(plain.size()));
    ct_len = len;
    EVP_EncryptFinal_ex(ctx, ct.data() + len, &len);
    ct_len += len;
    unsigned char tag[16];
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(ctx);
    ct.resize(ct_len);

    nlohmann::json blob{
        {"v", 1},
        {"alg", "aes-256-gcm"},
        {"iv", B64Encode(iv, sizeof(iv))},
        {"tag", B64Encode(tag, sizeof(tag))},
        {"ct", B64Encode(ct.data(), ct.size())},
    };
    return blob.dump();
}

std::string Store::Decrypt(const std::string& blob) const {
    if (passphrase_.empty()) return blob;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(blob);
    } catch (...) {
        return blob;  // legacy plaintext
    }
    if (!j.contains("ct")) return blob;
    unsigned char key[32];
    SHA256(reinterpret_cast<const unsigned char*>(passphrase_.data()), passphrase_.size(), key);
    auto iv = B64Decode(j.value("iv", ""));
    auto tag = B64Decode(j.value("tag", ""));
    auto ct = B64Decode(j.value("ct", ""));
    if (iv.size() != 12 || tag.size() != 16) throw std::runtime_error("bad secret blob");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    std::vector<unsigned char> pt(ct.size());
    int len = 0, pt_len = 0;
    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, iv.data());
    EVP_DecryptUpdate(ctx, pt.data(), &len, ct.data(), static_cast<int>(ct.size()));
    pt_len = len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data());
    if (EVP_DecryptFinal_ex(ctx, pt.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("secret decrypt failed (bad passphrase?)");
    }
    pt_len += len;
    EVP_CIPHER_CTX_free(ctx);
    return std::string(reinterpret_cast<char*>(pt.data()), pt_len);
}

void Store::Set(const std::string& key, const std::string& value) {
    data_[key] = Encrypt(value);
    Save();
}

std::optional<std::string> Store::Get(const std::string& key) const {
    if (!data_.contains(key) || !data_[key].is_string()) return std::nullopt;
    return Decrypt(data_[key].get<std::string>());
}

bool Store::Delete(const std::string& key) {
    if (!data_.contains(key)) return false;
    data_.erase(key);
    Save();
    return true;
}

std::vector<std::string> Store::ListKeys() const {
    std::vector<std::string> keys;
    for (auto it = data_.begin(); it != data_.end(); ++it) keys.push_back(it.key());
    return keys;
}

nlohmann::json Store::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& k : ListKeys()) {
        arr.push_back({{"key", k}, {"encrypted", Encrypted()}});
    }
    return arr;
}

}  // namespace openclaw::secrets
