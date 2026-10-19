#pragma once

#include "openclaw/agent/types.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace openclaw::session {

// File-backed session store with per-id mutexes for concurrent gateway requests.
class Store {
public:
    explicit Store(std::filesystem::path root);

    std::filesystem::path Root() const { return root_; }
    std::vector<std::string> List() const;
    bool Exists(const std::string& id) const;
    std::vector<agent::Message> Load(const std::string& id) const;
    void Save(const std::string& id, const std::vector<agent::Message>& messages) const;
    void Append(const std::string& id, const agent::Message& message) const;
    bool Remove(const std::string& id) const;

    // RAII lock for a session id (shared across Load/Save of same id).
    class Guard {
    public:
        Guard() = default;
        explicit Guard(std::shared_ptr<std::recursive_mutex> m) : m_(std::move(m)) {
            if (m_) m_->lock();
        }
        Guard(Guard&& o) noexcept : m_(std::move(o.m_)) {}
        Guard& operator=(Guard&& o) noexcept {
            if (this != &o) {
                unlock();
                m_ = std::move(o.m_);
            }
            return *this;
        }
        ~Guard() { unlock(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        void unlock() {
            if (m_) {
                m_->unlock();
                m_.reset();
            }
        }
        std::shared_ptr<std::recursive_mutex> m_;
    };

    Guard Lock(const std::string& id) const;

private:
    std::filesystem::path PathFor(const std::string& id) const;
    std::shared_ptr<std::recursive_mutex> MutexFor(const std::string& id) const;
    std::filesystem::path root_;
    mutable std::recursive_mutex map_mu_;
    mutable std::unordered_map<std::string, std::shared_ptr<std::recursive_mutex>> locks_;
};

}  // namespace openclaw::session
