#include "../platform.hpp"
#include "../running_process_impl.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace collab::process::detail {

// ── Supervisor protocol ───────────────────────────────────────────────────
//
// Unix platform_spawn double-forks. The intermediate process (the supervisor)
// stays alive between the library and the user's target. The supervisor owns
// lifecycle: if the library dies abruptly, the supervisor SIGKILLs the target
// so it doesn't orphan to PID 1 — matching Windows' Job Object kill-on-close
// contract across all three platforms.
//
// Three pipes from library to supervisor. All CLOEXEC so grandchild never
// inherits them:
//
//   info pipe      — supervisor writes, library reads. Two framed messages:
//                      message 1: tag=0 + pid_t (spawn ok) or tag=1 + int
//                                 (spawn error errno). Written after supervisor
//                                 learns the grandchild's execve result.
//                      message 2: tag=2 + int flag + int payload (target exit).
//                                 flag 0 = normal exit, payload = exit code.
//                                 flag 1 = killed by signal, payload = signal.
//                                 Written when the target exits naturally (or
//                                 via the library's kill()/terminate()).
//   lifecycle pipe — library writes-end open for life; supervisor read-end
//                    blocks. Library death → EOF → supervisor kills target.
//   release pipe   — library writes one byte to signal detach(); supervisor's
//                    read-end wakes and the supervisor _exit(0)s *without*
//                    touching the target. Target reparents to PID 1.
//
// Exec-probe pipe: internal to the supervisor/grandchild pair, CLOEXEC.
// Grandchild holds the write end before execve; successful execve → kernel
// closes it → supervisor reads EOF. Failed execve → grandchild writes errno
// and _exits → supervisor reads the errno.
//
// pgrp ordering matters. Supervisor forks grandchild while still in the
// library's pgrp, so grandchild inherits it (preserving "all-inherit children
// share the parent's pgrp"). Grandchild then does its own setpgid(0, 0) when
// signalable. *After* the grandchild exists, the supervisor moves itself to
// its own pgrp so an external kill against the library's pgrp doesn't take
// the supervisor with it.

// POSIX guarantees writes ≤ PIPE_BUF are atomic; all our messages are
// well under that. We write the whole message in a single ::write() call.
struct alignas(1) InfoMsgSpawnOk {
    unsigned char tag;  // 0
    pid_t target_pid;
};
struct alignas(1) InfoMsgSpawnError {
    unsigned char tag;  // 1
    int native_errno;
};
struct alignas(1) InfoMsgExit {
    unsigned char tag;  // 2
    int flag;           // 0 = normal, 1 = signalled
    int payload;        // exit code or signal number
};

// pipe2 with O_CLOEXEC is available on Linux since glibc 2.9 and on macOS
// since 10.10. Wrap for a single call site.
static auto make_cloexec_pipe(int fds[2]) -> bool {
    return ::pipe2(fds, O_CLOEXEC) == 0;
}

// Write the whole buffer or give up — used for the info pipe where a short
// write means protocol corruption. Retries on EINTR.
static auto write_all(int fd, const void* buf, size_t n) -> bool {
    const char* p = static_cast<const char*>(buf);
    size_t left = n;
    while (left > 0) {
        ssize_t w = ::write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        left -= static_cast<size_t>(w);
    }
    return true;
}

// Read exactly n bytes or fail — short reads / EOF before n means the writer
// died mid-message (protocol violation) or never wrote at all.
static auto read_all(int fd, void* buf, size_t n) -> bool {
    char* p = static_cast<char*>(buf);
    size_t left = n;
    while (left > 0) {
        ssize_t r = ::read(fd, p, left);
        if (r == 0) return false;  // EOF before message complete
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        left -= static_cast<size_t>(r);
    }
    return true;
}

