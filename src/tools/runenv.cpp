#include "openclaw/tools/runenv.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace openclaw::tools {

namespace { thread_local EnvVars g_overlay; }

ScopedRunEnv::ScopedRunEnv(const EnvVars& vars) : prev_(g_overlay) {
    for (const auto& kv : vars) {
        bool replaced = false;
        for (auto& o : g_overlay) if (o.first == kv.first) { o.second = kv.second; replaced = true; }
        if (!replaced) g_overlay.push_back(kv);
    }
}
ScopedRunEnv::~ScopedRunEnv() { g_overlay = std::move(prev_); }

const EnvVars& CurrentOverlay() { return g_overlay; }

std::string LookupEnv(const std::string& key) {
    for (const auto& o : g_overlay) if (o.first == key) return o.second;
    const char* v = std::getenv(key.c_str());
    return v ? v : "";
}

ProcessResult RunProcess(const std::string& cmd, const std::string& cwd, int timeout_ms, std::size_t max_out) {
    ProcessResult r;
    // Build envp before fork (no allocation after fork).
    std::vector<std::string> envs;
    for (char** e = environ; e && *e; ++e) {
        std::string s = *e;
        auto eq = s.find('=');
        bool shadow = false;
        for (const auto& o : g_overlay) if (eq != std::string::npos && s.compare(0, eq, o.first) == 0 && eq == o.first.size()) shadow = true;
        if (!shadow) envs.push_back(std::move(s));
    }
    for (const auto& o : g_overlay) envs.push_back(o.first + "=" + o.second);
    std::vector<char*> envp;
    for (auto& s : envs) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::string sh = "/bin/sh", dash_c = "-c", c = cmd;
    char* argv[] = {sh.data(), dash_c.data(), c.data(), nullptr};

    int fds[2];
    if (pipe(fds) != 0) { r.error = "pipe failed"; return r; }
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); r.error = "fork failed"; return r; }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1); dup2(fds[1], 2);
        close(fds[0]); close(fds[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        execve("/bin/sh", argv, envp.data());
        _exit(127);
    }
    close(fds[1]);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { r.timed_out = true; kill(-pid, SIGKILL); break; }
        pollfd p{fds[0], POLLIN, 0};
        int pr = poll(&p, 1, static_cast<int>(std::min<long long>(left, 200)));
        if (pr > 0) {
            ssize_t n = read(fds[0], buf, sizeof buf);
            if (n <= 0) break;
            if (r.out.size() < max_out) r.out.append(buf, std::min<std::size_t>(n, max_out - r.out.size()));
            else r.truncated = true;
        }
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

BackgroundHandle StartBackground(const std::string& cmd, const std::string& cwd) {
    BackgroundHandle h;
    std::vector<std::string> envs;
    for (char** e = environ; e && *e; ++e) {
        std::string s = *e;
        auto eq = s.find('=');
        bool shadow = false;
        for (const auto& o : g_overlay)
            if (eq != std::string::npos && s.compare(0, eq, o.first) == 0 && eq == o.first.size()) shadow = true;
        if (!shadow) envs.push_back(std::move(s));
    }
    for (const auto& o : g_overlay) envs.push_back(o.first + "=" + o.second);
    envs.push_back("OPENCLAW_SHELL=exec");
    std::vector<char*> envp;
    for (auto& s : envs) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::string sh = "/bin/sh", dash_c = "-c", c = cmd;
    char* argv[] = {sh.data(), dash_c.data(), c.data(), nullptr};
    int fds[2];
    if (pipe(fds) != 0) return h;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return h; }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1); dup2(fds[1], 2);
        close(fds[0]); close(fds[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        execve("/bin/sh", argv, envp.data());
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    h.pid = pid;
    h.read_fd = fds[0];
    return h;
}

ProcessResult WaitBackground(BackgroundHandle& h, int timeout_ms, std::size_t max_out) {
    ProcessResult r;
    if (h.pid < 0 || h.read_fd < 0) { r.error = "invalid background handle"; return r; }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms <= 0 ? 86400000 : timeout_ms);
    char buf[4096];
    for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (timeout_ms > 0 && left <= 0) { r.timed_out = true; kill(-h.pid, SIGKILL); break; }
        pollfd p{h.read_fd, POLLIN, 0};
        int pr = poll(&p, 1, static_cast<int>(std::min<long long>(timeout_ms > 0 ? left : 200, 200)));
        if (pr > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(h.read_fd, buf, sizeof buf);
            if (n > 0) {
                if (r.out.size() < max_out) r.out.append(buf, std::min<std::size_t>(n, max_out - r.out.size()));
                else r.truncated = true;
            } else if (n == 0) break;
        }
        int status = 0;
        pid_t w = waitpid(h.pid, &status, WNOHANG);
        if (w == h.pid) {
            // drain remaining
            for (;;) {
                ssize_t n = read(h.read_fd, buf, sizeof buf);
                if (n <= 0) break;
                if (r.out.size() < max_out) r.out.append(buf, std::min<std::size_t>(n, max_out - r.out.size()));
                else r.truncated = true;
            }
            r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
            close(h.read_fd); h.read_fd = -1; h.pid = -1;
            return r;
        }
    }
    int status = 0;
    waitpid(h.pid, &status, 0);
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (h.read_fd >= 0) { close(h.read_fd); h.read_fd = -1; }
    h.pid = -1;
    return r;
}


ProcessResult CollectBackground(BackgroundHandle& h, int timeout_ms, std::size_t max_out) {
    ProcessResult r;
    if (h.pid < 0 || h.read_fd < 0) { r.error = "invalid background handle"; return r; }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
    char buf[4096];
    for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { r.timed_out = true; return r; }  // still running
        pollfd p{h.read_fd, POLLIN, 0};
        int pr = poll(&p, 1, static_cast<int>(std::min<long long>(left, 50)));
        if (pr > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(h.read_fd, buf, sizeof buf);
            if (n > 0) {
                if (r.out.size() < max_out) r.out.append(buf, std::min<std::size_t>(n, max_out - r.out.size()));
                else r.truncated = true;
            } else if (n == 0) {
                // EOF — process closed pipes; wait
                int status = 0;
                waitpid(h.pid, &status, 0);
                r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                close(h.read_fd); h.read_fd = -1; h.pid = -1;
                return r;
            }
        }
        int status = 0;
        pid_t w = waitpid(h.pid, &status, WNOHANG);
        if (w == h.pid) {
            for (;;) {
                ssize_t n = read(h.read_fd, buf, sizeof buf);
                if (n <= 0) break;
                if (r.out.size() < max_out) r.out.append(buf, std::min<std::size_t>(n, max_out - r.out.size()));
                else r.truncated = true;
            }
            r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            close(h.read_fd); h.read_fd = -1; h.pid = -1;
            return r;
        }
    }
}

void KillBackground(BackgroundHandle& h) {
    if (h.pid > 0) kill(-h.pid, SIGTERM);
}

}  // namespace openclaw::tools
