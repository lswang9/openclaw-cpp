#include "openclaw/cron/scheduler.hpp"

#include <algorithm>
#include <sstream>
#include <thread>

namespace openclaw::cron {
namespace {

std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            if (!cur.empty()) {
                out.push_back(cur);
            }
            cur.clear();
        } else if (c != ' ' && c != '\t') {
            // keep non-space for field tokens; spaces already delimit fields at higher level
            cur.push_back(c);
        } else {
            // ignore
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

std::vector<std::string> SplitFields(const std::string& expr) {
    std::istringstream iss(expr);
    std::vector<std::string> fields;
    std::string f;
    while (iss >> f) {
        fields.push_back(f);
    }
    return fields;
}

bool MatchField(const std::string& field, int value, int min_v, int max_v) {
    if (field == "*") {
        return true;
    }
    // lists
    std::vector<std::string> parts;
    std::string cur;
    for (char c : field) {
        if (c == ',') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        parts.push_back(cur);
    }

    for (const auto& part : parts) {
        if (part == "*") {
            return true;
        }
        // */N
        if (part.rfind("*/", 0) == 0) {
            const int step = std::stoi(part.substr(2));
            if (step > 0 && ((value - min_v) % step) == 0) {
                return true;
            }
            continue;
        }
        // A-B or A-B/N
        auto dash = part.find('-');
        if (dash != std::string::npos) {
            int a = std::stoi(part.substr(0, dash));
            std::string rest = part.substr(dash + 1);
            int step = 1;
            auto slash = rest.find('/');
            if (slash != std::string::npos) {
                step = std::stoi(rest.substr(slash + 1));
                rest = rest.substr(0, slash);
            }
            int b = std::stoi(rest);
            if (value >= a && value <= b && step > 0 && ((value - a) % step) == 0) {
                return true;
            }
            continue;
        }
        if (std::stoi(part) == value) {
            return true;
        }
    }
    return false;
}

std::string MinuteKey(const std::tm& tm) {
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M", &tm);
    return buf;
}

}  // namespace

bool MatchesCron(const std::string& expr, const std::tm& utc) {
    const auto fields = SplitFields(expr);
    if (fields.size() != 5) {
        return false;
    }
    // m h dom mon dow — dow: 0=Sun .. 6=Sat (tm_wday)
    try {
        return MatchField(fields[0], utc.tm_min, 0, 59) &&
               MatchField(fields[1], utc.tm_hour, 0, 23) &&
               MatchField(fields[2], utc.tm_mday, 1, 31) &&
               MatchField(fields[3], utc.tm_mon + 1, 1, 12) &&
               MatchField(fields[4], utc.tm_wday, 0, 6);
    } catch (...) {
        return false;
    }
}

void Scheduler::Add(Job job) { jobs_.push_back(std::move(job)); }

bool Scheduler::Remove(const std::string& id) {
    const auto n = jobs_.size();
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [&](const Job& j) { return j.id == id; }),
                jobs_.end());
    return jobs_.size() != n;
}

bool Scheduler::Update(const std::string& id, const Job& job) {
    for (auto& j : jobs_) {
        if (j.id == id) {
            j = job;
            j.id = id;
            return true;
        }
    }
    return false;
}

std::vector<Job> Scheduler::List() const { return jobs_; }

Job* Scheduler::Find(const std::string& id) {
    for (auto& j : jobs_) {
        if (j.id == id) {
            return &j;
        }
    }
    return nullptr;
}

nlohmann::json Scheduler::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& j : jobs_) {
        arr.push_back({{"id", j.id},
                       {"cron", j.cron_expr},
                       {"payload", j.payload},
                       {"enabled", j.enabled},
                       {"run_count", j.run_count},
                       {"last_run_utc", j.last_run_utc}});
    }
    return arr;
}

std::size_t Scheduler::Tick(std::chrono::system_clock::time_point now, TickHandler on_fire) {
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    const std::string key = MinuteKey(tm);
    std::size_t fired = 0;
    for (auto& j : jobs_) {
        if (!j.enabled) {
            continue;
        }
        if (!MatchesCron(j.cron_expr, tm)) {
            continue;
        }
        if (j.last_run_utc == key) {
            continue;  // already fired this minute
        }
        j.last_run_utc = key;
        ++j.run_count;
        ++fired;
        if (on_fire) {
            on_fire(j);
        }
    }
    return fired;
}

void Scheduler::RunDaemon(std::atomic<bool>& running, TickHandler on_fire,
                          std::chrono::milliseconds interval) {
    while (running.load()) {
        Tick(std::chrono::system_clock::now(), on_fire);
        std::this_thread::sleep_for(interval);
    }
}

}  // namespace openclaw::cron
