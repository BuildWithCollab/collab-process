#include "../platform.hpp"
#include "../running_process_impl.hpp"

#include <windows.h>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace collab::process::detail {

void reset_console_for_interactive();  // console.cpp

// Windows implementation of RunningProcess::Impl
struct Win32ProcessImpl : RunningProcess::Impl {
    HANDLE process_handle = nullptr;
    HANDLE job_handle = nullptr;
    int process_id = 0;

    // Set before CreateProcess, tracks whether CREATE_NEW_PROCESS_GROUP was
    // applied. GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid) only has a
    // valid target when pid names a process-group leader, and Windows does
    // *not* reliably return zero for non-leaders — the call can succeed
    // while delivering the signal nowhere. Gate on this to get honest
    // return values.
    bool has_own_group = false;

    // Pipes for captured output
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;

    // Stdin writer thread — joined before reading pipes or destroying
    std::thread stdin_thread;

    // Background pipe reader — started at spawn time when we own at least
    // one read pipe. Fires on_stdout/on_stderr callbacks live and appends
    // to stdout_content / stderr_content as data arrives, so users can
    // observe output from a spawn()ed process without calling wait().
    std::thread pipe_reader_thread;

    // Callbacks
    collab::process::move_only_function<void(std::string_view)> on_stdout;
    collab::process::move_only_function<void(std::string_view)> on_stderr;

    // Captured output (populated by wait)
    std::string stdout_content;
    std::string stderr_content;
    bool waited = false;

    void join_stdin_thread() {
        if (stdin_thread.joinable())
            stdin_thread.join();
    }

    void join_pipe_reader() {
        if (pipe_reader_thread.joinable())
            pipe_reader_thread.join();
    }

    ~Win32ProcessImpl() override {
        // Ensure I/O threads have finished before closing handles — the
        // reader's ReadFile must complete (via EOF when the process exits
        // or cancellation in release_for_detach) before we tear down.
        join_stdin_thread();
        join_pipe_reader();
        if (stdout_read) CloseHandle(stdout_read);
        if (stderr_read) CloseHandle(stderr_read);
        if (process_handle) CloseHandle(process_handle);
        if (job_handle) CloseHandle(job_handle);
    }

    auto pid() const -> int override { return process_id; }

    auto is_alive() const -> bool override {
        if (!process_handle) return false;
        DWORD exit_code;
        GetExitCodeProcess(process_handle, &exit_code);
        return exit_code == STILL_ACTIVE;
    }

    void read_pipes() {
        // Read stdout
        auto read_handle = [](HANDLE h, std::string& out,
                              collab::process::move_only_function<void(std::string_view)>& cb) {
            if (!h) return;
            char buf[4096];
            DWORD bytes_read;
            while (ReadFile(h, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
                std::string_view chunk(buf, bytes_read);
                if (cb) cb(chunk);
                out.append(chunk);
            }
        };

        // If we have both stdout and stderr pipes, read them concurrently
        // to avoid deadlock
        if (stdout_read && stderr_read) {
            std::thread stderr_thread([&] {
                read_handle(stderr_read, stderr_content, on_stderr);
            });
            read_handle(stdout_read, stdout_content, on_stdout);
            stderr_thread.join();
        } else {
            read_handle(stdout_read, stdout_content, on_stdout);
            read_handle(stderr_read, stderr_content, on_stderr);
        }
    }

    auto wait() -> std::expected<Result, SpawnError> override {
        if (!process_handle)
            return std::unexpected(SpawnError{SpawnError::platform_error, 0});

        if (!waited) {
            join_stdin_thread();
            WaitForSingleObject(process_handle, INFINITE);
            // Reader thread (if any) exits once the child closes its
            // write-ends of the pipes, which happens on process exit.
            join_pipe_reader();
            waited = true;
        }

        DWORD exit_code = 0;
        GetExitCodeProcess(process_handle, &exit_code);

        // Trim trailing whitespace
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
        };
        trim(stdout_content);
        trim(stderr_content);

        return Result{
            .stdout_content = std::move(stdout_content),
            .stderr_content = std::move(stderr_content),
            .exit_code = static_cast<int>(exit_code),
            .timed_out = false,
        };
    }

