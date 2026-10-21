#pragma once
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

}  // namespace openclaw::tools
