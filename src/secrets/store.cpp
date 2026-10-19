#include "openclaw/secrets/store.hpp"

#include <fstream>

namespace openclaw::secrets {
namespace fs = std::filesystem;

Store::Store(fs::path path) : path_(std::move(path)) {
    if (path_.has_parent_path()) {
        fs::create_directories(path_.parent_path());
    }
    Load();
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

void Store::Set(const std::string& key, const std::string& value) {
    data_[key] = value;
    Save();
}

std::optional<std::string> Store::Get(const std::string& key) const {
    if (!data_.contains(key) || !data_[key].is_string()) return std::nullopt;
    return data_[key].get<std::string>();
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
    for (const auto& k : ListKeys()) arr.push_back({{"key", k}});
    return arr;
}

}  // namespace openclaw::secrets
