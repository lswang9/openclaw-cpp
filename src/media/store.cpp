#include "openclaw/media/store.hpp"
#include <fstream>

namespace openclaw::media {
namespace fs = std::filesystem;

Store::Store(fs::path root) : root_(std::move(root)) {
    fs::create_directories(root_);
}

Asset Store::Put(const std::string& filename, const std::string& content_type, const std::string& data) {
    Asset a;
    a.id = "media-" + std::to_string(seq_++);
    a.filename = filename;
    a.content_type = content_type;
    a.bytes = data.size();
    a.path = root_ / (a.id + "_" + filename);
    std::ofstream out(a.path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    assets_.push_back(a);
    return a;
}

nlohmann::json Store::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& a : assets_) {
        arr.push_back({{"id", a.id}, {"filename", a.filename}, {"content_type", a.content_type},
                       {"bytes", a.bytes}, {"path", a.path.string()}});
    }
    return arr;
}

nlohmann::json Store::GetJson(const std::string& id) const {
    for (const auto& a : assets_) {
        if (a.id == id) {
            return {{"id", a.id}, {"filename", a.filename}, {"content_type", a.content_type},
                    {"bytes", a.bytes}, {"path", a.path.string()}};
        }
    }
    return {{"error", "not found"}, {"id", id}};
}

}  // namespace openclaw::media
