#pragma once
// A2UI v0.8 (server→client: surfaceUpdate, dataModelUpdate, beginRendering, deleteSurface) host for the gateway
// canvas: JSONL validation matching upstream nodes-cli (createSurface = v0.9 rejected), persisted surface state,
// a server-side HTML renderer for the v0.8 standard catalog subset, and userAction resolution for Button clicks.
#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::canvas::a2ui {

struct Validation {
    bool ok = false;
    std::string version = "v0.8";
    std::size_t message_count = 0;
    std::string error;
    std::vector<nlohmann::json> messages;
};
Validation ValidateJsonl(const std::string& jsonl);
Validation ValidateMessages(const nlohmann::json& arr);
std::string BuildTextJsonl(const std::string& text, const std::string& surface_id = "main");

class Host {
public:
    explicit Host(std::filesystem::path file);
    nlohmann::json Apply(const std::vector<nlohmann::json>& messages);  // {ok, applied, surfaces}
    nlohmann::json Reset();
    nlohmann::json State() const;
    std::string RenderSurface(const std::string& surface_id) const;    // HTML fragment ("" if not rendering)
    std::string RenderPage() const;                                     // full renderer page
    // Resolve a Button click into an upstream-shaped userAction (name, surfaceId, sourceComponentId, timestamp, context).
    std::optional<nlohmann::json> ResolveAction(const std::string& surface_id, const std::string& component_id,
                                                const std::string& item_path = {}) const;
    std::uint64_t Revision() const;
    // Two-way input: write a TextField text / CheckBox value into the data model at the component's bound path.
    nlohmann::json SetInput(const std::string& surface_id, const std::string& component_id, const std::string& scope,
                            const nlohmann::json& value);

private:
    void Save() const;
    mutable std::mutex mu_;
    std::filesystem::path file_;
    nlohmann::json state_;  // {rev, surfaces:{id:{components:{}, data:{}, root, rendering, styles}}}
};

}  // namespace openclaw::canvas::a2ui
