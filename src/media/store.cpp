#include "openclaw/media/store.hpp"
#include <fstream>

namespace openclaw::media {
namespace fs = std::filesystem;

Store::Store(fs::path root) : root_(std::move(root)) {
    fs::create_directories(root_);
    ReloadIndex();
}

void Store::ReloadIndex() {
    assets_.clear();
    seq_ = 1;
    if (!fs::exists(root_)) return;
    for (const auto& e : fs::directory_iterator(root_)) {
        if (!e.is_regular_file()) continue;
        auto name = e.path().filename().string();
        // media-<n>_<filename>
        if (name.rfind("media-", 0) != 0) continue;
        auto us = name.find('_');
        if (us == std::string::npos) continue;
        Asset a;
        a.id = name.substr(0, us);
        a.filename = name.substr(us + 1);
        a.path = e.path();
        a.bytes = static_cast<std::size_t>(e.file_size());
        a.content_type = "application/octet-stream";
        assets_.push_back(a);
        // bump seq
        try {
            int n = std::stoi(a.id.substr(6));
            if (n >= seq_) seq_ = n + 1;
        } catch (...) {
        }
    }
}

std::string Store::DecodeBase64(const std::string& s) {
    static const int T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1};
    std::string out;
    out.reserve(s.size() * 3 / 4);
    int val = 0, valb = -8;
    for (unsigned char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int d = T[c];
        if (d < 0) return {};
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

Asset Store::Put(const std::string& filename, const std::string& content_type, const std::string& data) {
    Asset a;
    a.id = "media-" + std::to_string(seq_++);
    a.filename = filename.empty() ? "upload.bin" : filename;
    a.content_type = content_type.empty() ? "application/octet-stream" : content_type;
    a.bytes = data.size();
    a.path = root_ / (a.id + "_" + a.filename);
    std::ofstream out(a.path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    assets_.push_back(a);
    return a;
}

nlohmann::json Store::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& a : assets_) {
        arr.push_back({{"id", a.id},
                       {"filename", a.filename},
                       {"content_type", a.content_type},
                       {"bytes", a.bytes},
                       {"path", a.path.string()}});
    }
    return arr;
}

nlohmann::json Store::GetJson(const std::string& id) const {
    auto a = Find(id);
    if (!a) return {{"error", "not found"}, {"id", id}};
    return {{"id", a->id},
            {"filename", a->filename},
            {"content_type", a->content_type},
            {"bytes", a->bytes},
            {"path", a->path.string()}};
}

std::optional<Asset> Store::Find(const std::string& id) const {
    for (const auto& a : assets_)
        if (a.id == id) return a;
    return std::nullopt;
}

std::optional<std::string> Store::ReadBytes(const std::string& id) const {
    auto a = Find(id);
    if (!a) return std::nullopt;
    std::ifstream in(a->path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Store::Remove(const std::string& id) {
    for (auto it = assets_.begin(); it != assets_.end(); ++it) {
        if (it->id == id) {
            std::error_code ec;
            fs::remove(it->path, ec);
            assets_.erase(it);
            return true;
        }
    }
    return false;
}

}  // namespace openclaw::media
