#include "openclaw/daemon/lifecycle.hpp"

#include <csignal>
#include <fstream>
#include <sys/types.h>
#include <unistd.h>

namespace openclaw::daemon {
namespace {
void (*g_on_stop)() = nullptr;
void HandleSignal(int) {
    if (g_on_stop) g_on_stop();
}
}  // namespace

std::filesystem::path DefaultPidPath() {
    const char* e = std::getenv("OPENCLAW_PIDFILE");
    if (e && *e) return e;
    const char* data = std::getenv("OPENCLAW_DATA_DIR");
    if (data && *data) return std::filesystem::path(data) / "gateway.pid";
    const char* home = std::getenv("HOME");
    if (home && *home) return std::filesystem::path(home) / ".openclaw-cpp" / "gateway.pid";
    return "/tmp/openclaw-cpp-gateway.pid";
}

bool WritePidFile(const std::filesystem::path& path, int pid) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) return false;
    out << pid << '\n';
    return static_cast<bool>(out);
}

std::optional<int> ReadPidFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    int pid = 0;
    in >> pid;
    if (pid <= 0) return std::nullopt;
    return pid;
}

void RemovePidFile(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

bool ProcessAlive(int pid) {
    if (pid <= 0) return false;
    return ::kill(pid, 0) == 0;
}

bool StopPid(int pid) {
    if (pid <= 0) return false;
    return ::kill(pid, SIGTERM) == 0;
}

void InstallStopSignals(void (*on_stop)()) {
    g_on_stop = on_stop;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
}

}  // namespace openclaw::daemon
