#pragma once

#include "openclaw/agent/types.hpp"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace openclaw::session {

// Upstream session.reset: mode "daily" (at atHour local time) or "idle" (idleMinutes); both may apply.
struct ResetPolicy {
    std::string mode = "daily";  // daily | idle | off
    int at_hour = 4;
    int idle_minutes = 0;        // >0 enables idle expiry (also with daily)
};
ResetPolicy PolicyFromConfig(const nlohmann::json& raw);
// Upstream precedence: session.resetByChannel.<channel> > session.resetByType.<direct|group|thread> > session.reset.
ResetPolicy PolicyFor(const nlohmann::json& raw, const std::string& channel, const std::string& chat_type);
// Infer direct|group|thread from an upstream-style session key (agent:main:<ch>:group:<id>, ...:thread:...).
std::string InferChatType(const std::string& session_key);

// Upstream session keys. dm_scope: main (all DMs share agent:<id>:main) | per-peer (agent:<id>:dm:<peer>) |
// per-channel-peer (agent:<id>:<channel>:dm:<peer>). Groups: agent:<id>:<channel>:group:<conv>; channels:
// agent:<id>:<channel>:channel:<conv>; threads append :thread:<id>.
struct KeyParts {
    std::string agent_id = "main";
    std::string channel, chat_type, peer_id, conversation_id, thread_id;
};
std::string BuildSessionKey(const KeyParts& k, const std::string& dm_scope = "main");
// True if a session last updated at `updated` must be reset at `now` under policy.
bool IsStale(const ResetPolicy& p, std::int64_t updated_ms, std::int64_t now_ms);

// "/new", "/reset" (optionally followed by text). Returns remainder in `rest`.
// Parse an upstream JSONL transcript (or legacy {messages:[...]} JSON).
std::vector<agent::Message> ParseTranscript(const std::filesystem::path& path);

bool ParseResetCommand(const std::string& text, std::string& rest);

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

    // sessions.json index: key -> {sessionId, createdAt, updatedAt, resets, messages}
    nlohmann::json IndexEntry(const std::string& key) const;
    nlohmann::json IndexJson() const;
    // Archive the current transcript to archive/ and start a fresh sessionId. Returns new entry.
    nlohmann::json Reset(const std::string& key, const std::string& reason) const;
    // Apply policy: if stale, Reset(key, "daily"|"idle"). Returns true if reset happened.
    bool ResetIfStale(const std::string& key, const ResetPolicy& p, std::int64_t now_ms = 0) const;

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
    std::filesystem::path WritePathFor(const std::string& id) const;
    std::filesystem::path LegacyPathFor(const std::string& id) const;
    void Touch(const std::string& key, std::size_t messages) const;
    nlohmann::json ReadIndex() const;
    void WriteIndex(const nlohmann::json& j) const;
    mutable std::recursive_mutex index_mu_;
    std::shared_ptr<std::recursive_mutex> MutexFor(const std::string& id) const;
    std::filesystem::path root_;
    mutable std::recursive_mutex map_mu_;
    mutable std::unordered_map<std::string, std::shared_ptr<std::recursive_mutex>> locks_;
};

}  // namespace openclaw::session
