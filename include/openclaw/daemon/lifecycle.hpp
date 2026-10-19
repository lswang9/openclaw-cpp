#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace openclaw::daemon {

std::filesystem::path DefaultPidPath();
bool WritePidFile(const std::filesystem::path& path, int pid);
std::optional<int> ReadPidFile(const std::filesystem::path& path);
void RemovePidFile(const std::filesystem::path& path);
bool ProcessAlive(int pid);
// Send SIGTERM; returns true if signal delivered.
bool StopPid(int pid);
// Install SIGINT/SIGTERM → invoke callback once (async-signal-safe via flag).
void InstallStopSignals(void (*on_stop)());

}  // namespace openclaw::daemon
