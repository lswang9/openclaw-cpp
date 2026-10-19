#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace openclaw::cron {

struct Job {
    std::string id;
    std::string cron_expr;  // "m h dom mon dow" (5-field, UTC)
    std::string payload;
    bool enabled = true;
    std::uint64_t run_count = 0;
    std::string last_run_utc;
};

bool MatchesCron(const std::string& expr, const std::tm& utc);

class Scheduler {
public:
    using TickHandler = std::function<void(const Job&)>;

    void Add(Job job);
    bool Remove(const std::string& id);
    bool Update(const std::string& id, const Job& job);
    std::vector<Job> List() const;
    nlohmann::json ListJson() const;
    Job* Find(const std::string& id);

    std::size_t Tick(std::chrono::system_clock::time_point now, TickHandler on_fire = {});

    void RunDaemon(std::atomic<bool>& running, TickHandler on_fire = {},
                   std::chrono::milliseconds interval = std::chrono::milliseconds(1000));

private:
    std::vector<Job> jobs_;
};

}  // namespace openclaw::cron
