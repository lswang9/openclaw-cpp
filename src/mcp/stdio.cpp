#include "openclaw/mcp/stdio.hpp"

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace openclaw::mcp {

StdioProcess::~StdioProcess() { Close(); }

bool StdioProcess::Spawn(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;
    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) return false;

    const pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }
    if (pid == 0) {
        // child: stdin <- in_pipe[0], stdout -> out_pipe[1]
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        std::vector<char*> av;
        for (const auto& s : argv) av.push_back(const_cast<char*>(s.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    // parent
    close(in_pipe[0]);
    close(out_pipe[1]);
    stdin_fd_ = in_pipe[1];
    stdout_fd_ = out_pipe[0];
    pid_ = pid;
    return true;
}

void StdioProcess::Close() {
    if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; }
    if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        int status = 0;
        waitpid(pid_, &status, 0);
        pid_ = -1;
    }
}

bool StdioProcess::Alive() const {
    if (pid_ <= 0) return false;
    int status = 0;
    const pid_t r = waitpid(pid_, &status, WNOHANG);
    return r == 0;
}

void StdioProcess::WriteLine(const std::string& line) {
    if (stdin_fd_ < 0) throw std::runtime_error("mcp stdio: not spawned");
    std::string msg = line;
    if (msg.empty() || msg.back() != '\n') msg.push_back('\n');
    const char* p = msg.data();
    size_t left = msg.size();
    while (left > 0) {
        const ssize_t n = write(stdin_fd_, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("mcp stdio write failed");
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
}

std::string StdioProcess::ReadLine() {
    if (stdout_fd_ < 0) throw std::runtime_error("mcp stdio: not spawned");
    for (;;) {
        auto pos = read_buf_.find('\n');
        if (pos != std::string::npos) {
            std::string line = read_buf_.substr(0, pos);
            read_buf_.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        char buf[4096];
        const ssize_t n = read(stdout_fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("mcp stdio read failed");
        }
        if (n == 0) {
            std::string line = read_buf_;
            read_buf_.clear();
            return line;
        }
        read_buf_.append(buf, static_cast<size_t>(n));
    }
}

std::shared_ptr<Client> MakeStdioClient(const std::vector<std::string>& argv) {
    auto proc = std::make_shared<StdioProcess>();
    if (!proc->Spawn(argv)) {
        throw std::runtime_error("failed to spawn mcp stdio process");
    }
    auto write = [proc](const std::string& line) { proc->WriteLine(line); };
    auto read = [proc]() { return proc->ReadLine(); };
    // Keep process alive via shared_ptr capture in lambdas — also store in client? 
    // Lambdas hold proc. Good.
    auto client = std::make_shared<Client>(write, read);
    // Anchor process lifetime: wrap by storing in a custom deleter holder
    // Client doesn't hold proc; lambdas do. Return client as-is.
    (void)proc;
    return client;
}

}  // namespace openclaw::mcp
