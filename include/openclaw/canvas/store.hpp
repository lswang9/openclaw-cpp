#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace openclaw::canvas {

// Local canvas documents (HTML + JSON widgets) on disk.
// Not full upstream OpenClaw canvas / A2UI — Control UI + RPC only.
class Store {
public:
    explicit Store(std::filesystem::path dir);

    std::filesystem::path Dir() const { return dir_; }
    nlohmann::json Status() const;

    nlohmann::json List() const;
    nlohmann::json Create(const nlohmann::json& spec = {});
    nlohmann::json Get(const std::string& id) const;
    nlohmann::json Patch(const std::string& id, const nlohmann::json& patch);
    nlohmann::json Put(const std::string& id, const nlohmann::json& document);
    nlohmann::json Remove(const std::string& id);

    // A2UI-ish local event log (append-only, capped).
    nlohmann::json RecordEvent(const std::string& kind, const nlohmann::json& detail = {},
                               const std::string& canvas_id = {});
    nlohmann::json Events(std::size_t limit = 50, const std::string& canvas_id = {}) const;

    // Active / default document helpers (sim compatibility).
    nlohmann::json ActiveView();
    nlohmann::json ActivePut(const nlohmann::json& params);
    nlohmann::json ActivePreview();

private:
    void LoadIndex();
    void SaveIndex() const;
    void LoadEvents();
    void SaveEvents() const;
    std::filesystem::path DocPath(const std::string& id) const;
    std::string NextId();
    nlohmann::json* Find(const std::string& id);
    const nlohmann::json* Find(const std::string& id) const;
    void PersistDoc(const nlohmann::json& doc) const;
    nlohmann::json LoadDocFile(const std::string& id) const;

    mutable std::mutex mu_;
    std::filesystem::path dir_;
    nlohmann::json index_ = nlohmann::json::object();  // {version, active_id, docs:[{id,title,...}]}
    std::vector<nlohmann::json> events_;
    int seq_ = 1;
};

}  // namespace openclaw::canvas
