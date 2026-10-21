#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace openclaw::tools {

using EnvVars = std::vector<std::pair<std::string, std::string>>;

// Per-run (thread-local) environment overlay. Never touches the process environment, so concurrent
// agent runs can't see each other's skill env/apiKeys. Child processes spawned via RunProcess get
// environ + overlay; in-process readers use LookupEnv().
class ScopedRunEnv {
public:
    explicit ScopedRunEnv(const EnvVars& vars);
    ~ScopedRunEnv();
    ScopedRunEnv(const ScopedRunEnv&) = delete;
    ScopedRunEnv& operator=(const ScopedRunEnv&) = delete;
private:
    EnvVars prev_;
};

const EnvVars& CurrentOverlay();
// Overlay first, then process env. Empty if unset.
std::string LookupEnv(const std::string& key);

struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string out;   // stdout+stderr interleaved, capped
    bool truncated = false;
    std::string error;
};
// /bin/sh -c cmd in cwd with environ + overlay; kills the process group on timeout.
ProcessResult RunProcess(const std::string& cmd, const std::string& cwd, int timeout_ms = 60000,
                         std::size_t max_out = 64 * 1024);

// Detached process for exec background/yieldMs. Caller owns the handle; Wait fills result.
struct BackgroundHandle {
    int pid = -1;
    int read_fd = -1;  // stdout+stderr pipe (caller closes after Wait); PTY master when pty
    int write_fd = -1; // stdin of the child (PTY master dup) — only when pty=true
    bool pty = false;
};
// Fork+exec /bin/sh -c; returns handle with open read_fd. timeout applied in WaitBackground.
BackgroundHandle StartBackground(const std::string& cmd, const std::string& cwd, bool pty = false);
// Translate tmux-style key names (Enter, C-c, Up, Tab, Escape, BSpace, M-x, F1..F12) to bytes; unknown → literal.
std::string KeysToBytes(const std::vector<std::string>& keys);
// Drain output until EOF or timeout_ms; waitpid; close read_fd.
ProcessResult WaitBackground(BackgroundHandle& h, int timeout_ms, std::size_t max_out = 200 * 1024,
                             const std::function<void(const char*, std::size_t)>& on_chunk = {});
// Like WaitBackground but on timeout leaves the process running (pid/read_fd stay set).
ProcessResult CollectBackground(BackgroundHandle& h, int timeout_ms, std::size_t max_out = 200 * 1024);
void KillBackground(BackgroundHandle& h);
// Resize a PTY session (ioctl TIOCSWINSZ on the master). No-op / false when not a pty.
bool ResizePty(BackgroundHandle& h, int rows, int cols);

}  // namespace openclaw::tools