    // Public wait_for: poll only, no kill. Returns nullopt on timeout.
    // Does NOT start a pipe reader — if the process fills the pipe buffer
    // and blocks, wait_for will still return after the timeout.
    auto wait_for(std::chrono::milliseconds timeout) -> std::optional<Result> override {
        if (!process_handle)
            return std::nullopt;

        DWORD wait_ms = static_cast<DWORD>(timeout.count());
        DWORD wait_result = WaitForSingleObject(process_handle, wait_ms);

        if (wait_result == WAIT_TIMEOUT)
            return std::nullopt;

        // Process exited — reader thread has seen / will imminently see EOF.
        if (!waited) {
            join_pipe_reader();
            waited = true;
        }

        DWORD exit_code = 0;
        GetExitCodeProcess(process_handle, &exit_code);

        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
        };
        trim(stdout_content);
        trim(stderr_content);

        return Result{
            .stdout_content = std::move(stdout_content),
            .stderr_content = std::move(stderr_content),
            .exit_code = static_cast<int>(exit_code),
            .timed_out = false,
        };
    }

    // Used by run() — kills the process on timeout. The background reader
    // started at spawn time prevents deadlock when the child fills the
    // pipe buffer (child blocks writing, parent blocks waiting).
    auto wait_for_and_kill(std::chrono::milliseconds timeout) -> std::expected<Result, SpawnError> override {
        if (!process_handle)
            return std::unexpected(SpawnError{SpawnError::platform_error, 0});

        DWORD wait_ms = static_cast<DWORD>(timeout.count());
        DWORD wait_result = WaitForSingleObject(process_handle, wait_ms);

        if (wait_result == WAIT_TIMEOUT) {
            // Kill the process tree so pipes close and the reader can finish
            if (job_handle)
                TerminateJobObject(job_handle, 1);
            else
                TerminateProcess(process_handle, 1);
            WaitForSingleObject(process_handle, 5000);

            join_pipe_reader();
            waited = true;

            auto trim = [](std::string& s) {
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                    s.pop_back();
            };
            trim(stdout_content);
            trim(stderr_content);

            return Result{
                .stdout_content = std::move(stdout_content),
                .stderr_content = std::move(stderr_content),
                .exit_code = std::nullopt,
                .timed_out = true,
            };
        }

        // Process exited within timeout — pipe reader finishes naturally
        join_pipe_reader();
        waited = true;

        DWORD exit_code = 0;
        GetExitCodeProcess(process_handle, &exit_code);

        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
        };
        trim(stdout_content);
        trim(stderr_content);

        return Result{
            .stdout_content = std::move(stdout_content),
            .stderr_content = std::move(stderr_content),
            .exit_code = static_cast<int>(exit_code),
            .timed_out = false,
        };
    }

    auto terminate() -> bool override {
        if (!is_alive()) return false;
        // CTRL_BREAK_EVENT's second parameter is a process-group ID. A pid
        // only names a group when that process was spawned with
        // CREATE_NEW_PROCESS_GROUP (i.e. signalable). Without that we have
        // nothing meaningful to target — short-circuit to false.
        if (!has_own_group) return false;
        return GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                        static_cast<DWORD>(process_id)) != 0;
    }

    auto interrupt() -> bool override {
        // No viable mapping on Windows — CTRL_C_EVENT broadcasts to the
        // whole console (would hit the parent too) and is disabled for
        // processes in a new process group per MSDN.
        return false;
    }

    auto kill() -> bool override {
        if (!is_alive()) return false;

        if (job_handle)
            return TerminateJobObject(job_handle, 1) != 0;
        return TerminateProcess(process_handle, 1) != 0;
    }

    void release_for_detach() override {
        // Remove the kill-on-close flag so closing the job handle
        // doesn't kill the child process.
        if (job_handle) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
            // Clear the kill-on-close flag
            SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation,
                                    &job_info, sizeof(job_info));
        }
        // The reader thread would otherwise block on ReadFile until the
        // detached child closes its pipes — possibly never. Cancel pending
        // reads so the thread can exit, then join before we return.
        if (pipe_reader_thread.joinable()) {
            if (stdout_read) CancelIoEx(stdout_read, nullptr);
            if (stderr_read) CancelIoEx(stderr_read, nullptr);
            pipe_reader_thread.join();
        }
    }
};

// Build a command line string with proper quoting
static auto build_command_line(bool needs_cmd_wrapper,
                               const std::filesystem::path& program,
                               const std::vector<std::string>& args) -> std::wstring {
    std::wostringstream cmd;

    if (needs_cmd_wrapper) {
        cmd << L"cmd /c ";
    }

    // Quote the program path
    auto prog_str = program.wstring();
    cmd << L"\"" << prog_str << L"\"";

    for (auto& a : args) {
        cmd << L" ";
        std::wstring wa(a.begin(), a.end());
        bool needs_quote = wa.empty() || wa.find(L' ') != std::wstring::npos
            || wa.find(L'\t') != std::wstring::npos
            || wa.find(L'"') != std::wstring::npos;
        if (needs_quote) {
            cmd << L"\"";
            for (wchar_t c : wa) {
                if (c == L'"') cmd << L"\\\"";
                else cmd << c;
            }
            cmd << L"\"";
        } else {
            cmd << wa;
        }
    }

    return cmd.str();
}

