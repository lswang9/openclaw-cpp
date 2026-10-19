#include "openclaw/memory/store.hpp"
#include <fstream>

namespace openclaw::memory {
namespace fs = std::filesystem;

Store::Store(fs::path root) : path_(std::move(root) / "memory.json") {
    fs::create_directories(path_.parent_path());
    Load();
}

void Store::Load() {
    if (!fs::exists(path_)) {
        data_ = nlohmann::json::object();
        return;
    }
    std::ifstream in(path_);
    in >> data_;
    if (!data_.is_object()) {
        data_ = nlohmann::json::object();
    }
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
    if (!data_.contains(key) || !data_[key].is_string()) {
        return std::nullopt;
    }
    return data_[key].get<std::string>();
}

bool Store::Remove(const std::string& key) {
    if (!data_.contains(key)) {
        return false;
    }
    data_.erase(key);
    Save();
    return true;
}

std::vector<std::string> Store::Keys() const {
    std::vector<std::string> keys;
    for (auto it = data_.begin(); it != data_.end(); ++it) {
        keys.push_back(it.key());
    }
    return keys;
}

nlohmann::json Store::Dump() const { return data_; }

}  // namespace openclaw::memory
