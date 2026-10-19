#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace openclaw::cron {

struct Job {
    std::string id;
    std::string cron_expr;  // stored as string; evaluation TODO
    std::string payload;    // opaque / prompt
    bool enabled = true;
};

// In-memory cron registry (persist later). Toward OpenClaw gateway cron events.
class Scheduler {
public:
    void Add(Job job);
    bool Remove(const std::string& id);
    std::vector<Job> List() const;
    nlohmann::json ListJson() const;

private:
    std::vector<Job> jobs_;
};

}  // namespace openclaw::cron