// ── Supervisor body ───────────────────────────────────────────────────────
//
// Runs in the intermediate fork. Never returns — always _exit.
//
// Preconditions on entry:
//   - Grandchild has been forked and has pid = target_pid.
//   - Exec-probe read-end is exec_probe_read; write-end is closed in supervisor.
//   - Supervisor has not yet called setpgid.
//   - Grandchild has its own pgrp iff has_own_group; else shares library pgrp.
[[noreturn]] static void supervisor_main(pid_t target_pid,
                                         int info_write,
                                         int lifecycle_read,
                                         int release_read,
                                         int exec_probe_read,
                                         bool has_own_group) {
    // Wait for exec-probe: EOF means execve succeeded, bytes mean errno.
    int exec_errno = 0;
    {
        char buf[sizeof(int)];
        ssize_t r = ::read(exec_probe_read, buf, sizeof(buf));
        if (r == sizeof(int)) {
            std::memcpy(&exec_errno, buf, sizeof(int));
        } else if (r > 0) {
            // Truncated: grandchild died before a full errno landed.
            exec_errno = EIO;
        }
        // r == 0 → EOF → execve succeeded.
    }
    ::close(exec_probe_read);

    if (exec_errno != 0) {
        // Spawn error — tell the library, reap the grandchild corpse, exit.
        InfoMsgSpawnError msg{1, exec_errno};
        (void)write_all(info_write, &msg, sizeof(msg));
        (void)::close(info_write);
        (void)::waitpid(target_pid, nullptr, 0);
        _exit(0);
    }

    // Spawn ok — tell the library the target PID.
    InfoMsgSpawnOk ok{0, target_pid};
    if (!write_all(info_write, &ok, sizeof(ok))) {
        // Library died before we could tell it — kill target and bail.
        pid_t sig_target = has_own_group ? -target_pid : target_pid;
        (void)::kill(sig_target, SIGKILL);
        (void)::waitpid(target_pid, nullptr, 0);
        _exit(1);
    }

    // Move supervisor to its own pgrp *now* — the grandchild's pgrp has
    // already been fixed (either inherited from library or set by the
    // grandchild itself pre-execve). Isolating the supervisor means an
    // external tree-kill of the library's pgrp won't take the supervisor
    // with it and strand the target.
    (void)::setpgid(0, 0);

    // Poll loop. Watch lifecycle and release pipes; poll the target with
    // waitpid(WNOHANG) on each wake.
    pollfd fds[2];
    fds[0].fd = lifecycle_read;
    fds[0].events = POLLIN;
    fds[1].fd = release_read;
    fds[1].events = POLLIN;

    for (;;) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        // 50ms timeout is short enough to detect natural target exit quickly
        // without busy-waiting. Target exit has no fd signal here — we poll
        // via waitpid(WNOHANG) every tick.
        int n = ::poll(fds, 2, 50);

        if (n < 0) {
            if (errno == EINTR) continue;
            // poll broke — fail safe by killing target.
            pid_t sig_target = has_own_group ? -target_pid : target_pid;
            (void)::kill(sig_target, SIGKILL);
            (void)::waitpid(target_pid, nullptr, 0);
            _exit(1);
        }

        // Release wins over lifecycle when both land in the same epoch
        // (library wrote the release byte, then died). User intent was
        // detach, not tear-down.
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            // detach() — let the target go.
            (void)::close(info_write);
            _exit(0);
        }

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            // Library died — kill the target.
            pid_t sig_target = has_own_group ? -target_pid : target_pid;
            (void)::kill(sig_target, SIGKILL);
            (void)::waitpid(target_pid, nullptr, 0);
            _exit(0);
        }

        // Target exited on its own?
        int status = 0;
        pid_t r = ::waitpid(target_pid, &status, WNOHANG);
        if (r == target_pid) {
            InfoMsgExit msg{2, 0, 0};
            if (WIFEXITED(status)) {
                msg.flag = 0;
                msg.payload = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                msg.flag = 1;
                msg.payload = WTERMSIG(status);
            }
            (void)write_all(info_write, &msg, sizeof(msg));
            (void)::close(info_write);
            _exit(0);
        }
    }
}

struct UnixProcessImpl : RunningProcess::Impl {
    // Supervisor is our direct child; target is the grandchild we report to
    // the user. pid() returns target_pid. waitpid reaps supervisor_pid.
    pid_t supervisor_pid = -1;
    pid_t target_pid = -1;

