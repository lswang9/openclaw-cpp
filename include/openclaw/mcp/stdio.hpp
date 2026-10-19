#pragma once

#include "openclaw/mcp/client.hpp"

#include <memory>
#include <string>
#include <vector>

namespace openclaw::mcp {

// Owns a child process with stdin/stdout newline-delimited JSON-RPC.
class StdioProcess {
public:
    StdioProcess() = default;
    ~StdioProcess();
    StdioProcess(const StdioProcess&) = delete;
    StdioProcess& operator=(const StdioProcess&) = delete;

    // argv[0] is executable path. Returns false on spawn failure.
    bool Spawn(const std::vector<std::string>& argv);
    void Close();
    bool Alive() const;

    void WriteLine(const std::string& line);
    std::string ReadLine();  // blocking until newline or EOF

    int pid() const { return pid_; }

private:
    int pid_ = -1;
    int stdin_fd_ = -1;   // parent writes
    int stdout_fd_ = -1;  // parent reads
    std::string read_buf_;
};

std::shared_ptr<Client> MakeStdioClient(const std::vector<std::string>& argv);

}  // namespace openclaw::mcp
