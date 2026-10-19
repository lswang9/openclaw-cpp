#include "openclaw/session/store.hpp"
#include <cctype>
#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace openclaw::session {
namespace fs = std::filesystem;

Store::Store(fs::path root) : root_(std::move(root)) {
    fs::create_directories(root_);
}

fs::path Store::PathFor(const std::string& id) const {
    // sanitize: allow alnum, dash, underscore only
    std::string safe;
    for (char c : id) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            safe.push_back(c);
        }
    }
    if (safe.empty()) {
        safe = "default";
    }
    return root_ / (safe + ".json");
}

std::vector<std::string> Store::List() const {
    std::vector<std::string> ids;
    if (!fs::exists(root_)) {
        return ids;
    }
    for (const auto& e : fs::directory_iterator(root_)) {
        if (e.path().extension() == ".json") {
            ids.push_back(e.path().stem().string());
        }
    }
    return ids;
}

bool Store::Exists(const std::string& id) const {
    return fs::exists(PathFor(id));
}

std::vector<agent::Message> Store::Load(const std::string& id) const {
    const auto path = PathFor(id);
    if (!fs::exists(path)) {
        return {};
    }
    std::ifstream in(path);
    nlohmann::json j;
    in >> j;
    std::vector<agent::Message> msgs;
    if (j.contains("messages") && j["messages"].is_array()) {
        for (const auto& m : j["messages"]) {
            msgs.push_back(agent::Message::FromJson(m));
        }
    }
    return msgs;
}

void Store::Save(const std::string& id, const std::vector<agent::Message>& messages) const {
    nlohmann::json j;
    j["id"] = id;
    j["messages"] = nlohmann::json::array();
    for (const auto& m : messages) {
        j["messages"].push_back(m.ToJson());
    }
    const auto path = PathFor(id);
    std::ofstream out(path);
    out << j.dump(2);
}

void Store::Append(const std::string& id, const agent::Message& message) const {
    auto msgs = Load(id);
    msgs.push_back(message);
    Save(id, msgs);
}

bool Store::Remove(const std::string& id) const {
    const auto path = PathFor(id);
    if (!fs::exists(path)) {
        return false;
    }
    return fs::remove(path);
}

}  // namespace openclaw::session
