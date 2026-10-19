#include "openclaw/config/config.hpp"

#include <cstdlib>
#include <fstream>

namespace openclaw::config {
namespace fs = std::filesystem;

fs::path DefaultPath() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / ".openclaw-cpp" / "config.json";
    }
    return fs::path("/tmp/openclaw-cpp-config.json");
}

Snapshot LoadOrDefault(const fs::path& path) {
    Snapshot s;
    if (!fs::exists(path)) {
        return s;
    }
    std::ifstream in(path);
    nlohmann::json j;
    in >> j;
    s.raw = j;
    if (j.contains("gateway")) {
        s.gateway_host = j["gateway"].value("host", s.gateway_host);
        s.gateway_port = static_cast<std::uint16_t>(j["gateway"].value("port", s.gateway_port));
    }
    if (j.contains("agents") && j["agents"].contains("defaults")) {
        s.deepseek_model = j["agents"]["defaults"].value("model", s.deepseek_model);
    }
    s.session_dir = j.value("session_dir", s.session_dir);
    s.align_target = j.value("align_target", s.align_target);
    return s;
}

void Save(const fs::path& path, const Snapshot& snap) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << snap.ToJson().dump(2);
}

}  // namespace openclaw::config
