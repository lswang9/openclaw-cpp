#pragma once

#include "openclaw/auth/gateway_auth.hpp"
#include "openclaw/cron/scheduler.hpp"
#include "openclaw/nodes/registry.hpp"

#include <filesystem>
#include <string>

namespace openclaw::persist {

class StateStore {
public:
    explicit StateStore(std::filesystem::path data_dir);

    const std::filesystem::path& Root() const { return root_; }

    void SaveCron(const cron::Scheduler& sch) const;
    void LoadCron(cron::Scheduler& sch) const;

    void SavePairing(const auth::PairingStore& pairing) const;
    void LoadPairing(auth::PairingStore& pairing) const;

    void SaveNodes(const nodes::Registry& nodes) const;
    void LoadNodes(nodes::Registry& nodes) const;

private:
    std::filesystem::path root_;
    void WriteJson(const std::filesystem::path& path, const nlohmann::json& j) const;
    nlohmann::json ReadJson(const std::filesystem::path& path) const;
};

}  // namespace openclaw::persist
