#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
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
    Asset Put(const std::string& filename, const std::string& content_type, const std::string& data);
    nlohmann::json ListJson() const;
    nlohmann::json GetJson(const std::string& id) const;

private:
    std::filesystem::path root_;
    std::vector<Asset> assets_;
    int seq_ = 1;
};

}  // namespace openclaw::media
