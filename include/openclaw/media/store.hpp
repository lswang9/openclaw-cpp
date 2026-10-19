#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::media {

struct Asset {
    std::string id;
    std::string filename;
    std::string content_type;
    std::size_t bytes = 0;
    std::filesystem::path path;
};

class Store {
public:
    explicit Store(std::filesystem::path root);
    std::filesystem::path Root() const { return root_; }

    Asset Put(const std::string& filename, const std::string& content_type, const std::string& data);
    nlohmann::json ListJson() const;
    nlohmann::json GetJson(const std::string& id) const;
    std::optional<Asset> Find(const std::string& id) const;
    std::optional<std::string> ReadBytes(const std::string& id) const;
    bool Remove(const std::string& id);

    // Decode standard base64 (padded); empty on failure.
    static std::string DecodeBase64(const std::string& b64);

private:
    std::filesystem::path root_;
    std::vector<Asset> assets_;
    int seq_ = 1;
    void ReloadIndex();
};

}  // namespace openclaw::media
