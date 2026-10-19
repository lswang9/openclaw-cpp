#pragma once

#include <nlohmann/json.hpp>

#include <deque>
#include <mutex>
#include <string>

namespace openclaw::logging {

class TailBuffer {
public:
    explicit TailBuffer(std::size_t capacity = 500);
    void Append(const std::string& line);
    void Append(const std::string& level, const std::string& msg);
    nlohmann::json Tail(std::size_t n = 100) const;
    std::size_t Size() const;

private:
    mutable std::mutex mu_;
    std::size_t capacity_;
    std::deque<std::string> lines_;
};

TailBuffer& GlobalLog();

}  // namespace openclaw::logging
