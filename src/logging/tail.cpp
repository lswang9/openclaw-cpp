#include "openclaw/logging/tail.hpp"

#include <chrono>

namespace openclaw::logging {

TailBuffer::TailBuffer(std::size_t capacity) : capacity_(capacity) {}

void TailBuffer::Append(const std::string& line) {
    std::lock_guard lock(mu_);
    lines_.push_back(line);
    while (lines_.size() > capacity_) lines_.pop_front();
}

void TailBuffer::Append(const std::string& level, const std::string& msg) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    Append(std::to_string(ms) + " [" + level + "] " + msg);
}

nlohmann::json TailBuffer::Tail(std::size_t n) const {
    std::lock_guard lock(mu_);
    nlohmann::json arr = nlohmann::json::array();
    const std::size_t start = lines_.size() > n ? lines_.size() - n : 0;
    for (std::size_t i = start; i < lines_.size(); ++i) arr.push_back(lines_[i]);
    return arr;
}

std::size_t TailBuffer::Size() const {
    std::lock_guard lock(mu_);
    return lines_.size();
}

TailBuffer& GlobalLog() {
    static TailBuffer buf(1000);
    return buf;
}

}  // namespace openclaw::logging
