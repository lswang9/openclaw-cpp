#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <string>

namespace openclaw::board {

// Simple Kanban-style board persisted as JSON on disk.
// Not full upstream OpenClaw board UX — local Control UI only.
class Store {
public:
    explicit Store(std::filesystem::path path);

    std::filesystem::path Path() const { return path_; }
    nlohmann::json Get() const;
    nlohmann::json Status() const;

    nlohmann::json EnsureDefaults();  // create default columns if empty
    nlohmann::json SetBoard(const nlohmann::json& board);

    nlohmann::json AddColumn(const std::string& title, const std::string& id = {});
    nlohmann::json AddCard(const std::string& column_id, const std::string& title,
                           const std::string& body = {});
    nlohmann::json UpdateCard(const std::string& id, const nlohmann::json& patch);
    nlohmann::json MoveCard(const std::string& id, const std::string& column_id, int order = -1);
    nlohmann::json DeleteCard(const std::string& id);

private:
    void Load();
    void Save() const;
    std::string NextId(const std::string& prefix);
    nlohmann::json* FindCard(const std::string& id);
    const nlohmann::json* FindCard(const std::string& id) const;

    mutable std::mutex mu_;
    std::filesystem::path path_;
    nlohmann::json data_ = nlohmann::json::object();
    int seq_ = 1;
};

}  // namespace openclaw::board