    int stdout_fd = -1;
    int stderr_fd = -1;

    // Library-side fds of the three protocol pipes. All CLOEXEC.
    int info_fd = -1;       // read end of info pipe
    int lifecycle_fd = -1;  // write end of lifecycle pipe (held open for life)
    int release_fd = -1;    // write end of release pipe

    // Matches the setpgid branch taken inside the supervisor/grandchild —
    // terminate/kill consult this to decide between killpg(-pid) and a
    // direct signal.
    bool has_own_group = false;

    collab::process::move_only_function<void(std::string_view)> on_stdout;
    collab::process::move_only_function<void(std::string_view)> on_stderr;

    std::string stdout_content;
    std::string stderr_content;
    bool waited = false;
    int cached_exit_code = -1;

    std::thread pipe_reader_thread;

    void join_pipe_reader() {
        if (pipe_reader_thread.joinable())
            pipe_reader_thread.join();
    }

    // Reap the supervisor and pull the target's exit status off the info
    // pipe. Caller must have already arranged for the supervisor to exit
    // (either because the target died, or because we killed the target).
    void reap_supervisor_and_read_exit() {
        if (supervisor_pid <= 0) return;
        int sup_status = 0;
        if (::waitpid(supervisor_pid, &sup_status, 0) > 0) {
            if (WIFEXITED(sup_status) && WEXITSTATUS(sup_status) == 0) {
                InfoMsgExit msg{};
                if (info_fd >= 0 && read_all(info_fd, &msg, sizeof(msg))
                        && msg.tag == 2) {
                    cached_exit_code = msg.flag == 0 ? msg.payload
                                                     : (128 + msg.payload);
                } else {
                    cached_exit_code = -1;
                }
            } else {
                // Supervisor died abnormally — target's fate is unknown.
                cached_exit_code = -1;
            }
        }
        supervisor_pid = -1;
    }

    ~UnixProcessImpl() override {
        // Reader must drain before we close the fds it reads.
        join_pipe_reader();
        if (stdout_fd >= 0) ::close(stdout_fd);
        if (stderr_fd >= 0) ::close(stderr_fd);

        // If we still own the supervisor, closing the lifecycle pipe makes
        // it SIGKILL the target. Reap to avoid leaving a zombie.
        if (lifecycle_fd >= 0) { ::close(lifecycle_fd); lifecycle_fd = -1; }
        if (release_fd >= 0)   { ::close(release_fd);   release_fd   = -1; }
        if (supervisor_pid > 0) {
            (void)::waitpid(supervisor_pid, nullptr, 0);
            supervisor_pid = -1;
        }
        if (info_fd >= 0) { ::close(info_fd); info_fd = -1; }
    }

    auto pid() const -> int override { return static_cast<int>(target_pid); }

    auto is_alive() const -> bool override {
        if (target_pid <= 0) return false;
        return ::kill(target_pid, 0) == 0;
    }

    void read_pipes() {
        auto read_fd = [](int fd, std::string& out,
                          collab::process::move_only_function<void(std::string_view)>& cb) {
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
            .exit_code = timed_out ? std::optional<int>{} : std::optional<int>{cached_exit_code},
            .timed_out = timed_out,
        };
    }

    auto wait() -> std::expected<Result, SpawnError> override {
        if (!waited) {
            reap_supervisor_and_read_exit();
            // Target exited → pipe write-ends closed → reader sees EOF → exits.
            join_pipe_reader();
            waited = true;
        }
        return build_result();
    }