// Build a null-terminated wide environment block for CreateProcessW
static auto build_env_block_wide(const std::vector<std::string>& entries) -> std::wstring {
    std::wstring block;
    for (auto& entry : entries) {
        std::wstring wide(entry.begin(), entry.end());
        block.append(wide);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

auto platform_spawn(SpawnParams params)
    -> std::expected<std::unique_ptr<RunningProcess::Impl>, SpawnError> {

    auto impl = std::make_unique<Win32ProcessImpl>();
    impl->on_stdout = std::move(params.on_stdout);
    impl->on_stderr = std::move(params.on_stderr);
    impl->has_own_group = params.signalable;

    // Interactive: all streams inherit AND child is not signalable — the
    // child shares the parent's console and process group, so Ctrl+C routes
    // naturally. Signalable children get CREATE_NEW_PROCESS_GROUP, which
    // breaks interactive console setup (foreground group tracking etc.).
    bool is_interactive = (params.stdout_mode == CommandConfig::OutputMode::inherit)
        && (params.stderr_mode == CommandConfig::OutputMode::inherit)
        && (params.stdin_mode == CommandConfig::StdinMode::inherit)
        && !params.signalable;

    if (is_interactive)
        reset_console_for_interactive();

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // Stdin pipe
    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    bool pipe_stdin = (params.stdin_mode == CommandConfig::StdinMode::content)
                   || (params.stdin_mode == CommandConfig::StdinMode::file);

    if (pipe_stdin) {
        if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0))
            return std::unexpected(SpawnError{SpawnError::pipe_creation_failed,
                                              static_cast<int>(GetLastError())});
        SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    }

    // Stdout pipe
    HANDLE stdout_read_h = nullptr, stdout_write = nullptr;
    if (params.stdout_mode != CommandConfig::OutputMode::inherit) {
        if (!CreatePipe(&stdout_read_h, &stdout_write, &sa, 0)) {
            if (stdin_read) { CloseHandle(stdin_read); CloseHandle(stdin_write); }
            return std::unexpected(SpawnError{SpawnError::pipe_creation_failed,
                                              static_cast<int>(GetLastError())});
        }
        SetHandleInformation(stdout_read_h, HANDLE_FLAG_INHERIT, 0);
    }

    // Stderr pipe (or merge with stdout)
    HANDLE stderr_read_h = nullptr, stderr_write = nullptr;
    if (params.stderr_merge && stdout_write) {
        // Duplicate stdout write handle for stderr
        DuplicateHandle(GetCurrentProcess(), stdout_write,
                        GetCurrentProcess(), &stderr_write,
                        0, TRUE, DUPLICATE_SAME_ACCESS);
    } else if (params.stderr_mode != CommandConfig::OutputMode::inherit) {
        if (!CreatePipe(&stderr_read_h, &stderr_write, &sa, 0)) {
            if (stdin_read) { CloseHandle(stdin_read); CloseHandle(stdin_write); }
            if (stdout_read_h) { CloseHandle(stdout_read_h); CloseHandle(stdout_write); }
            return std::unexpected(SpawnError{SpawnError::pipe_creation_failed,
                                              static_cast<int>(GetLastError())});
        }
        SetHandleInformation(stderr_read_h, HANDLE_FLAG_INHERIT, 0);
    }

    // NUL handles for discard mode and stdin_closed
    HANDLE nul_write = nullptr;
    HANDLE nul_read = nullptr;
    if (params.stdout_mode == CommandConfig::OutputMode::discard
        || params.stderr_mode == CommandConfig::OutputMode::discard) {
        nul_write = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                                &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (params.stdin_mode == CommandConfig::StdinMode::closed) {
        nul_read = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ,
                               &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    // Setup STARTUPINFO
    STARTUPINFOW si = {};
    si.cb = sizeof(si);

    if (!is_interactive) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = pipe_stdin ? stdin_read
                       : nul_read ? nul_read
                       : GetStdHandle(STD_INPUT_HANDLE);

        if (params.stdout_mode == CommandConfig::OutputMode::capture)
            si.hStdOutput = stdout_write;
        else if (params.stdout_mode == CommandConfig::OutputMode::discard)
            si.hStdOutput = nul_write;
        else
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);

        if (params.stderr_merge)
            si.hStdError = stderr_write ? stderr_write : stdout_write;
        else if (params.stderr_mode == CommandConfig::OutputMode::capture)
            si.hStdError = stderr_write;
        else if (params.stderr_mode == CommandConfig::OutputMode::discard)
            si.hStdError = nul_write;
        else
            si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }

    // Build command line and env block
    auto cmd_line = build_command_line(params.needs_cmd_wrapper,
                                       params.resolved_program, params.args);
    auto env_block = build_env_block_wide(params.env_entries);

    const wchar_t* cwd = params.working_dir.empty()
        ? nullptr
        : params.working_dir.c_str();

    // Creation flags
    DWORD flags = 0;
    // CREATE_NO_WINDOW hides the child's console window — but it also
    // effectively detaches the child's console enough that a parent
    // GenerateConsoleCtrlEvent cannot reach it. Signalable children need
    // that console reachability to receive CTRL_BREAK later, so keep the
    // shared console in that case.
    if (!is_interactive && !params.signalable)
        flags |= CREATE_NO_WINDOW;
    if (params.signalable)
        flags |= CREATE_NEW_PROCESS_GROUP;
    flags |= CREATE_UNICODE_ENVIRONMENT;

    // Create job object for tree kill support
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
        job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &job_info, sizeof(job_info));
    }

    // Spawn
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        params.needs_cmd_wrapper ? nullptr : params.resolved_program.c_str(),
        cmd_line.data(),
        nullptr, nullptr,
        TRUE,
        flags,
        env_block.data(),
        cwd,
        &si, &pi
    );

    // Cleanup write-side handles and NUL handles (child has them now)
    if (stdin_read) CloseHandle(stdin_read);
    if (stdout_write) CloseHandle(stdout_write);
    if (stderr_write) CloseHandle(stderr_write);
    if (nul_write) CloseHandle(nul_write);
    if (nul_read) CloseHandle(nul_read);

    if (!ok) {
        int err = static_cast<int>(GetLastError());
        if (stdin_write) CloseHandle(stdin_write);
        if (stdout_read_h) CloseHandle(stdout_read_h);
        if (stderr_read_h) CloseHandle(stderr_read_h);
        if (job) CloseHandle(job);
        return std::unexpected(SpawnError{SpawnError::platform_error, err});
    }

    // Assign to job object
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);

    CloseHandle(pi.hThread);

    // Write stdin content in a thread to avoid deadlock with stdout reading.
    // Thread is stored in impl and joined before reading pipes or destroying.
    if (stdin_write && params.stdin_mode == CommandConfig::StdinMode::content) {
        impl->stdin_thread = std::thread(
            [content = std::move(params.stdin_content), h = stdin_write] {
                DWORD written;
                WriteFile(h, content.data(),
                          static_cast<DWORD>(content.size()), &written, nullptr);
                CloseHandle(h);
            });
    } else if (stdin_write && params.stdin_mode == CommandConfig::StdinMode::file) {
        impl->stdin_thread = std::thread(
            [path = std::move(params.stdin_path), h = stdin_write] {
                HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    char buf[4096];
                    DWORD bytes_read;
                    while (ReadFile(file, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
                        DWORD written;
                        WriteFile(h, buf, bytes_read, &written, nullptr);
                    }
                    CloseHandle(file);
                }
                CloseHandle(h);
            });
    } else if (stdin_write) {
        CloseHandle(stdin_write);
    }

    impl->process_handle = pi.hProcess;
    impl->job_handle = job;
    impl->process_id = static_cast<int>(pi.dwProcessId);
    impl->stdout_read = stdout_read_h;
    impl->stderr_read = stderr_read_h;

    // Live pipe reader — fulfills the IoCallbacks contract that callbacks
    // fire "as data arrives" (not "when wait() happens to drain"). Only
    // start it when the child will actually write to one of our pipes:
    // discard mode creates a pipe too but routes the child's stdout/stderr
    // to NUL, so a reader would block on ReadFile forever.
    bool needs_reader = (params.stdout_mode == CommandConfig::OutputMode::capture)
                     || (params.stderr_mode == CommandConfig::OutputMode::capture);
    if (needs_reader) {
        Win32ProcessImpl* raw = impl.get();
        impl->pipe_reader_thread = std::thread([raw] { raw->read_pipes(); });
    }

    return impl;
}

}  // namespace collab::process::detail
