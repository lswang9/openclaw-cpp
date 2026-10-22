#include "openclaw/persist/store.hpp"

#include <fstream>
#include "openclaw/persist/state_db.hpp"

namespace openclaw::persist {
namespace fs = std::filesystem;

StateStore::StateStore(fs::path data_dir) : root_(std::move(data_dir)) {
    fs::create_directories(root_);
}

void StateStore::WriteJson(const fs::path& path, const nlohmann::json& j) const {
    StateWrite(path, j.dump(2));
}

nlohmann::json StateStore::ReadJson(const fs::path& path) const {
    auto t = StateRead(path);
    if (!t) return nlohmann::json::object();
    auto j = nlohmann::json::parse(*t, nullptr, false);
    return j.is_discarded() ? nlohmann::json::object() : j;
}

void StateStore::SaveCron(const cron::Scheduler& sch) const {
    WriteJson(root_ / "cron.json", {{"jobs", sch.ListJson()}});
}

void StateStore::LoadCron(cron::Scheduler& sch) const {
    auto j = ReadJson(root_ / "cron.json");
    if (!j.contains("jobs") || !j["jobs"].is_array()) return;
    for (const auto& item : j["jobs"]) {
        cron::Job job;
        job.id = item.value("id", "");
        job.cron_expr = item.value("cron", item.value("cron_expr", "* * * * *"));
        job.payload = item.value("payload", "");
        job.enabled = item.value("enabled", true);
        job.run_count = item.value("run_count", 0);
        job.last_run_utc = item.value("last_run_utc", "");
        if (!job.id.empty()) sch.Add(std::move(job));
    }
}

void StateStore::SavePairing(const auth::PairingStore& pairing) const {
    WriteJson(root_ / "pairing.json", {{"pending", pairing.ListJson()}});
}

void StateStore::LoadPairing(auth::PairingStore& pairing) const {
    auto j = ReadJson(root_ / "pairing.json");
    if (!j.contains("pending") || !j["pending"].is_array()) return;
    for (const auto& item : j["pending"]) {
        auth::PairingRequest r;
        r.id = item.value("id", "");
        r.channel = item.value("channel", "");
        r.code = item.value("code", "");
        r.sender = item.value("sender", "");
        r.approved = item.value("approved", false);
        if (!r.id.empty()) pairing.Add(std::move(r));
    }
}

void StateStore::SaveNodes(const nodes::Registry& nodes) const {
    WriteJson(root_ / "nodes.json", {{"nodes", nodes.ListJson()}});
}

void StateStore::LoadNodes(nodes::Registry& nodes) const {
    auto j = ReadJson(root_ / "nodes.json");
    if (!j.contains("nodes") || !j["nodes"].is_array()) return;
    for (const auto& item : j["nodes"]) {
        nodes.Upsert({.id = item.value("id", ""),
                      .name = item.value("name", ""),
                      .kind = item.value("kind", "stub"),
                      .online = item.value("online", false)});
    }
}

}  // namespace openclaw::persist
