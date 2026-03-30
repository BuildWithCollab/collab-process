#include "../platform.hpp"
#include "../running_process_impl.hpp"

#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace collab::process::detail {

struct UnixProcessImpl : RunningProcess::Impl {
    pid_t child_pid = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;

    std::move_only_function<void(std::string_view)> on_stdout;
    std::move_only_function<void(std::string_view)> on_stderr;

    std::string stdout_content;
    std::string stderr_content;
    bool waited = false;
    int cached_exit_code = -1;

    ~UnixProcessImpl() override {
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
    }

    auto pid() const -> int override { return static_cast<int>(child_pid); }

    auto is_alive() const -> bool override {
        if (child_pid <= 0) return false;
        return ::kill(child_pid, 0) == 0;
    }

    void read_pipes() {
        auto read_fd = [](int fd, std::string& out,
                          std::move_only_function<void(std::string_view)>& cb) {
            if (fd < 0) return;
            char buf[4096];
            ssize_t n;
            while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
                std::string_view chunk(buf, n);
                if (cb) cb(chunk);
                out.append(chunk);
            }
        };

        if (stdout_fd >= 0 && stderr_fd >= 0) {
            std::thread stderr_thread([&] {
                read_fd(stderr_fd, stderr_content, on_stderr);
            });
            read_fd(stdout_fd, stdout_content, on_stdout);
            stderr_thread.join();
        } else {
            read_fd(stdout_fd, stdout_content, on_stdout);
            read_fd(stderr_fd, stderr_content, on_stderr);
        }
    }

    auto do_wait(int wait_flags = 0) -> int {
        int status = 0;
        pid_t result = waitpid(child_pid, &status, wait_flags);
        if (result > 0 && WIFEXITED(status))
            return WEXITSTATUS(status);
        if (result > 0 && WIFSIGNALED(status))
            return 128 + WTERMSIG(status);
        return -1;
    }

    auto build_result(bool timed_out = false) -> Result {
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
        };
        trim(stdout_content);
        trim(stderr_content);

        return Result{
            .stdout_content = std::move(stdout_content),
            .stderr_content = std::move(stderr_content),
            .exit_code = timed_out ? -1 : cached_exit_code,
            .timed_out = timed_out,
        };
    }

    auto wait() -> std::expected<Result, SpawnError> override {
        if (!waited) {
            read_pipes();
            cached_exit_code = do_wait();
            waited = true;
        }
        return build_result();
    }

    auto wait_for(std::chrono::milliseconds timeout) -> std::expected<Result, SpawnError> override {
        if (!waited) {
            read_pipes();
            waited = true;
        }

        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            pid_t result = waitpid(child_pid, &status, WNOHANG);
            if (result != 0) {
                cached_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                return build_result();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        // Timeout — kill
        ::kill(child_pid, SIGKILL);
        waitpid(child_pid, nullptr, 0);
        return build_result(true);
    }

    auto stop(std::chrono::milliseconds grace) -> StopResult override {
        if (!is_alive()) return StopResult::not_running;

        ::kill(child_pid, SIGTERM);

        auto deadline = std::chrono::steady_clock::now() + grace;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!is_alive()) return StopResult::stopped_gracefully;
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        // Escalate
        ::kill(child_pid, SIGKILL);
        waitpid(child_pid, nullptr, 0);
        return StopResult::killed;
    }

    auto kill() -> bool override {
        if (!is_alive()) return false;
        ::kill(child_pid, SIGKILL);
        waitpid(child_pid, nullptr, 0);
        return true;
    }
};

auto platform_spawn(SpawnParams params)
    -> std::expected<std::unique_ptr<RunningProcess::Impl>, SpawnError> {

    auto impl = std::make_unique<UnixProcessImpl>();
    impl->on_stdout = std::move(params.on_stdout);
    impl->on_stderr = std::move(params.on_stderr);

    // Stdin pipe
    int stdin_pipe[2] = {-1, -1};
    bool pipe_stdin = !params.stdin_content.empty() || !params.stdin_path.empty();
    if (pipe_stdin && pipe(stdin_pipe) < 0)
        return std::unexpected(SpawnError{SpawnError::pipe_creation_failed, errno});

    // Stdout pipe
    int stdout_pipe[2] = {-1, -1};
    if (params.stdout_mode != CommandConfig::OutputMode::inherit) {
        if (pipe(stdout_pipe) < 0)
            return std::unexpected(SpawnError{SpawnError::pipe_creation_failed, errno});
    }

    // Stderr pipe
    int stderr_pipe[2] = {-1, -1};
    if (!params.stderr_merge && params.stderr_mode != CommandConfig::OutputMode::inherit) {
        if (pipe(stderr_pipe) < 0)
            return std::unexpected(SpawnError{SpawnError::pipe_creation_failed, errno});
    }

    pid_t child = fork();
    if (child < 0)
        return std::unexpected(SpawnError{SpawnError::platform_error, errno});

    if (child == 0) {
        // ── Child process ──

        // Stdin
        if (pipe_stdin) {
            close(stdin_pipe[1]);
            dup2(stdin_pipe[0], STDIN_FILENO);
            close(stdin_pipe[0]);
        } else if (params.stdin_closed) {
            close(STDIN_FILENO);
        }

        // Stdout
        if (stdout_pipe[1] >= 0) {
            close(stdout_pipe[0]);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            close(stdout_pipe[1]);
        } else if (params.stdout_mode == CommandConfig::OutputMode::discard) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
        }

        // Stderr
        if (params.stderr_merge) {
            dup2(STDOUT_FILENO, STDERR_FILENO);
        } else if (stderr_pipe[1] >= 0) {
            close(stderr_pipe[0]);
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stderr_pipe[1]);
        } else if (params.stderr_mode == CommandConfig::OutputMode::discard) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        }

        // Working directory
        if (!params.working_dir.empty())
            chdir(params.working_dir.c_str());

        // Environment
        for (auto& entry : params.env_entries) {
            auto eq = entry.find('=');
            if (eq != std::string::npos)
                setenv(entry.substr(0, eq).c_str(), entry.substr(eq + 1).c_str(), 1);
        }

        // Detach
        if (params.detached)
            setsid();

        // Build argv
        std::vector<const char*> argv;
        argv.push_back(params.resolved_program.c_str());
        for (auto& a : params.args)
            argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp(params.resolved_program.c_str(),
               const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // ── Parent process ──

    // Close child-side pipe ends
    if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
    if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
    if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);

    // Write stdin content in a thread to avoid deadlock
    if (stdin_pipe[1] >= 0 && !params.stdin_content.empty()) {
        std::thread([content = std::move(params.stdin_content), fd = stdin_pipe[1]] {
            write(fd, content.data(), content.size());
            close(fd);
        }).detach();
    } else if (stdin_pipe[1] >= 0) {
        close(stdin_pipe[1]);
    }

    impl->child_pid = child;
    impl->stdout_fd = stdout_pipe[0];
    impl->stderr_fd = stderr_pipe[0];

    return impl;
}

}  // namespace collab::process::detail