    // Poll only: returns nullopt if still running. Does NOT kill on timeout.
    auto wait_for(std::chrono::milliseconds timeout) -> std::optional<Result> override {
        if (waited) return build_result();

        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            int sup_status = 0;
            pid_t r = ::waitpid(supervisor_pid, &sup_status, WNOHANG);
            if (r != 0) {
                if (r > 0 && WIFEXITED(sup_status) && WEXITSTATUS(sup_status) == 0) {
                    InfoMsgExit msg{};
                    if (info_fd >= 0 && read_all(info_fd, &msg, sizeof(msg))
                            && msg.tag == 2) {
                        cached_exit_code = msg.flag == 0 ? msg.payload
                                                         : (128 + msg.payload);
                    } else {
                        cached_exit_code = -1;
                    }
                } else {
                    cached_exit_code = -1;
                }
                supervisor_pid = -1;
                join_pipe_reader();
                waited = true;
                return build_result();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        return std::nullopt;
    }

    // Used by run() — kills the target on timeout. Background reader prevents
    // deadlock when the child fills a capture pipe.
    auto wait_for_and_kill(std::chrono::milliseconds timeout) -> std::expected<Result, SpawnError> override {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        bool exited = false;
        while (std::chrono::steady_clock::now() < deadline) {
            int sup_status = 0;
            pid_t r = ::waitpid(supervisor_pid, &sup_status, WNOHANG);
            if (r != 0) {
                if (r > 0 && WIFEXITED(sup_status) && WEXITSTATUS(sup_status) == 0) {
                    InfoMsgExit msg{};
                    if (info_fd >= 0 && read_all(info_fd, &msg, sizeof(msg))
                            && msg.tag == 2) {
                        cached_exit_code = msg.flag == 0 ? msg.payload
                                                         : (128 + msg.payload);
                    } else {
                        cached_exit_code = -1;
                    }
                } else {
                    cached_exit_code = -1;
                }
                supervisor_pid = -1;
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        if (!exited) {
            // Timeout → kill target; supervisor reacts and exits; we reap.
            pid_t sig_target = has_own_group ? -target_pid : target_pid;
            (void)::kill(sig_target, SIGKILL);
            reap_supervisor_and_read_exit();
        }

        join_pipe_reader();
        waited = true;

        return build_result(!exited);
    }

    auto terminate() -> bool override {
        // Non-signalable children share the terminal's signal path already —
        // Ctrl+C is the user-facing tool; terminate() is a no-op so the
        // public contract matches Windows' CREATE_NEW_PROCESS_GROUP rule.
        if (!has_own_group) return false;
        if (!is_alive()) return false;
        return ::kill(-target_pid, SIGTERM) == 0;
    }

    auto interrupt() -> bool override {
        if (!has_own_group) return false;
        if (!is_alive()) return false;
        return ::kill(-target_pid, SIGINT) == 0;
    }

    auto kill() -> bool override {
        if (!is_alive()) return false;
        pid_t sig_target = has_own_group ? -target_pid : target_pid;
        bool ok = ::kill(sig_target, SIGKILL) == 0;
        // Reap the supervisor (which sees the target exit and shuts down).
        // Join the reader before flagging waited so any later wait_for()
        // doesn't race with the reader draining the final bytes.
        if (!waited) {
            reap_supervisor_and_read_exit();
            join_pipe_reader();
            waited = true;
        }
        return ok;
    }

    void release_for_detach() override {
        // Tell the supervisor to stop watching and exit *without* touching
        // the target. Sequence matters: write the release byte while the
        // lifecycle pipe is still open, then reap the supervisor, *then*
        // close lifecycle — otherwise a lifecycle-EOF race could kill the
        // target before the supervisor sees the release byte.
        if (release_fd >= 0) {
            unsigned char byte = 1;
            (void)::write(release_fd, &byte, 1);
            ::close(release_fd);
            release_fd = -1;
        }
        if (supervisor_pid > 0) {
            (void)::waitpid(supervisor_pid, nullptr, 0);
            supervisor_pid = -1;
        }
        if (lifecycle_fd >= 0) { ::close(lifecycle_fd); lifecycle_fd = -1; }
        if (info_fd >= 0)      { ::close(info_fd);      info_fd      = -1; }

        // Reader would block on the detached child's pipes forever — close
        // our read ends so the blocked read unblocks, then join. Matches
        // the pre-supervisor behavior.
        if (pipe_reader_thread.joinable()) {
            if (stdout_fd >= 0) { ::close(stdout_fd); stdout_fd = -1; }
            if (stderr_fd >= 0) { ::close(stderr_fd); stderr_fd = -1; }
            pipe_reader_thread.join();
        }
        waited = true;
    }
};

auto platform_spawn(SpawnParams params)
    -> std::expected<std::unique_ptr<RunningProcess::Impl>, SpawnError> {

    auto impl = std::make_unique<UnixProcessImpl>();
    impl->on_stdout = std::move(params.on_stdout);
    impl->on_stderr = std::move(params.on_stderr);
    impl->has_own_group = params.signalable;

    // ── Library-owned pipes. All O_CLOEXEC so the grandchild never inherits
    //    them — dup2 onto stdin/stdout/stderr clears the flag on the dup
    //    targets, which is how the target still sees its stdio.

    int stdin_pipe[2] = {-1, -1};
    bool pipe_stdin = (params.stdin_mode == CommandConfig::StdinMode::content)
                   || (params.stdin_mode == CommandConfig::StdinMode::file);
    if (pipe_stdin && !make_cloexec_pipe(stdin_pipe))
        return std::unexpected(SpawnError{SpawnError::pipe_creation_failed, errno});

    int stdout_pipe[2] = {-1, -1};
    if (params.stdout_mode == CommandConfig::OutputMode::capture) {
        if (!make_cloexec_pipe(stdout_pipe))
            return std::unexpected(SpawnError{SpawnError::pipe_creation_failed, errno});
    }

    int stderr_pipe[2] = {-1, -1};
    if (!params.stderr_merge && params.stderr_mode == CommandConfig::OutputMode::capture) {
        if (!make_cloexec_pipe(stderr_pipe))
            return std::unexpected(SpawnError{SpawnError::pipe_creation_failed, errno});
    }

    // Supervisor protocol pipes.
    int info_pipe[2] = {-1, -1};       // [0] library reads, [1] supervisor writes
    int lifecycle_pipe[2] = {-1, -1};  // [0] supervisor reads, [1] library writes
    int release_pipe[2] = {-1, -1};    // [0] supervisor reads, [1] library writes
    int exec_probe[2] = {-1, -1};      // [0] supervisor reads, [1] grandchild writes

    auto close_all = [&](int* p) { if (p[0] >= 0) ::close(p[0]); if (p[1] >= 0) ::close(p[1]); };
    auto cleanup_all_pipes = [&]() {
        close_all(stdin_pipe); close_all(stdout_pipe); close_all(stderr_pipe);
        close_all(info_pipe); close_all(lifecycle_pipe); close_all(release_pipe);
        close_all(exec_probe);
    };

    if (!make_cloexec_pipe(info_pipe)
        || !make_cloexec_pipe(lifecycle_pipe)
        || !make_cloexec_pipe(release_pipe)
        || !make_cloexec_pipe(exec_probe)) {
        int e = errno;
        cleanup_all_pipes();
        return std::unexpected(SpawnError{SpawnError::pipe_creation_failed, e});
    }

    pid_t supervisor = ::fork();
    if (supervisor < 0) {
        int e = errno;
        cleanup_all_pipes();
        return std::unexpected(SpawnError{SpawnError::platform_error, e});
    }

    if (supervisor == 0) {
        // ── Supervisor process ──
        // Close library-side ends of the protocol pipes.
        ::close(info_pipe[0]);
        ::close(lifecycle_pipe[1]);
        ::close(release_pipe[1]);

        // Fork the grandchild (the real target).
        pid_t grandchild = ::fork();
        if (grandchild < 0) {
            // Second fork failed — report to library and exit.
            InfoMsgSpawnError msg{1, errno};
            (void)write_all(info_pipe[1], &msg, sizeof(msg));
            _exit(1);
        }

        if (grandchild == 0) {
            // ── Grandchild (target) ──
            // Close the supervisor/library pipes the target doesn't need.
            // (stdio child-ends are still needed until after dup2.)
            ::close(info_pipe[1]);
            ::close(lifecycle_pipe[0]);
            ::close(release_pipe[0]);
            ::close(exec_probe[0]);
            // All other library pipes are CLOEXEC and will close on execve,
            // but we explicitly close the child-ends we inherited for stdio.

            // Stdin
            if (pipe_stdin) {
                ::close(stdin_pipe[1]);
                ::dup2(stdin_pipe[0], STDIN_FILENO);
                ::close(stdin_pipe[0]);
            } else if (params.stdin_mode == CommandConfig::StdinMode::closed) {
                ::close(STDIN_FILENO);
            }

            // Stdout
            if (stdout_pipe[1] >= 0) {
                ::close(stdout_pipe[0]);
                ::dup2(stdout_pipe[1], STDOUT_FILENO);
                ::close(stdout_pipe[1]);
            } else if (params.stdout_mode == CommandConfig::OutputMode::discard) {
                int devnull = ::open("/dev/null", O_WRONLY);
                if (devnull >= 0) { ::dup2(devnull, STDOUT_FILENO); ::close(devnull); }
            }

            // Stderr
            if (params.stderr_merge) {
                ::dup2(STDOUT_FILENO, STDERR_FILENO);
            } else if (stderr_pipe[1] >= 0) {
                ::close(stderr_pipe[0]);
                ::dup2(stderr_pipe[1], STDERR_FILENO);
                ::close(stderr_pipe[1]);
            } else if (params.stderr_mode == CommandConfig::OutputMode::discard) {
                int devnull = ::open("/dev/null", O_WRONLY);
                if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
            }

            // Resolve the program to an absolute path BEFORE chdir.
            std::string program_path = params.resolved_program.string();
            if (program_path[0] != '/') {
                auto abs = std::filesystem::absolute(params.resolved_program);
                program_path = abs.string();
            }

            if (!params.working_dir.empty())
                (void)::chdir(params.working_dir.c_str());

            // Own pgrp only when signalable. Must happen before execve so
            // the inheritance is visible to the target.
            if (params.signalable)
                ::setpgid(0, 0);

            std::vector<const char*> argv;
            argv.push_back(program_path.c_str());
            for (auto& a : params.args)
                argv.push_back(a.c_str());
            argv.push_back(nullptr);

            std::vector<const char*> envp;
            for (auto& entry : params.env_entries)
                envp.push_back(entry.c_str());
            envp.push_back(nullptr);

            ::execve(program_path.c_str(),
                     const_cast<char* const*>(argv.data()),
                     const_cast<char* const*>(envp.data()));

            // execve failed — report errno to supervisor via the CLOEXEC
            // exec-probe pipe, then exit. (On success the kernel would have
            // closed exec_probe[1] atomically and the supervisor would have
            // seen EOF.)
            int e = errno;
            (void)write_all(exec_probe[1], &e, sizeof(e));
            _exit(127);
        }

        // ── Supervisor (parent of grandchild) ──
        // Close the grandchild-side stdio ends we no longer need.
        if (stdin_pipe[0]  >= 0) ::close(stdin_pipe[0]);
        if (stdout_pipe[1] >= 0) ::close(stdout_pipe[1]);
        if (stderr_pipe[1] >= 0) ::close(stderr_pipe[1]);
        // Stdio library-side ends belong to the library, not us.
        if (stdin_pipe[1]  >= 0) ::close(stdin_pipe[1]);
        if (stdout_pipe[0] >= 0) ::close(stdout_pipe[0]);
        if (stderr_pipe[0] >= 0) ::close(stderr_pipe[0]);
        // Don't need the exec-probe write end.
        ::close(exec_probe[1]);

        supervisor_main(grandchild,
                        info_pipe[1],
                        lifecycle_pipe[0],
                        release_pipe[0],
                        exec_probe[0],
                        params.signalable);
        // supervisor_main is [[noreturn]]; unreachable.
    }

    // ── Library (parent of supervisor) ──
    // Close the supervisor/grandchild sides of every pipe.
    ::close(info_pipe[1]);       info_pipe[1] = -1;
    ::close(lifecycle_pipe[0]);  lifecycle_pipe[0] = -1;
    ::close(release_pipe[0]);    release_pipe[0] = -1;
    // The exec-probe pipe is between supervisor and grandchild — we don't
    // use it in the library at all.
    ::close(exec_probe[0]);
    ::close(exec_probe[1]);

    if (stdin_pipe[0]  >= 0) ::close(stdin_pipe[0]);
    if (stdout_pipe[1] >= 0) ::close(stdout_pipe[1]);
    if (stderr_pipe[1] >= 0) ::close(stderr_pipe[1]);

    // Read message 1 (spawn result) synchronously.
    unsigned char tag = 0;
    if (!read_all(info_pipe[0], &tag, 1)) {
        // Supervisor died before writing — clean up.
        ::close(info_pipe[0]);
        ::close(lifecycle_pipe[1]);
        ::close(release_pipe[1]);
        if (stdin_pipe[1]  >= 0) ::close(stdin_pipe[1]);
        if (stdout_pipe[0] >= 0) ::close(stdout_pipe[0]);
        if (stderr_pipe[0] >= 0) ::close(stderr_pipe[0]);
        (void)::waitpid(supervisor, nullptr, 0);
        return std::unexpected(SpawnError{SpawnError::platform_error, EIO});
    }

    if (tag == 1) {
        // Spawn error — read the errno, reap the supervisor, return error.
        int native_err = 0;
        (void)read_all(info_pipe[0], &native_err, sizeof(native_err));
        ::close(info_pipe[0]);
        ::close(lifecycle_pipe[1]);
        ::close(release_pipe[1]);
        if (stdin_pipe[1]  >= 0) ::close(stdin_pipe[1]);
        if (stdout_pipe[0] >= 0) ::close(stdout_pipe[0]);
        if (stderr_pipe[0] >= 0) ::close(stderr_pipe[0]);
        (void)::waitpid(supervisor, nullptr, 0);
        auto kind = (native_err == ENOENT) ? SpawnError::command_not_found
                  : (native_err == EACCES) ? SpawnError::permission_denied
                  : SpawnError::platform_error;
        return std::unexpected(SpawnError{kind, native_err});
    }

    // tag == 0 (spawn ok): read target PID.
    pid_t target_pid = -1;
    if (!read_all(info_pipe[0], &target_pid, sizeof(target_pid))) {
        ::close(info_pipe[0]);
        ::close(lifecycle_pipe[1]);
        ::close(release_pipe[1]);
        if (stdin_pipe[1]  >= 0) ::close(stdin_pipe[1]);
        if (stdout_pipe[0] >= 0) ::close(stdout_pipe[0]);
        if (stderr_pipe[0] >= 0) ::close(stderr_pipe[0]);
        (void)::waitpid(supervisor, nullptr, 0);
        return std::unexpected(SpawnError{SpawnError::platform_error, EIO});
    }

    // Stdin writer thread — same as before.
    if (stdin_pipe[1] >= 0 && params.stdin_mode == CommandConfig::StdinMode::content) {
        std::thread([content = std::move(params.stdin_content), fd = stdin_pipe[1]] {
            (void)::write(fd, content.data(), content.size());
            ::close(fd);
        }).detach();
    } else if (stdin_pipe[1] >= 0 && params.stdin_mode == CommandConfig::StdinMode::file) {
        std::thread([path = std::move(params.stdin_path), fd = stdin_pipe[1]] {
            int file = ::open(path.c_str(), O_RDONLY);
            if (file >= 0) {
                char buf[4096];
                ssize_t n;
                while ((n = ::read(file, buf, sizeof(buf))) > 0)
                    (void)::write(fd, buf, n);
                ::close(file);
            }
            ::close(fd);
        }).detach();
    } else if (stdin_pipe[1] >= 0) {
        ::close(stdin_pipe[1]);
    }

    impl->supervisor_pid = supervisor;
    impl->target_pid     = target_pid;
    impl->stdout_fd      = stdout_pipe[0];
    impl->stderr_fd      = stderr_pipe[0];
    impl->info_fd        = info_pipe[0];
    impl->lifecycle_fd   = lifecycle_pipe[1];
    impl->release_fd     = release_pipe[1];

    if (impl->stdout_fd >= 0 || impl->stderr_fd >= 0) {
        UnixProcessImpl* raw = impl.get();
        impl->pipe_reader_thread = std::thread([raw] { raw->read_pipes(); });
    }

    return impl;
}

}  // namespace collab::process::detail
