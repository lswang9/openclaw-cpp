#include "openclaw/cron/scheduler.hpp"
#include <algorithm>

namespace openclaw::cron {

void Scheduler::Add(Job job) { jobs_.push_back(std::move(job)); }

bool Scheduler::Remove(const std::string& id) {
    const auto n = jobs_.size();
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [&](const Job& j) { return j.id == id; }),
                jobs_.end());
    return jobs_.size() != n;
}

std::vector<Job> Scheduler::List() const { return jobs_; }

nlohmann::json Scheduler::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& j : jobs_) {
        arr.push_back({{"id", j.id},
                       {"cron", j.cron_expr},
                       {"payload", j.payload},
                       {"enabled", j.enabled}});
    }
    return arr;
}

}  // namespace openclaw::cron
